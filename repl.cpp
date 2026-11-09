// `eskiuc repl`: an interactive session over the LLVM ORC JIT.
//
// The session keeps the source of every accepted declaration input (functions, types,
// imports, :load files) and the REPL globals (`let x = ...` at the prompt). Each new input
// re-parses that state plus the input, so the type checker sees the whole program, and a
// later declaration of a name replaces the earlier one. A statement or expression input
// becomes the body of a fresh `void __repl_N()`; its top-level variables become globals
// (assigned by the wrapper, since a global initializer must be constant) and the value of
// a final expression is stored in a global `__repl_val_N` and printed by type.
//
// Every input is compiled into its own LLVM module and added to one JIT. A symbol the JIT
// already holds is turned into a declaration in later modules, so its code and its state
// (a global's value) are shared; a function or global the input redeclares gets a new
// version (`f` -> `f.v2`) that later modules refer to. Code compiled earlier keeps the
// version it was linked against.

#include "repl.h"
#include "main_support.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "sema/type_checker.h"
#include "sema/async_transform.h"
#include "codegen/codegen.h"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <cstring>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

#include <cxxabi.h>
#if !defined(_WIN32)
#include <csetjmp>
#include <csignal>
extern "C" int __gxx_personality_v0(...);
#else
#include <cstdarg>
// mingw: the SEH personality, and the C99 printf family the stdlib calls on Windows.
extern "C" int __gxx_personality_seh0(...);
extern "C" int __mingw_vsnprintf(char*, size_t, const char*, va_list);
#endif

namespace {

const char* kReplFile = "<repl>";
bool isReplFile(const std::string& f) { return f.rfind("<repl", 0) == 0; }
using Layout = CodeGen::ReplLayout;

// Collects what the compiler writes to std::cerr while alive, so a speculative parse or
// type check can be retried another way before anything is shown.
class CerrCapture {
public:
    CerrCapture() : old(std::cerr.rdbuf(buf.rdbuf())) {}
    ~CerrCapture() { std::cerr.rdbuf(old); }
    std::string text() const { return buf.str(); }
private:
    std::stringstream buf;
    std::streambuf* old;
};

// The parser state one attempt shares across the session's sources (macros, imports,
// type names), rebuilt from scratch for every attempt.
struct ParseCtx {
    std::map<std::string, Macro> macros;
    std::set<std::string> importedFiles;
    ImportCache cache;
    std::set<std::string> typeNames, genericNames;
    std::vector<std::string> linkLibs;
    ParseCtx() {
        seedPredefinedMacros(macros, "", false);
        cache.stdlibPath = stdlibRoot;
        cache.macros = &macros;
    }
    ParseCtx(const ParseCtx&) = delete;
    ParseCtx& operator=(const ParseCtx&) = delete;
};

// An accepted declaration input (typed at the prompt or loaded from a file).
struct Chunk {
    std::string text;
    std::string file;
    int epoch;
};

// A variable declared at the prompt: a global of the session.
struct ReplGlobal {
    std::string name;
    std::string type;
    int epoch;
    int line = 0, col = 0;
};

enum class Mode { Decls, Stmts };

// How to turn the current input into a program.
struct Plan {
    Mode mode = Mode::Stmts;
    std::string text;
    std::string file = kReplFile;
    // Untyped top-level `let x = E;`: the type to write in, by the let's ordinal.
    std::map<size_t, std::vector<Token>> letTypes;
    bool placeholderLets = false;   // write `int` into the untyped lets (just to parse)
    long probeLet = -1;             // cut the input at this let (ordinal), leaving `(E);`
    std::string valueType;          // capture the final expression into __repl_val_N
};

struct Attempt {
    std::unique_ptr<ParseCtx> ctx;
    std::shared_ptr<Program> program;
    FunctionDecl* wrapper = nullptr;
    std::string entry;                                        // the function the JIT calls
    std::vector<DeclPtr> inputDecls;                          // Decls mode
    std::vector<ReplGlobal> newGlobals;                       // Stmts mode
    bool usesAwait = false;
    bool asyncSupport = false;   // the build holds kAsyncSupport
    std::vector<std::pair<int, int>> letPos;   // where each untyped let's name is
};

std::string versioned(const std::string& base, int v) {
    return v <= 1 ? base : base + ".v" + std::to_string(v);
}

// The session's event loop and the driver of an input that awaits (see Repl::build): the
// input runs as `async void __repl_async_N()`, and `void __repl_N()` hands its future to
// `__repl_drive`, which runs `repl_loop()` until the future completes or nothing is left
// on the loop that could complete it (a watchdog timer checks for pending timers and
// watched descriptors). `__repl_await_ok` tells the REPL which happened.
const char* kAsyncSupport = R"ESK(import <future>;
import <eventloop>;
import <atomic>;
import <time>;
EventLoop* __repl_loop;
int __repl_watch_id = -1;
int __repl_await_ok;
EventLoop* repl_loop() {
    if (__repl_loop == null) { __repl_loop = el_new(64); }
    return __repl_loop;
}
void __repl_watch_fire(EventLoop* lp);
void __repl_arm(EventLoop* lp, int64 deadline) {
    __repl_watch_id = EventLoop_add_timer(lp, deadline, void() { __repl_watch_fire(lp); });
}
void __repl_watch_fire(EventLoop* lp) {
    __repl_watch_id = -1;
    int64 last = -1;
    for (t in 0..lp.tcap) {
        if (lp.timers[t].active != 0 && lp.timers[t].deadline > last) { last = lp.timers[t].deadline; }
    }
    int watched = 0;
    for (d in 0..lp.cap) {
        if (lp.slots[d].active != 0) { watched = 1; }
    }
    if (last < 0 && watched == 0) { EventLoop_stop(lp); return; }
    if (watched != 0) { last = time_monotonic_ms() + 50; }
    __repl_arm(lp, last);
}
void __repl_unwatch(EventLoop* lp) {
    if (__repl_watch_id >= 0) { EventLoop_del_timer(lp, __repl_watch_id); }
    __repl_watch_id = -1;
}
int __repl_drive(FutureHdr* f) {
    EventLoop* lp = repl_loop();
    f.waker = void() { EventLoop_stop(lp); };
    if (atomic_cas(&f.state, PENDING, WAITING)) {
        __repl_arm(lp, time_monotonic_ms());
        try { EventLoop_run(lp); } finally { __repl_unwatch(lp); }
    }
    if (atomic_load(&f.state) == READY) {
        free_closure(f.waker);
        free_closure(f.on_drop);
        free((*void)f);
        return 1;
    }
    Future_drop(f);
    return 0;
}
)ESK";

// Whether an input may need kAsyncSupport: it awaits or names the session's loop.
bool needsAsyncSupport(const std::string& text) {
    static const std::regex word("\\b(await|repl_loop)\\b");
    return std::regex_search(text, word);
}

// --- value printing -------------------------------------------------------------------

std::string quoteText(const char* s, size_t n, char q) {
    std::string out(1, q);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\\': out += "\\\\"; break;
            case 0: out += "\\0"; break;
            default:
                if (c == (unsigned char)q) { out += '\\'; out += (char)c; }
                else if (c < 0x20 || c == 0x7f) {
                    char b[8];
                    snprintf(b, sizeof b, "\\x%02x", c);
                    out += b;
                } else {
                    out += (char)c;
                }
        }
    }
    return out + q;
}

std::string fmtDouble(double d, bool isFloat) {
    char b[64];
    for (int prec : {isFloat ? 6 : 15, isFloat ? 9 : 17}) {
        snprintf(b, sizeof b, "%.*g", prec, d);
        if (isFloat ? (float)strtod(b, nullptr) == (float)d : strtod(b, nullptr) == d) break;
    }
    std::string s = b;
    if (s.find_first_of(".eni") == std::string::npos) s += ".0";
    return s;
}

