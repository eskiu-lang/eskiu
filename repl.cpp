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

#include "repl_values.h"
#include "repl_input.h"
#include "repl_session.h"
#include "repl_exec.h"

namespace {

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
