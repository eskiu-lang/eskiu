// pkg_web.h: copying a package's [web] assets (internal to pkg.cpp, which includes it once, in order).
#pragma once
#include <filesystem>
#include <iostream>
#include <string>

namespace {

// One [web] asset: the package listing it, the package's directory and the asset's
// path inside it (relative, no `..`: readManifest checks the spelling).
struct WebAsset {
    std::string pkg, root, rel;
    std::string path() const { return root + "/" + rel; }
};

// Whether the asset is a file or directory of its package: no component of its path
// below the package directory is a symbolic link, and it resolves inside the package.
// A link could make a web build copy any file the user can read into its output.
bool webAssetInside(const WebAsset& a) {
    std::error_code ec;
    fs::path cur(a.root);
    for (const auto& part : fs::path(a.rel)) {
        if (part.empty() || part == ".") continue;
        cur /= part;
        if (fs::is_symlink(fs::symlink_status(cur, ec))) return false;
    }
    fs::path root = fs::canonical(a.root, ec);
    if (ec) return false;
    fs::path real = fs::canonical(a.path(), ec);
    return !ec && pathUnder(real, root);
}

// Copy the asset directory `from` to `to` with what it holds. A symbolic link inside it
// is not followed or copied (a warning names it).
void copyWebTree(const fs::path& from, const fs::path& to, const WebAsset& a, std::error_code& ec) {
    fs::create_directories(to, ec);
    if (ec) return;
    fs::recursive_directory_iterator it(from, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        fs::path dest = to / fs::relative(e.path(), from, ec);
        if (ec) return;
        std::error_code sec;
        if (e.is_symlink(sec)) {
            std::cerr << "warning: web asset " << displayPath(a.path()) << " of package '" << a.pkg
                      << "': symbolic link " << displayPath(e.path().generic_string())
                      << " is not copied" << std::endl;
        } else if (e.is_directory(sec)) {
            fs::create_directories(dest, ec);
        } else if (e.is_regular_file(sec)) {
            fs::copy_file(e.path(), dest, fs::copy_options::overwrite_existing, ec);
        }
    }
}

// Copy one asset to `to/name`; a link or a path leading out of the package is skipped
// with a warning.
void copyWebAsset(const WebAsset& a, const fs::path& to, const std::string& name) {
    std::error_code ec;
    if (!webAssetInside(a)) {
        std::cerr << "warning: web asset " << displayPath(a.path()) << " of package '" << a.pkg
                  << "' is a symbolic link or leads out of the package; not copied" << std::endl;
        return;
    }
    fs::path from(a.path());
    if (fs::equivalent(from, to / name, ec)) return;
    ec.clear();
    if (fs::is_directory(from, ec))
        copyWebTree(from, to / name, a, ec);
    else
        fs::copy_file(from, to / name, fs::copy_options::overwrite_existing, ec);
    if (ec)
        std::cerr << "warning: could not copy web asset " << displayPath(a.path()) << " to "
                  << displayPath((to / name).generic_string()) << ": " << ec.message() << std::endl;
}

}  // namespace