template <typename T> T readAt(const char* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

long long readInt(const char* p, unsigned size, bool isSigned) {
    switch (size) {
        case 1: return isSigned ? (long long)readAt<int8_t>(p) : (long long)readAt<uint8_t>(p);
        case 2: return isSigned ? (long long)readAt<int16_t>(p) : (long long)readAt<uint16_t>(p);
        case 4: return isSigned ? (long long)readAt<int32_t>(p) : (long long)readAt<uint32_t>(p);
        default: return readAt<long long>(p);
    }
}

std::string formatValue(const Layout& L, const char* p, int depth);

std::string formatElems(const Layout& elem, const char* data, uint64_t n, uint64_t stride, int depth) {
    const uint64_t kMax = 100;
    std::string out = "[";
    for (uint64_t i = 0; i < n && i < kMax; ++i) {
        if (i) out += ", ";
        out += formatValue(elem, data + i * stride, depth + 1);
    }
    if (n > kMax) out += ", ...";
    return out + "]";
}

std::string formatValue(const Layout& L, const char* p, int depth) {
    if (depth > 8) return "...";
    switch (L.kind) {
        case Layout::Int: return std::to_string(readInt(p, L.size, true));
        case Layout::UInt:
            if (L.size == 8) return std::to_string(readAt<unsigned long long>(p));
            return std::to_string(readInt(p, L.size, false));
        case Layout::Bool: return readAt<uint8_t>(p) ? "true" : "false";
        case Layout::Char: {
            char c = readAt<char>(p);
            return quoteText(&c, 1, '\'');
        }
        case Layout::Float: return fmtDouble(readAt<float>(p), true);
        case Layout::Double: return fmtDouble(readAt<double>(p), false);
        case Layout::CStr: {
            const char* s = readAt<const char*>(p);
            return s ? quoteText(s, std::strlen(s), '"') : "null";
        }
        case Layout::Ptr: {
            const void* v = readAt<const void*>(p);
            if (!v) return "null";
            char b[32];
            snprintf(b, sizeof b, "0x%llx", (unsigned long long)(uintptr_t)v);
            return b;
        }
        case Layout::Enum: {
            long long v = readInt(p, 4, true);
            auto it = L.enumNames.find(v);
            return it != L.enumNames.end() ? it->second : std::to_string(v);
        }
        case Layout::Variant: {
            long long tag = readInt(p, 4, true);
            auto it = L.alts.find(tag);
            if (it == L.alts.end()) return "<" + L.name + ">";
            std::string out = it->second.name;
            if (!it->second.payload.empty()) {
                out += "(";
                for (size_t i = 0; i < it->second.payload.size(); ++i) {
                    const auto& f = it->second.payload[i];
                    if (i) out += ", ";
                    out += formatValue(*f.layout, p + f.offset, depth + 1);
                }
                out += ")";
            }
            return out;
        }
        case Layout::Struct: {
            std::string out = L.name + "{";
            for (size_t i = 0; i < L.fields.size(); ++i) {
                const auto& f = L.fields[i];
                if (i) out += ", ";
                out += f.name + ": " + formatValue(*f.layout, p + f.offset, depth + 1);
            }
            return out + "}";
        }
        case Layout::Str: {
            const char* d = readAt<const char*>(p + L.dataOff);
            long long n = readInt(p + L.lenOff, L.lenSize, true);
            if (!d || n <= 0) return "\"\"";
            return quoteText(d, (size_t)n, '"');
        }
        case Layout::List: {
            const char* d = readAt<const char*>(p + L.dataOff);
            long long n = readInt(p + L.lenOff, L.lenSize, true);
            if (!d || n <= 0) return "[]";
            return formatElems(*L.elem, d, (uint64_t)n, L.stride, depth);
        }
        case Layout::Array:
            return formatElems(*L.elem, p, L.count, L.stride, depth);
        case Layout::Slice: {
            const char* d = readAt<const char*>(p + L.dataOff);
            long long n = readInt(p + L.lenOff, L.lenSize, true);
            if (!d || n <= 0) return "[]";
            return formatElems(*L.elem, d, (uint64_t)n, L.stride, depth);
        }
        case Layout::Opaque: break;
    }
    return "<" + L.name + ">";
}

// The thrown value of an Eskiu exception: { int64 size, char* type, payload }.
std::string describeException(void* ex) {
    if (!ex) return "";
    const char* p = (const char*)ex;
    const char* type = readAt<const char*>(p + 8);
    if (!type) return "";
    std::string t = type, out = t;
    const char* pay = p + 16;
    // The same primitives as a compiled program's uncaught-exception report.
    if (t == "int" || t == "int32") out += " " + std::to_string(readAt<int32_t>(pay));
    else if (t == "int8") out += " " + std::to_string(readAt<int8_t>(pay));
    else if (t == "int16") out += " " + std::to_string(readAt<int16_t>(pay));
    else if (t == "int64") out += " " + std::to_string(readAt<long long>(pay));
    else if (t == "uint" || t == "uint32") out += " " + std::to_string(readAt<uint32_t>(pay));
    else if (t == "uint8") out += " " + std::to_string(readAt<uint8_t>(pay));
    else if (t == "uint16") out += " " + std::to_string(readAt<uint16_t>(pay));
    else if (t == "uint64") out += " " + std::to_string(readAt<unsigned long long>(pay));
    else if (t == "double" || t == "float") {
        char b[64];
        std::snprintf(b, sizeof b, " %g", t == "float" ? (double)readAt<float>(pay) : readAt<double>(pay));
        out += b;
    }
    else if (t == "bool") out += readAt<uint8_t>(pay) ? " true" : " false";
    else if (t == "char") out += std::string(" '") + readAt<char>(pay) + "'";
    else if (t == "string") {
        const char* s = readAt<const char*>(pay);
        if (s) out += " " + quoteText(s, std::strlen(s), '"');
    }
    return out;
}

// --- running an input -----------------------------------------------------------------

#if !defined(_WIN32)
sigjmp_buf gCrashJmp;
volatile sig_atomic_t gCrashSig = 0;

void onCrash(int sig) {
    gCrashSig = sig;
    siglongjmp(gCrashJmp, 1);
}
#endif

// Call the compiled input. A crash (a bad pointer, a trap) returns the signal number instead
// of ending the session; 0 when it returned normally. Exceptions pass through to the caller.
int callGuarded(void (*fn)()) {
#if defined(_WIN32)
    fn();
    return 0;
#else
    static thread_local std::vector<char> altStack;
    if (altStack.empty()) {
        altStack.resize(1 << 16);
        stack_t ss{};
        ss.ss_sp = altStack.data();
        ss.ss_size = altStack.size();
        sigaltstack(&ss, nullptr);
    }
    const int sigs[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
    struct sigaction sa{}, old[5];
    sa.sa_handler = onCrash;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_ONSTACK | SA_NODEFER;
    for (int i = 0; i < 5; ++i) sigaction(sigs[i], &sa, &old[i]);
    int caught = 0;
    if (sigsetjmp(gCrashJmp, 1) == 0) fn();
    else caught = gCrashSig;
    for (int i = 0; i < 5; ++i) sigaction(sigs[i], &old[i], nullptr);
    return caught;
#endif
}

// --- input scanning -------------------------------------------------------------------

// Bracket depth of `text` outside string/char literals and comments: the reader keeps
// reading lines while it is positive.
// Whether `text` is an unfinished input: it parses neither as top-level declarations
// nor as statements, and the parse fails at its very end (as Python's REPL decides).
// `1 +`, `int sq(int n)`, `do { ... }` without its `while`: wait for more lines. An
// error before the end means the input is complete and wrong: it is sent so the
// diagnostic shows.
bool incompleteInput(const std::string& text) {
    // A preprocessor conditional still open (`#ifdef X` without its `#endif`).
    {
        static const std::regex open(R"(^\s*#\s*(if|ifdef|ifndef)\b)"), close(R"(^\s*#\s*endif\b)");
        int depth = 0;
        std::istringstream in(text);
        for (std::string l; std::getline(in, l);) {
            if (std::regex_search(l, open)) ++depth;
            else if (std::regex_search(l, close)) --depth;
        }
        if (depth > 0) return true;
    }
    // A trailing `?` or `:` starts the arms of a `?:` (or labels the loop on the next
    // line); postfix `?` propagation has no function to return from at the prompt.
    std::string t = text;
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    if (!t.empty() && (t.back() == '?' || (t.back() == ':' && (t.size() < 2 || t[t.size() - 2] != ':'))))
        return true;
    auto probe = [](const std::string& src, int& errLine, int& errCol, int& eofLine, int& eofCol) {
        std::map<std::string, Macro> macros;
        std::streambuf* errBuf = std::cerr.rdbuf();
        std::ostringstream lexQuiet;
        std::cerr.rdbuf(lexQuiet.rdbuf());
        struct Restore { std::streambuf* b; ~Restore() { std::cerr.rdbuf(b); } } restore{errBuf};
        Lexer lexer(src, &macros, "<probe>", nullptr);
        std::vector<Token> toks;
        Token t = lexer.next_token();
        for (; t.type != TokenType::EOF_TOKEN; t = lexer.next_token()) toks.push_back(t);
        if (lexer.hadError) return true;   // a lexical error is reported on submit
        eofLine = t.line; eofCol = t.column;
        // Imports are resolved on submit; the probe only checks the syntax around them.
        std::vector<Token> kept;
        for (size_t i = 0; i < toks.size(); ++i) {
            if (toks[i].type == TokenType::IMPORT) {
                while (i < toks.size() && toks[i].type != TokenType::SEMICOLON) ++i;
                continue;
            }
            kept.push_back(toks[i]);
        }
        kept.push_back(t);
        Parser parser(kept);
        parser.filename = "<probe>";
        Parser::firstErrorLine() = 0;
        std::streambuf* saved = std::cerr.rdbuf();
        std::ostringstream quiet;
        std::cerr.rdbuf(quiet.rdbuf());
        bool ok = false;
        try { ok = parser.parse() != nullptr; } catch (...) { ok = false; }
        std::cerr.rdbuf(saved);
        errLine = Parser::firstErrorLine();
        errCol = Parser::firstErrorCol();
        return ok;
    };
    int el = 0, ec = 0, fl = 0, fc = 0;
    if (probe(text, el, ec, fl, fc)) return false;
    bool declAtEnd = el > fl || (el == fl && ec >= fc);
    // As statements: the body of a function, as typed and with a `;` after it, so a
    // bare final expression (`x`, the usual REPL input) is complete too.
    int lines = (int)std::count(text.begin(), text.end(), '\n') + 1;
    if (probe("void __probe() {\n" + text + "\n}", el, ec, fl, fc)) return false;
    bool stmtAtEnd = el >= lines + 2;
    if (probe("void __probe() {\n" + text + "\n;}", el, ec, fl, fc)) return false;
    stmtAtEnd = stmtAtEnd && el >= lines + 2;
    return declAtEnd || stmtAtEnd;
}

// An operator a line cannot start with (`? 10`, `: 20`, `.len`, `&& b`): it continues an
// expression of the line before, which already ran. Returns it, or "".
std::string danglingOperator(const std::string& t) {
    static const char* ops[] = {"&&", "||", "==", "!=", "<=", ">=", "->", "<<", ">>",
                                "?", ".", "<", ">", "|", "^", "%", "="};
    if (t.empty()) return "";
    if (t[0] == ':') return (t.size() == 1 || !std::isalpha((unsigned char)t[1])) ? ":" : "";
    if (t[0] == '/' && t.size() > 1 && (t[1] == '/' || t[1] == '*')) return "";
    if (t[0] == '/') return "/";
    for (const char* op : ops)
        if (t.compare(0, std::strlen(op), op) == 0) return op;
    return "";
}

// The first word of `text` (`if`, `try`, ...), after blanks and comments.
std::string firstWord(const std::string& text) {
    static const std::regex lead(R"(^(?:\s|//[^\n]*\n|/\*[\s\S]*?\*/)*([A-Za-z_]\w*))");
    std::smatch m;
    return std::regex_search(text, m, lead) ? m[1].str() : "";
}

// A header whose body starts on the next line (`int sq(int n)`, `for (i in 0..3)`,
// `struct P`, `} else`), as written in the style that puts `{` on its own line: the
// reader waits for the body instead of sending an incomplete input.
bool awaitsBody(const std::string& text) {
    std::string t = text;
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    size_t ls = t.find_last_of('\n');
    std::string last = t.substr(ls == std::string::npos ? 0 : ls + 1);
    size_t b = last.find_first_not_of(" \t");
    if (b == std::string::npos) return false;
    last = last.substr(b);
    static const std::regex control(R"(^(\}\s*)?(if|else\s+if|for|while|switch|match)\b.*\)$)");
    static const std::regex bare(R"(^(\}\s*)?(else|do)$)");
    static const std::regex typeHead(R"(^(packed\s+)?(struct|union|enum|interface)\s+\w+(\s*<[^>]*>)?$)");
    static const std::regex fnHead(
        R"(^(?!(await|return|throw|defer|errdefer|case|else|sizeof|free_closure|thread_join)\b)((async|must_use|static|extern|deprecated\("[^"]*"\))\s+)*[\w*<>\[\],?. ]*[\w*>\]?]\s+\*?\w+\s*(<[^>]*>)?\s*\([^;{}]*\)$)");
    // A lambda whose body comes next: `= int(int v)`, `= void()`, `= *Node(*Node a, int k)`
    // (parameters with names, or none after a type keyword), which a call never has.
    static const std::regex lambdaHead(
        R"(=\s*(([\w*<>\[\]?]+\s*\(\s*[A-Za-z_*][\w*<>\[\]?]*\s+[A-Za-z_]\w*\s*(,\s*[A-Za-z_*][\w*<>\[\]?]*\s+[A-Za-z_]\w*\s*)*\))|((\*+\w+|int\w*|uint\w*|float|double|bool|char|string|void)\s*\(\s*\)))\s*$)");
    return std::regex_match(last, control) || std::regex_match(last, bare) ||
           std::regex_match(last, typeHead) || std::regex_match(last, fnHead) ||
           std::regex_search(last, lambdaHead);
}

