#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <utility>
#include "sema/type.h"

// ============================================================================
// Template type-name utilities — shared by the type checker and codegen.
// These operate purely on type-name strings (e.g. "Result<int,string>"); they
// were previously duplicated verbatim in sema/ and codegen/.
// ============================================================================

// The program's type aliases (name -> target), registered by the type checker and codegen
// as they meet each `type` declaration: a template argument that names an alias mangles
// as its target, so `Box<F>` (`type F = int`) and `Box<int>` are one instance.
inline std::map<std::string, std::string>& templateTypeAliases() {
    static std::map<std::string, std::string> aliases;
    return aliases;
}

// "Result<int,string>" -> "Result_int_string"
inline std::string mangleTemplate(const std::string& type0) {
    const auto& aliases = templateTypeAliases();
    std::string type = aliases.empty() ? type0 : ty::dealiasSpelling(type0, aliases);
    std::string out;
    int depth = 0;
    bool afterArray = false;
    for (size_t i = 0; i < type.size(); ++i) {
        char c = type[i];
        bool wasAfterArray = afterArray;
        afterArray = false;
        if (c == '<') ++depth;
        else if (c == '>' && depth) --depth;
        // An array / slice type argument (`Box<int[3]>`, `Box<int[]>`) mangles to an
        // identifier (`Box_int_A3`, `Box_int_S`), not a bracketed array spelling.
        if (depth > 0 && c == '[') {
            if (i + 1 < type.size() && type[i + 1] == ']') { out += "_S"; ++i; }
            else out += "_A";
        }
        else if (depth > 0 && c == ']') afterArray = true;
        // A pointer to an array or slice argument (`Box<int[3]*>`, from `Box<*A3>` with
        // `type A3 = int[3]`) marks each star `_P`: a trailing `*` would read as a pointer
        // to the instance.
        else if (depth > 0 && c == '*' && wasAfterArray) { out += "_P"; afterArray = true; }
        else if (c == '<' || c == '>' || c == ',') out += '_';
        else if (c != ' ')                   out += c;
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

// "Result<int,string>" -> {"Result", {"int","string"}}
inline std::pair<std::string, std::vector<std::string>>
splitTemplateType(const std::string& type) {
    size_t lt = type.find('<');
    if (lt == std::string::npos) return {type, {}};
    std::string name = type.substr(0, lt);
    std::string inner = type.substr(lt + 1, type.size() - lt - 2);
    std::vector<std::string> args;
    // A comma splits only at the top level: not inside a nested `<...>`, a fn type's
    // `(...)` or an array's `[...]`, and the `>` of a `->` closes nothing.
    int depth = 0; std::string cur;
    for (size_t i = 0; i < inner.size(); ++i) {
        char c = inner[i];
        if (c == '<' || c == '(' || c == '[') depth++;
        else if ((c == '>' && !(i > 0 && inner[i - 1] == '-')) || c == ')' || c == ']') depth--;
        if (c == ',' && depth == 0) { args.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) args.push_back(cur);
    return {name, args};
}

// Substitute type parameters in `t` using `subs` (e.g. T->int), recursively
// through pointers, arrays, function types, and nested templates. Delegates to
// the structured `Type` IR (`Type::substitute`) — one parser/renderer instead of
// the hand-rolled string surgery this used to be. The names appearing as `subs`
// keys parse as type parameters so they substitute structurally.
inline std::string substType(const std::string& t,
                             const std::map<std::string, std::string>& subs) {
    std::set<std::string> keys;
    for (const auto& kv : subs) keys.insert(kv.first);
    return ty::Type::parse(t, keys).substitute(subs).str();
}
