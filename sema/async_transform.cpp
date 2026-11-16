// async_transform.cpp: AsyncTransform::run, which lowers each async function with an
// AsyncLowering (async_lower.h), and declType.
#include "async_lower.h"

// A checked type as a declaration spells it: the `struct:`/`interface:` tags dropped and
// a generic instance's mangled name (`List_int`) written as the template (`List<int>`).
std::string AsyncTransform::declType(const std::string& checked) const {
    std::string out;
    size_t i = 0, n = checked.size();
    auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    while (i < n) {
        if (!isId(checked[i])) { out += checked[i++]; continue; }
        size_t j = i;
        while (j < n && isId(checked[j])) ++j;
        std::string id = checked.substr(i, j - i);
        i = j;
        if ((id == "struct" || id == "interface") && i < n && checked[i] == ':') { ++i; continue; }
        const std::pair<std::string, std::vector<std::string>>* inst = nullptr;
        if (instanceArgs) {
            auto it = instanceArgs->find(id);
            if (it != instanceArgs->end()) inst = &it->second;
        }
        if (inst) {
            out += inst->first + "<";
            for (size_t k = 0; k < inst->second.size(); ++k)
                out += (k ? "," : "") + declType(inst->second[k]);
            out += ">";
        } else {
            out += id;
        }
    }
    return out;
}

void AsyncTransform::run(Program* program) {
    std::vector<DeclPtr> out;
    std::set<std::string> topNames;   // top-level declarations (the frame type / resume fn avoid them)
    for (auto& decl : program->declarations) topNames.insert(decl->name);

    for (auto& decl : program->declarations) {
        auto* fn = nodeCast<FunctionDecl>(decl.get());
        if (!fn || !fn->isAsync) { out.push_back(decl); continue; }
        AsyncLowering lw(*this, fn, topNames);
        if (!lw.setup()) { out.push_back(decl); continue; }
        lw.collectFrame();
        lw.lowerBody();
        lw.buildCleanups();
        lw.snapshotCaptures();
        lw.emitDecls(out);
    }

    program->declarations = std::move(out);
}