// A complete `if` or `try`: an `else`, `catch` or `finally` may follow on the next line,
// so it is held for one line (a blank line, or any other line, runs it).
bool ifAwaitsElse(const std::string& text) {
    std::string w = firstWord(text);
    return w == "if" || w == "try";
}

// Whether `line` continues a held `if`/`try` (`else`, `catch`, `finally`).
bool continuesHeld(const std::string& line) {
    std::string w = firstWord(line);
    if (w.empty()) {
        size_t b = line.find_first_not_of(" \t");
        return b != std::string::npos && line[b] == '}' && firstWord(line.substr(b + 1)) == "else";
    }
    return w == "else" || w == "catch" || w == "finally";
}

int bracketDepth(const std::string& text) {
    int depth = 0;
    bool block = false, str = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        char n = i + 1 < text.size() ? text[i + 1] : 0;
        if (block) {
            if (c == '*' && n == '/') { block = false; ++i; }
            continue;
        }
        if (c == '/' && n == '/') { while (i < text.size() && text[i] != '\n') ++i; continue; }
        if (c == '/' && n == '*') { block = true; ++i; continue; }
        // A string literal may span lines (a char literal may not): one still open at
        // the end keeps the input going.
        if (c == '"' || c == '\'') {
            for (++i; i < text.size() && text[i] != c && (c == '"' || text[i] != '\n'); ++i)
                if (text[i] == '\\') ++i;
            if (c == '"' && i >= text.size()) str = true;
            continue;
        }
        if (c == '(' || c == '{' || c == '[') ++depth;
        else if (c == ')' || c == '}' || c == ']') --depth;
    }
    return block || str ? std::max(depth, 1) : depth;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// The first diagnostic's position in captured compiler output, for choosing between the
