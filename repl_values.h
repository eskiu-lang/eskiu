// repl_values.h: session records, async support and printing values (internal to repl.cpp, which includes it once, in order).
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "repl.h"
#include "codegen/codegen.h"
#include "sema/type_checker.h"

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

}  // namespace
