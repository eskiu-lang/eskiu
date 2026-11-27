// Which imported files are the program's own code, and so get warnings (TypeChecker::lintRoot),
// and which module a file belongs to (named in the note after a redefinition).
#include "type_checker.h"
#include "../parser/parser.h"
#include "../main_support.h"
#include <filesystem>

std::string TypeChecker::lintRootFor(const std::string& rootFile) {
    if (rootFile.empty()) return "";
    std::error_code ec;
    std::filesystem::path file = std::filesystem::weakly_canonical(std::filesystem::absolute(rootFile, ec), ec);
    if (ec) return "";
    std::filesystem::path start = file.parent_path();
    for (std::filesystem::path dir = start; !dir.empty(); dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / "eskiu.toml", ec)) { start = dir; break; }
        if (dir == dir.parent_path()) break;
    }
    std::string root = start.string();
    if (root.empty() || (root.back() != '/' && root.back() != '\\')) root += std::filesystem::path::preferred_separator;
    return root;
}

bool TypeChecker::lintsDecl(const Decl* d) {
    if (!d || !d->fromImport) return true;
    if (lintRoot.empty() || d->sourceFile.empty()) return false;
    auto it = lintedFiles.find(d->sourceFile);
    if (it != lintedFiles.end()) return it->second;
    std::string canon = Parser::canonicalPath(d->sourceFile);
    bool own = canon.compare(0, lintRoot.size(), lintRoot) == 0;
    lintedFiles[d->sourceFile] = own;
    return own;
}

std::string TypeChecker::moduleOfFile(const std::string& file) {
    auto under = [&](const std::string& dir) {
        return file.size() > dir.size() + 1 && file.compare(0, dir.size(), dir) == 0 && file[dir.size()] == '/';
    };
    for (const auto& [name, root] : packageRoots.roots)
        if (!root.empty() && under(root)) return " (in package '" + name + "')";
    std::string lib = stdlibRoot.empty() ? "stdlib" : stdlibRoot + "/stdlib";
    if (under(lib) && file.size() > lib.size() + 5 && file.compare(file.size() - 4, 4, ".esk") == 0)
        return " (in <" + file.substr(lib.size() + 1, file.size() - lib.size() - 5) + ">)";
    return "";
}

void TypeChecker::noteFirstDefined(Decl* prev, const std::string& name, const std::string& shown) {
    clashedNames.insert(name);
    if (!prev || errors.empty()) return;
    std::string file = prev->sourceFile.empty() ? sourceFile : prev->sourceFile;
    errors.back() += "\nnote: " + file + ":" + std::to_string(prev->line) + ":" + std::to_string(prev->col) +
                     ": '" + (shown.empty() ? name : shown) + "' is first defined here" + moduleOfFile(file);
}