// declaration and the statement reading of an input that is neither.
std::pair<int, int> firstErrorPos(const std::string& errs) {
    std::string key = std::string(kReplFile) + ":";
    size_t at = errs.find(key);
    if (at == std::string::npos) return {0, 0};
    int line = 0, col = 0;
    if (sscanf(errs.c_str() + at + key.size(), "%d:%d", &line, &col) < 1) return {0, 0};
    return {line, col};
}

class Repl {
public:
    explicit Repl(bool interactive) : interactive(interactive) {}
    int run(const std::vector<std::string>& files);

private:
    bool interactive;
    std::vector<Chunk> chunks;
    std::vector<ReplGlobal> globals;
    int epoch = 0;
    std::unique_ptr<llvm::orc::LLJIT> jit;
    // Per symbol the JIT defines: its current version and the input that defined it.
    std::map<std::string, int> version, defEpoch;
    // The input that last declared a name, and a prefix (`Point_` for a struct's methods,
    // `id_` for a generic function's instances, "" for every function after a type is
    // redefined): a symbol defined before that is stale and gets a new version.
    std::map<std::string, int> declEpoch, prefixEpoch;
    std::set<std::string> loadedLibs;
    bool asyncSupport = false;   // kAsyncSupport is part of the session

    bool initJit();
    void reset();
    bool lexInto(ParseCtx& ctx, const std::string& text, const std::string& file,
                 std::vector<Token>& toks);
    std::shared_ptr<Program> parseTokens(ParseCtx& ctx, std::vector<Token> toks,
                                         const std::string& file);
    std::vector<size_t> untypedLets(const std::vector<Token>& toks) const;
    bool build(const Plan& plan, Attempt& a);
    bool typeOfLast(const Attempt& a, const TypeChecker& tc, std::string& semaType,
                    bool& printable) const;
    bool handleInput(const std::string& text, const std::string& file, Mode* forced);
    bool inferLets(Plan& plan, const std::vector<std::pair<int, int>>& letPos);
    bool compileAndRun(Attempt& a, const std::string& valueSemaType,
                       const std::string& valueDisplay);
    void processModule(llvm::Module& m, std::map<std::string, int>& ver,
                       std::map<std::string, int>& defEp);
    bool isStale(const std::string& sym, bool isFunction) const;
    void commit(const Plan& plan, Attempt& a);
    void typeCommand(const std::string& expr);
    void loadCommand(const std::string& path);
    void loadLibs(const std::vector<std::string>& libs);
};

bool Repl::initJit() {
    auto jtmb = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!jtmb) {
        llvm::errs() << "error: " << llvm::toString(jtmb.takeError()) << "\n";
        return false;
    }
    auto j = llvm::orc::LLJITBuilder().setJITTargetMachineBuilder(std::move(*jtmb)).create();
    if (!j) {
        llvm::errs() << "error: " << llvm::toString(j.takeError()) << "\n";
        return false;
    }
    jit = std::move(*j);
    auto& jd = jit->getMainJITDylib();
    auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
        jit->getDataLayout().getGlobalPrefix());
    if (!gen) {
        llvm::errs() << "error: " << llvm::toString(gen.takeError()) << "\n";
        return false;
    }
    jd.addGenerator(std::move(*gen));
    // The C++ exception runtime the compiled code throws through: taken from this binary,
    // where it is linked in (statically or not), so a lookup never depends on how the
    // compiler itself was linked.
    llvm::orc::SymbolMap eh;
    auto add = [&](const char* name, const void* addr) {
        eh[jit->mangleAndIntern(name)] = {llvm::orc::ExecutorAddr::fromPtr(addr),
                                          llvm::JITSymbolFlags::Exported};
    };
    add("__cxa_allocate_exception", (const void*)&__cxxabiv1::__cxa_allocate_exception);
    add("__cxa_free_exception", (const void*)&__cxxabiv1::__cxa_free_exception);
    add("__cxa_throw", (const void*)&__cxxabiv1::__cxa_throw);
    add("__cxa_begin_catch", (const void*)&__cxxabiv1::__cxa_begin_catch);
    add("__cxa_end_catch", (const void*)&__cxxabiv1::__cxa_end_catch);
    add("__cxa_rethrow", (const void*)&__cxxabiv1::__cxa_rethrow);
#if !defined(_WIN32)
    add("__gxx_personality_v0", (const void*)&__gxx_personality_v0);
#else
    // A Windows executable exports nothing, so what the stdlib reaches beyond the C
    // runtime DLLs comes from this binary too.
    add("__gxx_personality_seh0", (const void*)&__gxx_personality_seh0);
    add("__mingw_vsnprintf", (const void*)&__mingw_vsnprintf);
#endif
    add("_ZTIPv", (const void*)&typeid(void*));
    if (auto err = jd.define(llvm::orc::absoluteSymbols(std::move(eh)))) {
        llvm::errs() << "error: " << llvm::toString(std::move(err)) << "\n";
        return false;
    }
    return true;
}

void Repl::reset() {
    chunks.clear();
    globals.clear();
    asyncSupport = false;
    version.clear();
    defEpoch.clear();
    declEpoch.clear();
    prefixEpoch.clear();
    jit.reset();
    initJit();
}

void Repl::loadLibs(const std::vector<std::string>& libs) {
    for (const auto& l : libs) {
        if (!loadedLibs.insert(l).second) continue;
        for (const std::string& f : {"lib" + l + ".so", "lib" + l + ".so.6", "lib" + l + ".dylib",
                                     l + ".dll"}) {
            std::string err;
            if (!llvm::sys::DynamicLibrary::LoadLibraryPermanently(f.c_str(), &err)) break;
        }
    }
}

bool Repl::lexInto(ParseCtx& ctx, const std::string& text, const std::string& file,
                   std::vector<Token>& toks) {
    PPImportHook hook = ctx.cache.hookFor(isReplFile(file) ? "." : dirOf(file));
    Lexer lexer(text, &ctx.macros, file, &hook);
    for (Token t = lexer.next_token(); t.type != TokenType::EOF_TOKEN; t = lexer.next_token())
        toks.push_back(t);
    return !lexer.hadError;
}

std::shared_ptr<Program> Repl::parseTokens(ParseCtx& ctx, std::vector<Token> toks,
                                           const std::string& file) {
    int line = toks.empty() ? 1 : toks.back().line;
    toks.push_back(Token(TokenType::EOF_TOKEN, "", line, 0));
    Parser parser(toks);
    parser.filename = file;
    parser.stdlibPath = stdlibRoot;
    parser.basedir = isReplFile(file) ? "." : dirOf(file);
    parser.macros = &ctx.macros;
    parser.importedFiles = &ctx.importedFiles;
    parser.importCache = &ctx.cache;
    parser.sharedTypeNames = &ctx.typeNames;
    parser.sharedGenericNames = &ctx.genericNames;
    auto prog = parser.parse();
    if (prog) ctx.linkLibs.insert(ctx.linkLibs.end(), prog->linkLibs.begin(), prog->linkLibs.end());
    return prog;
}

// Top-level `let NAME = E;` statements (no type written): the REPL infers their type.
std::vector<size_t> Repl::untypedLets(const std::vector<Token>& toks) const {
    std::vector<size_t> out;
    int depth = 0;
    for (size_t i = 0; i < toks.size(); ++i) {
        TokenType t = toks[i].type;
        if (t == TokenType::LPAREN || t == TokenType::LBRACE || t == TokenType::LBRACKET) ++depth;
        else if (t == TokenType::RPAREN || t == TokenType::RBRACE || t == TokenType::RBRACKET) --depth;
        else if (t == TokenType::LET && depth == 0 && i + 2 < toks.size() &&
                 toks[i + 1].type == TokenType::IDENT && toks[i + 2].type == TokenType::EQ &&
                 (i == 0 || toks[i - 1].type == TokenType::SEMICOLON ||
                  toks[i - 1].type == TokenType::RBRACE))
            out.push_back(i);
    }
    return out;
}

