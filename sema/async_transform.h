#pragma once
#include "../ast/ast.h"
#include <map>
#include <string>
#include <vector>

// AsyncTransform — lowers `async fn` + `await` into a resumable state machine
// (see docs/dev/async-design.md §4). Runs after type checking, before codegen:
// each async function is replaced by a frame struct, a `__<name>_resume` function
// (an if-chain over the resume state), and a constructor that returns *Future<T>.
// The result is ordinary Eskiu AST that normal codegen handles.
//
// Awaits are lowered across all control flow: `if`/`else`, `while`, `do`/`while`,
// C-style `for`, `switch`, `match`, `for`-`in` (desugared to a counted `for`) and
// `try`/`catch`, including `break`/`continue` and early `return`. A first pass hoists
// every `await` (in evaluation order) into a `let` of its own; `exprTypes` (the type
// checker's expression types) types the temporaries that hold an operand evaluated
// before an await in the same expression.
class AsyncTransform {
public:
    using InstanceArgs = std::map<std::string, std::pair<std::string, std::vector<std::string>>>;
    // `instanceArgs` (the checker's generic instances: mangled name -> template and
    // arguments) spells a checked type the way a declaration writes it (`List<int>`).
    using InstanceTypes = std::vector<std::pair<std::map<std::string, std::string>, std::string>>;
    // `instanceExprTypes` (a generic async body's expression types, per checked instance)
    // types the same temporaries there, generalized over the type parameters.
    AsyncTransform(const std::map<Expr*, std::string>* exprTypes = nullptr,
                   const InstanceArgs* instanceArgs = nullptr,
                   const std::map<Expr*, InstanceTypes>* instanceExprTypes = nullptr)
        : exprTypes(exprTypes), instanceArgs(instanceArgs), instanceExprTypes(instanceExprTypes) {}
    void run(Program* program);
private:
    friend class AsyncLowering;   // lowers one function (async_lower.h)
    const std::map<Expr*, std::string>* exprTypes;
    const InstanceArgs* instanceArgs;
    const std::map<Expr*, InstanceTypes>* instanceExprTypes;
    std::string declType(const std::string& checked) const;
};