bool Repl::build(const Plan& plan, Attempt& a) {
    a.ctx = std::make_unique<ParseCtx>();
    ParseCtx& ctx = *a.ctx;
    struct Item { int epoch; DeclPtr decl; };
    std::vector<Item> items;
    if (asyncSupport || needsAsyncSupport(plan.text)) {
        const std::string file = "<repl-async>";
        std::vector<Token> toks;
        if (!lexInto(ctx, kAsyncSupport, file, toks)) return false;
        auto prog = parseTokens(ctx, toks, file);
        if (!prog) return false;
        for (const auto& d : prog->declarations) {
            d->fromImport = true;
            items.push_back({0, d});
        }
        a.asyncSupport = true;
    }
    for (const auto& c : chunks) {
        // An earlier prompt input is named by its number in diagnostics.
        std::string file = c.file == kReplFile ? "<repl:" + std::to_string(c.epoch) + ">" : c.file;
        std::vector<Token> toks;
        if (!lexInto(ctx, c.text, file, toks)) return false;
        auto prog = parseTokens(ctx, toks, file);
        if (!prog) return false;
        for (const auto& d : prog->declarations) items.push_back({c.epoch, d});
    }
    std::vector<Item> globalItems;
    for (const auto& g : globals) {
        auto v = std::make_shared<VarDecl>(g.name, g.type);
        v->sourceFile = "<repl:" + std::to_string(g.epoch) + ">";
        v->line = g.line;
        v->col = g.col;
        globalItems.push_back({g.epoch, v});
    }

    std::vector<Token> toks;
    if (!lexInto(ctx, plan.text, plan.file, toks)) return false;
    int cur = epoch + 1;
    if (plan.mode == Mode::Decls) {
        auto prog = parseTokens(ctx, toks, plan.file);
        if (!prog) return false;
        a.inputDecls = prog->declarations;
        for (const auto& d : prog->declarations) items.push_back({cur, d});
    } else {
        for (const auto& t : toks)
            if (t.type == TokenType::AWAIT) a.usesAwait = true;
        // The input's statements, with the untyped lets typed (or cut at the probed one).
        std::map<size_t, size_t> letOrd;   // LET token index -> ordinal
        for (size_t li : untypedLets(toks)) {
            size_t n = letOrd.size();
            letOrd[li] = n;
            a.letPos.push_back({toks[li + 1].line, toks[li + 1].column});
        }
        std::vector<Token> body;
        for (size_t i = 0; i < toks.size(); ++i) {
            auto ord = letOrd.find(i);
            if (plan.probeLet >= 0 && ord != letOrd.end() && (long)ord->second == plan.probeLet) {
                // `(E);`: parenthesized, so a lambda is not read as a declaration.
                int depth = 0;
                body.push_back(Token(TokenType::LPAREN, "(", toks[i].line, toks[i].column));
                for (size_t k = i + 3; k < toks.size(); ++k) {
                    TokenType t = toks[k].type;
                    if (t == TokenType::SEMICOLON && depth == 0) break;
                    if (t == TokenType::LPAREN || t == TokenType::LBRACE || t == TokenType::LBRACKET) ++depth;
                    else if (t == TokenType::RPAREN || t == TokenType::RBRACE || t == TokenType::RBRACKET) --depth;
                    body.push_back(toks[k]);
                }
                body.push_back(Token(TokenType::RPAREN, ")", body.back().line, body.back().column));
                body.push_back(Token(TokenType::SEMICOLON, ";", body.back().line, body.back().column));
                break;
            }
            body.push_back(toks[i]);
            auto named = i >= 1 ? letOrd.find(i - 1) : letOrd.end();
            if (named != letOrd.end()) {
                auto lt = plan.letTypes.find(named->second);
                if (lt != plan.letTypes.end()) {
                    body.push_back(Token(TokenType::COLON, ":", toks[i].line, toks[i].column));
                    body.insert(body.end(), lt->second.begin(), lt->second.end());
                } else if (plan.placeholderLets) {
                    body.push_back(Token(TokenType::COLON, ":", toks[i].line, toks[i].column));
                    body.push_back(Token(TokenType::INT, "int", toks[i].line, toks[i].column));
                }
            }
        }
        if (body.empty()) return false;
        // An input that awaits runs as the body of an async function (see kAsyncSupport).
        std::string wname = (a.usesAwait ? "__repl_async_" : "__repl_") + std::to_string(cur);
        auto parseWrapped = [&](bool addSemi) {
            std::vector<Token> b = body;
            if (addSemi)
                b.push_back(Token(TokenType::SEMICOLON, ";", b.back().line,
                                  b.back().column + (int)b.back().value.size()));
            int l0 = b.front().line, c0 = b.front().column;
            std::vector<Token> wrapped;
            if (a.usesAwait) wrapped.push_back(Token(TokenType::ASYNC, "async", l0, c0));
            wrapped.insert(wrapped.end(), {
                Token(TokenType::VOID, "void", l0, c0), Token(TokenType::IDENT, wname, l0, c0),
                Token(TokenType::LPAREN, "(", l0, c0), Token(TokenType::RPAREN, ")", l0, c0),
                Token(TokenType::LBRACE, "{", l0, c0)});
            wrapped.insert(wrapped.end(), b.begin(), b.end());
            wrapped.push_back(Token(TokenType::RBRACE, "}", b.back().line, b.back().column + 1));
            return parseTokens(ctx, wrapped, plan.file);
        };
        // A final expression needs no `;`. One ending in `}` (`Point{1, 2}`, a lambda) is
        // tried with it first, then as a block statement (`if (c) { ... }`).
        std::shared_ptr<Program> prog;
        if (body.back().type == TokenType::SEMICOLON) {
            prog = parseWrapped(false);
        } else if (body.back().type == TokenType::RBRACE) {
            {
                CerrCapture cap;
                prog = parseWrapped(true);
            }
            if (!prog) prog = parseWrapped(false);
        } else {
            prog = parseWrapped(true);
        }
        if (!prog || prog->declarations.empty()) return false;
        a.wrapper = nodeCast<FunctionDecl>(prog->declarations.back().get());
        auto* blk = a.wrapper ? nodeCast<BlockStmt>(a.wrapper->body.get()) : nullptr;
        if (!blk) return false;

        // Top-level variables become globals of the session, assigned here.
        std::vector<BlockItem> kept;
        for (auto& it : blk->items) {
            auto* dp = std::get_if<DeclPtr>(&it);
            auto* vd = dp ? nodeCast<VarDecl>(dp->get()) : nullptr;
            if (!vd || vd->isExtern) { kept.push_back(it); continue; }
            a.newGlobals.push_back({vd->name, vd->type, epoch + 1, vd->line, vd->col});
            // `x = init;`. A brace list is assigned element by element for an array (the
            // global starts zeroed, so omitted elements stay zero as in C) and as a struct
            // literal otherwise.
            std::function<void(ExprPtr, const ExprPtr&, const std::string&)> assign =
                [&](ExprPtr target, const ExprPtr& init, const std::string& ty) {
                auto* al = nodeCast<ArrayLitExpr>(init.get());
                size_t open = ty.find('['), close = ty.find(']', open == std::string::npos ? 0 : open);
                if (al && !ty.empty() && ty.back() == ']' && open != std::string::npos &&
                    close != std::string::npos) {
                    std::string elemTy = ty.substr(0, open) + ty.substr(close + 1);
                    for (size_t k = 0; k < al->elements.size(); ++k) {
                        auto idx = std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT,
                                                                 std::to_string(k));
                        idx->line = init->line;
                        idx->col = init->col;
                        auto el = std::make_shared<IndexExpr>(target, idx);
                        el->line = init->line;
                        el->col = init->col;
                        assign(el, al->elements[k], elemTy);
                    }
                    return;
                }
                ExprPtr value = init;
                if (al) {
                    std::vector<std::pair<std::string, ExprPtr>> inits;
                    for (const auto& e : al->elements) inits.push_back({"", e});
                    value = std::make_shared<StructInitExpr>(ty, inits);
                    value->line = init->line;
                    value->col = init->col;
                }
                auto as = std::make_shared<BinaryExpr>(target, "=", value);
                as->line = vd->line;
                as->col = vd->col;
                auto st = std::make_shared<ExprStmt>(as);
                st->line = vd->line;
                st->col = vd->col;
                kept.push_back(StmtPtr(st));
            };
            if (vd->initializer) {
                auto id = std::make_shared<IdentExpr>(vd->name);
                id->line = vd->line;
                id->col = vd->col;
                assign(id, vd->initializer, vd->type);
            }
        }
        blk->items = kept;
        for (const auto& g : a.newGlobals) {
            auto v = std::make_shared<VarDecl>(g.name, g.type);
            v->sourceFile = kReplFile;
            v->line = g.line;
            v->col = g.col;
            globalItems.push_back({cur, v});
        }
        if (!plan.valueType.empty() && !blk->items.empty()) {
            auto* sp = std::get_if<StmtPtr>(&blk->items.back());
            auto* es = sp ? nodeCast<ExprStmt>(sp->get()) : nullptr;
            if (es) {
                std::string vname = "__repl_val_" + std::to_string(cur);
                auto id = std::make_shared<IdentExpr>(vname);
                id->line = es->line;
                id->col = es->col;
                auto as = std::make_shared<BinaryExpr>(id, "=", es->expr);
                as->line = es->expr->line;
                as->col = es->expr->col;
                es->expr = as;
                auto v = std::make_shared<VarDecl>(vname, plan.valueType);
                v->sourceFile = kReplFile;
                globalItems.push_back({cur, v});
            }
        }
        items.push_back({cur, prog->declarations.back()});
        a.entry = wname;
        if (a.usesAwait) {
            a.entry = "__repl_" + std::to_string(cur);
            std::string driver = "void " + a.entry + "() { __repl_await_ok = __repl_drive((FutureHdr*)" +
                                 wname + "()); }";
            std::vector<Token> dt;
            if (!lexInto(ctx, driver, "<repl-async>", dt)) return false;
            auto dprog = parseTokens(ctx, dt, "<repl-async>");
            if (!dprog || dprog->declarations.empty()) return false;
            items.push_back({cur, dprog->declarations.back()});
        }
    }

    // A later declaration of a name replaces the earlier ones.
    std::map<std::string, int> latest;
    auto note = [&](const Item& it) {
        if (it.decl->fromImport) return;
        int& e = latest[it.decl->name];
        e = std::max(e, it.epoch);
    };
    for (const auto& it : items) note(it);
    for (const auto& it : globalItems) note(it);
    auto live = [&](const Item& it) {
        return it.decl->fromImport || latest[it.decl->name] == it.epoch;
    };
    // Types and other declarations first, then the session's variables, then functions:
    // a function may use any variable declared at the prompt.
    std::vector<DeclPtr> ordered;
    for (const auto& it : items)
        if (!nodeCast<FunctionDecl>(it.decl.get()) && live(it)) ordered.push_back(it.decl);
    for (const auto& it : globalItems)
        if (live(it)) ordered.push_back(it.decl);
    for (const auto& it : items)
        if (nodeCast<FunctionDecl>(it.decl.get()) && live(it)) ordered.push_back(it.decl);
    a.program = std::make_shared<Program>(ordered);
    a.program->linkLibs = ctx.linkLibs;
    return true;
}

// The type of the input's final expression statement, and whether to print it (not an
// assignment, an increment or a call to a variadic C function such as printf).
bool Repl::typeOfLast(const Attempt& a, const TypeChecker& tc, std::string& semaType,
                      bool& printable) const {
    printable = false;
    if (!a.wrapper) return false;
    auto* blk = nodeCast<BlockStmt>(a.wrapper->body.get());
    if (!blk || blk->items.empty()) return false;
    auto* sp = std::get_if<StmtPtr>(&blk->items.back());
    auto* es = sp ? nodeCast<ExprStmt>(sp->get()) : nullptr;
    if (!es || !es->expr) return false;
    const auto& types = tc.expressionTypeMap();
    auto it = types.find(es->expr.get());
    if (it == types.end() || it->second.empty() || it->second == "void" ||
        it->second == "unknown")
        return false;
    semaType = it->second;
    printable = true;
    if (auto* be = nodeCast<BinaryExpr>(es->expr.get()); be && be->op == "=") printable = false;
    if (nodeCast<IncDecExpr>(es->expr.get())) printable = false;
    if (auto* ce = nodeCast<CallExpr>(es->expr.get())) {
        if (auto* id = nodeCast<IdentExpr>(ce->callee.get()))
            for (const auto& d : a.program->declarations)
                if (auto* ex = nodeCast<ExternDecl>(d.get()); ex && ex->name == id->name)
                    for (const auto& p : ex->params)
                        if (p.first == "...") printable = false;
    }
    return true;
}

bool Repl::isStale(const std::string& sym, bool isFunction) const {
    auto de = defEpoch.find(sym);
    int defined = de == defEpoch.end() ? 0 : de->second;
    if (auto d = declEpoch.find(sym); d != declEpoch.end() && d->second > defined) return true;
    for (const auto& p : prefixEpoch)
        if (p.second > defined && sym.compare(0, p.first.size(), p.first) == 0 &&
            (isFunction || !p.first.empty()))
            return true;
    return false;
}

void Repl::processModule(llvm::Module& m, std::map<std::string, int>& ver,
                         std::map<std::string, int>& defEp) {
    for (llvm::GlobalValue& gv : m.global_values()) {
        if (!gv.hasName() || gv.hasLocalLinkage() || gv.hasAppendingLinkage()) continue;
        if (!llvm::isa<llvm::Function>(gv) && !llvm::isa<llvm::GlobalVariable>(gv)) continue;
        std::string base = gv.getName().str();
        if (base.rfind("llvm.", 0) == 0) continue;
        auto vit = version.find(base);
        if (gv.isDeclaration()) {
            if (vit != version.end()) gv.setName(versioned(base, vit->second));
            continue;
        }
        if (vit == version.end()) {
            ver[base] = 1;
            defEp[base] = epoch + 1;
            continue;
        }
        if (isStale(base, llvm::isa<llvm::Function>(gv))) {
            int v = vit->second + 1;
            gv.setName(versioned(base, v));
            ver[base] = v;
            defEp[base] = epoch + 1;
            continue;
        }
        // Already in the JIT: refer to that definition.
        if (auto* go = llvm::dyn_cast<llvm::GlobalObject>(&gv)) go->setComdat(nullptr);
        if (auto* f = llvm::dyn_cast<llvm::Function>(&gv)) {
            f->deleteBody();
        } else if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(&gv)) {
            g->setInitializer(nullptr);
        }
        gv.setLinkage(llvm::GlobalValue::ExternalLinkage);
        gv.setDSOLocal(false);
        gv.setName(versioned(base, vit->second));
    }
}

bool Repl::compileAndRun(Attempt& a, const std::string& valueSemaType,
                         const std::string& valueDisplay) {
    Program* program = a.program.get();
    TypeChecker tc;
    tc.sourceFile = kReplFile;
    if (!tc.check(program)) return false;
    AsyncTransform(&tc.expressionTypeMap(), &tc.instanceArgsMap(), &tc.instanceExprTypeMap())
        .run(program);
    TypeChecker postTc;
    postTc.sourceFile = kReplFile;
    if (!postTc.check(program)) {
        std::cerr << "error: internal: the async lowering produced a program that does not "
                     "type-check" << std::endl;
        return false;
    }
    std::unique_ptr<llvm::LLVMContext> llctx;
    std::unique_ptr<llvm::Module> mod;
    std::shared_ptr<Layout> layout;
    {
        CodeGen cg;
        cg.resolvedExprTypes = &postTc.expressionTypeMap();
        cg.semaInstanceArgs = &postTc.instanceArgsMap();
        try {
            if (!cg.generateCode(a.program)) {
                std::cerr << "error: code generation failed" << std::endl;
                return false;
            }
            if (!valueSemaType.empty()) {
                layout = cg.replLayout(valueSemaType);
                if (layout->kind == Layout::Opaque || layout->kind == Layout::Struct)
                    layout->name = valueDisplay;
            }
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
            return false;
        }
        mod = cg.takeModule(llctx);
    }

    std::map<std::string, int> ver = version, defEp = defEpoch;
    std::map<std::string, int> savedDecl = declEpoch, savedPrefix = prefixEpoch;
    int cur = epoch + 1;
    // The names this input declares: their earlier symbols become stale.
    for (const auto& d : a.inputDecls) {
        if (d->fromImport) continue;
        bool isType = nodeCast<StructDecl>(d.get()) || nodeCast<EnumDecl>(d.get()) ||
                      nodeCast<UnionDecl>(d.get()) || nodeCast<TypeAliasDecl>(d.get()) ||
                      nodeCast<InterfaceDecl>(d.get());
        // A redefined type may change its layout: every function is compiled again, and
        // the variables whose type names it start over (zeroed) in the new layout.
        if (isType && declEpoch.count(d->name)) {
            prefixEpoch[""] = cur;
            std::regex word("\\b" + d->name + "\\b");
            for (const auto& g : program->declarations)
                if (auto* vd = nodeCast<VarDecl>(g.get()))
                    if (!vd->fromImport && std::regex_search(vd->type, word)) declEpoch[vd->name] = cur;
        }
        declEpoch[d->name] = cur;
        if (auto* fd = nodeCast<FunctionDecl>(d.get()); fd && !fd->typeParams.empty())
            prefixEpoch[d->name + "_"] = cur;
        if (nodeCast<StructDecl>(d.get()) || nodeCast<EnumDecl>(d.get()))
            prefixEpoch[d->name + "_"] = cur;
    }
    // A variable declared again with the same type keeps its storage (the input assigns
    // it); with another type it is a new variable.
    for (const auto& g : a.newGlobals) {
        bool same = false;
        for (const auto& old : globals)
            if (old.name == g.name && old.type == g.type) same = true;
        if (!same) declEpoch[g.name] = cur;
    }
    processModule(*mod, ver, defEp);
    mod->setDataLayout(jit->getDataLayout());
    mod->setTargetTriple(jit->getTargetTriple());
    {
        std::string err;
        llvm::raw_string_ostream es(err);
        if (llvm::verifyModule(*mod, &es)) {
            std::cerr << "error: internal: REPL module does not verify:\n" << err << std::endl;
            declEpoch = savedDecl;
            prefixEpoch = savedPrefix;
            return false;
        }
    }
    loadLibs(program->linkLibs);
    if (auto err = jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(llctx)))) {
        std::cerr << "error: " << llvm::toString(std::move(err)) << std::endl;
        declEpoch = savedDecl;
        prefixEpoch = savedPrefix;
        return false;
    }
    version = ver;
    defEpoch = defEp;

    if (!a.wrapper) return true;
    auto sym = jit->lookup(a.entry);
    if (!sym) {
        std::cerr << "error: " << llvm::toString(sym.takeError()) << std::endl;
        return true;
    }
    auto fn = sym->toPtr<void (*)()>();
    fflush(stdout);
    bool threw = false;
    try {
        if (int sig = callGuarded(fn)) {
            threw = true;
            fflush(stdout);
#ifdef _WIN32
            std::cerr << "error: the input crashed (signal " << sig << ")"
#else
            std::cerr << "error: the input crashed (" << strsignal(sig)
#endif
                      << "); the session may be unstable" << std::endl;
        }
    } catch (void*& ex) {
        // The thrown object is the Eskiu exception itself (typed void*, its first word).
        threw = true;
        fflush(stdout);
        std::string d = describeException((void*)&ex);
        std::cerr << "error: uncaught exception" << (d.empty() ? "" : ": " + d) << std::endl;
    } catch (...) {
        threw = true;
        fflush(stdout);
        std::cerr << "error: uncaught exception" << std::endl;
    }
    fflush(stdout);
    if (!threw && a.usesAwait) {
        auto ok = jit->lookup("__repl_await_ok");
        if (!ok) {
            llvm::consumeError(ok.takeError());
        } else if (*ok->toPtr<const int*>() == 0) {
            std::cerr << "error: the awaited future did not complete" << std::endl;
            threw = true;
        }
    }
    if (!threw && layout) {
        std::string vname = "__repl_val_" + std::to_string(cur);
        auto vs = jit->lookup(vname);
        if (!vs) {
            llvm::consumeError(vs.takeError());
        } else {
            std::string text = formatValue(*layout, vs->toPtr<const char*>(), 0);
            std::fputs((text + "\n").c_str(), stdout);
            fflush(stdout);
        }
    }
    return true;
}

void Repl::commit(const Plan& plan, Attempt& a) {
    int cur = ++epoch;
    if (a.asyncSupport) asyncSupport = true;
    if (plan.mode == Mode::Decls) {
        chunks.push_back({plan.text, plan.file, cur});
        return;
    }
    for (auto g : a.newGlobals) {
        globals.erase(std::remove_if(globals.begin(), globals.end(),
                                     [&](const ReplGlobal& r) { return r.name == g.name; }),
                      globals.end());
        g.epoch = cur;
        globals.push_back(g);
    }
}

// Type each untyped `let x = E;`, in order, from a check of the input cut at that let.
bool Repl::inferLets(Plan& plan, const std::vector<std::pair<int, int>>& letPos) {
    for (size_t li = 0; li < letPos.size(); ++li) {
        Plan probe = plan;
        probe.probeLet = (long)li;
        Attempt a;
        std::string errs;
        bool built;
        {
            CerrCapture cap;
            built = build(probe, a);
            errs = cap.text();
        }
        if (!built) { std::cerr << errs; return false; }
        TypeChecker tc;
        tc.sourceFile = kReplFile;
        bool ok;
        {
            CerrCapture cap;
            ok = tc.check(a.program.get());
            errs = cap.text();
        }
        std::string t;
        bool printable;
        if (!typeOfLast(a, tc, t, printable)) {
            if (!ok) std::cerr << errs;
            else std::cerr << "error: " << kReplFile << ": cannot infer a type for this `let`"
                           << std::endl;
            return false;
        }
        std::string spelled = tc.displayType(t);
        ParseCtx ctx;
        std::vector<Token> tt;
        if (!lexInto(ctx, spelled, kReplFile, tt) || tt.empty()) return false;
        // A diagnostic about the written-in type points at the variable's name.
        for (auto& tok : tt) { tok.line = letPos[li].first; tok.column = letPos[li].second; }
        plan.letTypes[li] = tt;
    }
    return true;
}

// Run one input. `forced` picks the reading (a :load file is declarations).
bool Repl::handleInput(const std::string& text, const std::string& file, Mode* forced) {
    Plan plan;
    plan.text = text;
    plan.file = file;

    std::vector<std::pair<int, int>> letPos;
    if (forced) {
        plan.mode = *forced;
    } else {
        // Declarations if the input reads as top-level declarations other than variables,
        // else statements, else whichever reading got further before its error.
        Attempt d, s;
        std::string dErr, sErr;
        bool dOk, sOk;
        {
            CerrCapture cap;
            Plan p = plan;
            p.mode = Mode::Decls;
            dOk = build(p, d);
            dErr = cap.text();
        }
        bool dHasVars = false;
        for (const auto& x : d.inputDecls)
            if (nodeCast<VarDecl>(x.get())) dHasVars = true;
        {
            CerrCapture cap;
            Plan p = plan;
            p.mode = Mode::Stmts;
            p.placeholderLets = true;
            sOk = build(p, s);
            sErr = cap.text();
        }
        if (dOk && (!dHasVars || !sOk)) plan.mode = Mode::Decls;
        else if (sOk) plan.mode = Mode::Stmts;
        else {
            std::cerr << (firstErrorPos(dErr) > firstErrorPos(sErr) ? dErr : sErr);
            return false;
        }
        letPos = s.letPos;
    }

    if (plan.mode == Mode::Decls) {
        Attempt a;
        if (!build(plan, a)) return false;
        if (!compileAndRun(a, "", "")) return false;
        commit(plan, a);
        return true;
    }

    if (!letPos.empty() && !inferLets(plan, letPos)) return false;

    // Check the input once to learn the type of its final expression.
    Attempt first;
    std::string errs;
    bool ok;
    {
        CerrCapture cap;
        ok = build(plan, first);
        errs = cap.text();
    }
    if (!ok) { std::cerr << errs; return false; }
    TypeChecker tc;
    tc.sourceFile = kReplFile;
    {
        CerrCapture cap;
        ok = tc.check(first.program.get());
        errs = cap.text();
    }
    std::string semaType;
    bool printable = false;
    typeOfLast(first, tc, semaType, printable);
    if (printable) {
        Plan withValue = plan;
        // `null` has no type to declare a variable with: hold it as a pointer.
        if (semaType == "null") semaType = "*void";
        withValue.valueType = tc.displayType(semaType);
        Attempt a;
        std::string errs2;
        bool built;
        {
            CerrCapture cap;
            built = build(withValue, a);
            if (built) {
                TypeChecker probe;
                probe.sourceFile = kReplFile;
                built = probe.check(a.program.get());
            }
            errs2 = cap.text();
        }
        if (built) {
            Attempt run;
            build(withValue, run);
            if (!compileAndRun(run, semaType, withValue.valueType)) return false;
            commit(plan, run);
            return true;
        }
        if (!ok) { std::cerr << errs2; return false; }
    } else if (!ok) {
        std::cerr << errs;
        return false;
    }
    Attempt a;
    if (!build(plan, a)) return false;
    if (!compileAndRun(a, "", "")) return false;
    commit(plan, a);
    return true;
}

void Repl::typeCommand(const std::string& expr) {
    Plan plan;
    plan.text = expr;
    Attempt a;
    std::string errs;
    bool ok;
    {
        CerrCapture cap;
        ok = build(plan, a);
        errs = cap.text();
    }
    if (!ok) { std::cerr << errs; return; }
    TypeChecker tc;
    tc.sourceFile = kReplFile;
    {
        CerrCapture cap;
        ok = tc.check(a.program.get());
        errs = cap.text();
    }
    std::string t;
    bool printable;
    if (!typeOfLast(a, tc, t, printable)) {
        if (!ok) std::cerr << errs;
        else std::fputs("void\n", stdout);
        return;
    }
    std::fputs((tc.displayType(t) + "\n").c_str(), stdout);
}

void Repl::loadCommand(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "error: cannot open '" << path << "'" << std::endl;
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    Mode m = Mode::Decls;
    handleInput(ss.str(), path, &m);
}

const char* kHelp =
    "Enter declarations (functions, structs, enums, imports), statements or an\n"
    "expression; an expression's value is printed. `let x = expr;` declares a session\n"
    "variable. An unfinished input continues on the next line (an open bracket or string,\n"
    "a trailing operator, a header such as `int f(int n)` whose `{` comes next); an `if`\n"
    "or `try` waits one line for `else`/`catch`. A blank line sends what is pending.\n"
    "`await` runs on the session's event loop, `repl_loop()`.\n"
    "  :type EXPR   print the type of EXPR without running it\n"
    "  :load FILE   add the declarations of FILE\n"
    "  :reset       forget every declaration and variable\n"
    "  :help        show this help\n"
    "  :quit        leave (also Ctrl-D)\n";

int Repl::run(const std::vector<std::string>& files) {
    if (!initJit()) return 1;
    for (const auto& f : files) loadCommand(f);
    if (interactive) {
        std::printf("Eskiu REPL. Type :help for help, :quit to leave.\n");
    }
    std::string pending;
    std::string line;
    while (true) {
        if (interactive) {
            std::fputs(pending.empty() ? "> " : "... ", stdout);
            fflush(stdout);
        }
        auto submit = [&]() {
            std::string input = pending;
            pending.clear();
            try {
                handleInput(input, kReplFile, nullptr);
            } catch (const std::exception& e) {
                std::cerr << "error: " << e.what() << std::endl;
            }
            fflush(stdout);
        };
        if (!std::getline(std::cin, line)) {
            if (interactive) std::fputs("\n", stdout);
            if (!trim(pending).empty()) submit();
            break;
        }
        // An `if` held for a possible `else`: anything else on this line runs it first.
        if (!pending.empty() && bracketDepth(pending) <= 0 && ifAwaitsElse(pending) &&
            !awaitsBody(pending) && !incompleteInput(pending) &&
            !trim(line).empty() && !continuesHeld(line))
            submit();
        if (pending.empty()) {
            std::string t = trim(line);
            if (t.empty()) continue;
            if (std::string op = danglingOperator(t); !op.empty()) {
                std::cerr << "error: a line cannot start with '" << op << "': the line before already ran."
                          << " To continue an expression, end the line with the operator (x > 3 ?)"
                          << std::endl;
                continue;
            }
            if (t[0] == ':') {
                size_t sp = t.find_first_of(" \t");
                std::string cmd = t.substr(0, sp);
                std::string arg = sp == std::string::npos ? "" : trim(t.substr(sp));
                if (cmd == ":quit" || cmd == ":q" || cmd == ":exit") break;
                else if (cmd == ":help" || cmd == ":h") std::fputs(kHelp, stdout);
                else if (cmd == ":reset") reset();
                else if (cmd == ":type" || cmd == ":t") typeCommand(arg);
                else if (cmd == ":load" || cmd == ":l") loadCommand(arg);
                else std::cerr << "error: unknown command '" << cmd << "' (try :help)" << std::endl;
                fflush(stdout);
                continue;
            }
        }
        // A blank line sends what is pending as it is (a way out of a continuation).
        // A blank line inside an open bracket is part of the input.
        if (!pending.empty() && trim(line).empty() && bracketDepth(pending) <= 0) {
            submit();
            continue;
        }
        pending += line + "\n";
        if (bracketDepth(pending) > 0 || awaitsBody(pending) || incompleteInput(pending) ||
            ifAwaitsElse(pending))
            continue;
        std::string input = pending;
        pending.clear();
        if (trim(input).empty()) continue;
        try {
            handleInput(input, kReplFile, nullptr);
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
        }
        fflush(stdout);
    }
    return 0;
}

}  // namespace

int runRepl(const std::vector<std::string>& files) {
#if defined(_WIN32)
    // The JIT registers no Windows (SEH) unwind tables for the code it loads, so code that
    // throws cannot run, and sessions were not reliable: the REPL is left out on Windows.
    (void)files;
    std::cerr << "error: `eskiuc repl` is not available on Windows in this release; "
                 "run a file with `eskiuc run file.esk`" << std::endl;
    return 1;
#endif
    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    stdlibRoot = resolveStdlibPath();
    Repl repl(llvm::sys::Process::StandardInIsUserInput());
    return repl.run(files);
}
