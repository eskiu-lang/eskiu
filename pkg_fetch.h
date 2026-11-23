// pkg_fetch.h: git, fetching packages and native libraries (internal to pkg.cpp, which includes it once, in order).
#pragma once
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "pkg.h"
#include "main_support.h"

namespace {

// ── git ──────────────────────────────────────────────────────────────────────

std::string gitProgram() {
    static std::string git = [] {
        auto p = llvm::sys::findProgramByName("git");
        return p ? *p : std::string();
    }();
    return git;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Run git with `args`; stdout goes to `out` and stderr to `errOut` (trimmed).
int runGit(const std::vector<std::string>& args, std::string* out, std::string* errOut,
           const std::string& forPkg) {
    std::string git = gitProgram();
    if (git.empty()) throw PkgError("git not found on PATH (needed to fetch package '" + forPkg + "')");
    llvm::SmallString<128> outFile, errFile;
    llvm::sys::fs::createTemporaryFile("eskiu-git", "out", outFile);
    llvm::sys::fs::createTemporaryFile("eskiu-git", "err", errFile);
    std::vector<std::string> argv = {git};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<llvm::StringRef> refs(argv.begin(), argv.end());
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(outFile),
                                                  llvm::StringRef(errFile)};
    int rc = llvm::sys::ExecuteAndWait(git, refs, std::nullopt, redirects);
    if (out) *out = trim(readText(std::string(outFile)));
    if (errOut) *errOut = trim(readText(std::string(errFile)));
    llvm::sys::fs::remove(outFile);
    llvm::sys::fs::remove(errFile);
    return rc;
}

std::string withGitError(const std::string& msg, const std::string& gitErr) {
    if (gitErr.empty()) return msg;
    return msg + "\n" + gitErr;
}

class Fetcher {
public:
    bool update = false;   // fetch every mirror and re-resolve revs (eskiuc fetch / add)

    std::string resolveRev(const std::string& name, const std::string& url, const std::string& rev) {
        std::string key = url + "\n" + rev;
        if (auto it = revCache.find(key); it != revCache.end()) return it->second;
        std::string mirror = ensureMirror(name, url);
        // A branch, a tag or HEAD names whatever the remote has now: resolve it after fetching
        // (once per run), or a mirror from an earlier run would keep answering with an old
        // commit. A full commit hash cannot move, so the mirror answers it when it has it.
        bool hash = rev.size() == 40 && rev.find_first_not_of("0123456789abcdef") == std::string::npos;
        if (!hash) refresh(name, url, mirror);
        std::string commit;
        if (!lookup(mirror, rev, commit, name)) {
            refresh(name, url, mirror);
            if (!lookup(mirror, rev, commit, name))
                throw PkgError("rev '" + rev + "' not found in " + url + " (package '" + name + "')");
        }
        revCache[key] = commit;
        return commit;
    }

    // Check out `commit` into its own directory unless it is already there.
    void checkout(const std::string& name, const std::string& url, const std::string& commit) {
        std::string dir = pkgHome() + "/pkgs/" + name + "/" + commit;
        std::error_code ec;
        if (fs::is_directory(dir, ec)) return;
        std::string mirror = ensureMirror(name, url);
        std::string c;
        if (!lookup(mirror, commit, c, name)) {
            refresh(name, url, mirror);
            if (!lookup(mirror, commit, c, name))
                throw PkgError("commit " + commit + " not found in " + url + " (package '" + name + "')");
        }
        std::string tmp = dir + ".tmp";
        fs::remove_all(tmp, ec);
        std::string gerr;
        if (runGit({"clone", "--quiet", "--no-checkout", mirror, tmp}, nullptr, &gerr, name) != 0 ||
            runGit({"-C", tmp, "checkout", "--quiet", "--detach", commit}, nullptr, &gerr, name) != 0) {
            fs::remove_all(tmp, ec);
            throw PkgError(withGitError("could not check out " + commit + " of package '" + name + "'", gerr));
        }
        fs::rename(tmp, dir, ec);
        if (ec) {
            fs::remove_all(tmp, ec);
            throw PkgError("could not create " + dir);
        }
    }

private:
    std::map<std::string, std::string> revCache;
    std::set<std::string> fresh;   // mirrors cloned or fetched in this run

    bool lookup(const std::string& mirror, const std::string& rev, std::string& commit,
                const std::string& name) {
        std::string out;
        if (runGit({"-C", mirror, "rev-parse", "--verify", "--quiet", rev + "^{commit}"}, &out,
                   nullptr, name) != 0 || out.empty())
            return false;
        commit = out;
        return true;
    }

    std::string ensureMirror(const std::string& name, const std::string& url) {
        std::string mirror = pkgHome() + "/pkgs/" + name + "/.repo";
        std::error_code ec;
        if (fs::is_directory(mirror, ec)) {
            std::string have;
            runGit({"-C", mirror, "config", "--get", "remote.origin.url"}, &have, nullptr, name);
            if (have == url) {
                if (update) refresh(name, url, mirror);
                return mirror;
            }
            fs::remove_all(mirror, ec);   // the manifest now names another URL
        }
        fs::create_directories(pkgHome() + "/pkgs/" + name, ec);
        std::cerr << "fetching " << name << " from " << url << std::endl;
        std::string gerr;
        if (runGit({"clone", "--quiet", "--bare", url, mirror}, nullptr, &gerr, name) != 0) {
            fs::remove_all(mirror, ec);
            throw PkgError(withGitError("could not clone " + url + " (package '" + name + "')", gerr));
        }
        fresh.insert(mirror);
        return mirror;
    }

    void refresh(const std::string& name, const std::string& url, const std::string& mirror) {
        if (!fresh.insert(mirror).second) return;
        std::cerr << "updating " << name << " from " << url << std::endl;
        std::string gerr;
        if (runGit({"-C", mirror, "fetch", "--quiet", "--force", "--tags", "origin",
                    "+refs/heads/*:refs/heads/*"}, nullptr, &gerr, name) != 0)
            throw PkgError(withGitError("could not fetch " + url + " (package '" + name + "')", gerr));
    }
};

// ── Native libraries ────────────────────────────────────────────────────────

// Run `program` with `args`; stderr goes to `errOut` (trimmed). -1 when it is not found.
int runTool(const std::string& name, const std::vector<std::string>& args, std::string* errOut) {
    auto found = llvm::sys::findProgramByName(name);
    if (!found) return -1;
    llvm::SmallString<128> errFile;
    llvm::sys::fs::createTemporaryFile("eskiu-tool", "err", errFile);
    std::vector<std::string> argv = {*found};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<llvm::StringRef> refs(argv.begin(), argv.end());
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(""),
                                                  llvm::StringRef(errFile)};
    int rc = llvm::sys::ExecuteAndWait(*found, refs, std::nullopt, redirects);
    if (errOut) *errOut = trim(readText(std::string(errFile)));
    llvm::sys::fs::remove(errFile);
    return rc;
}

std::string nativeDir(const NativeSource& src) { return pkgHome() + "/native/" + src.sha256; }

std::string sha256Hex(const std::string& data) {
    llvm::SHA256 h;
    h.update(llvm::StringRef(data));
    auto d = h.final();
    return llvm::toHex(llvm::ArrayRef<uint8_t>(d.data(), d.size()), /*LowerCase=*/true);
}

// Whether `p` is `root` or lies under it, component by component.
bool pathUnder(const fs::path& p, const fs::path& root) {
    auto q = p.begin();
    for (auto r = root.begin(); r != root.end(); ++r, ++q)
        if (q == p.end() || *q != *r) return false;
    return true;
}

// What makes the library directory `lib` of the archive unpacked in `base` unsafe to
// link from, or "": a component of it that is a symbolic link, or a symbolic link in it
// that does not resolve inside `base`. The checksum covers the archive's bytes, not what
// a link in it reaches (a library directory linked to /usr/lib would be rewritten and
// re-signed on macOS, and linked from).
std::string nativeLibDirProblem(const std::string& base, const std::string& lib) {
    std::error_code ec;
    fs::path cur(base);
    for (const auto& part : fs::path(lib)) {
        if (part.empty() || part == ".") continue;
        cur /= part;
        if (fs::is_symlink(fs::symlink_status(cur, ec))) return "its directory '" + lib + "' is a symbolic link";
    }
    fs::path root = fs::canonical(base, ec);
    if (ec) return "its directory cannot be read";
    for (const auto& ent : fs::directory_iterator(cur, ec)) {
        std::error_code sec;
        if (!ent.is_symlink(sec)) continue;
        fs::path real = fs::canonical(ent.path(), sec);
        if (sec || !pathUnder(real, root))
            return "the symbolic link '" + lib + "/" + ent.path().filename().string() +
                   "' leads out of the archive";
    }
    return "";
}

// Download, check and unpack `src` into nativeDir(src) unless it is already there.
void fetchNative(const std::string& pkg, const std::string& lib, const NativeSource& src) {
    std::string dir = nativeDir(src);
    std::error_code ec;
    std::string what = "native library '" + lib + "' of package '" + pkg + "'";
    if (fs::is_directory(dir, ec)) {
        std::string bad = nativeLibDirProblem(dir, src.lib);
        if (!bad.empty()) throw PkgError("the " + what + " is not used: " + bad);
        return;
    }
    fs::create_directories(pkgHome() + "/native", ec);
    std::string file = dir + ".download", tmp = dir + ".tmp";
    fs::remove(file, ec);
    fs::remove_all(tmp, ec);
    std::cerr << "fetching " << lib << " for " << pkg << " from " << src.url << std::endl;
    std::string err;
    int rc = runTool("curl", {"-fsSL", "--retry", "2", "-o", file, src.url}, &err);
    if (rc == -1) throw PkgError("curl not found on PATH (needed to fetch the " + what + ")");
    if (rc != 0) {
        fs::remove(file, ec);
        throw PkgError(withGitError("could not download the " + what + " from " + src.url, err));
    }
    std::string got = sha256Hex(readText(file));
    if (got != src.sha256) {
        fs::remove(file, ec);
        throw PkgError("the " + what + " from " + src.url + " has sha256 " + got + ", not " + src.sha256);
    }
    fs::create_directories(tmp, ec);
    std::string url = src.url;
    bool zip = url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0;
#if defined(__linux__)
    // GNU tar does not read zip archives; macOS and Windows tar (libarchive) do.
    rc = zip ? runTool("unzip", {"-q", file, "-d", tmp}, &err) : runTool("tar", {"-xf", file, "-C", tmp}, &err);
    std::string tool = zip ? "unzip" : "tar";
#else
    (void)zip;
    rc = runTool("tar", {"-xf", file, "-C", tmp}, &err);
    std::string tool = "tar";
#endif
    fs::remove(file, ec);
    if (rc != 0) {
        fs::remove_all(tmp, ec);
        if (rc == -1) throw PkgError(tool + " not found on PATH (needed to unpack the " + what + ")");
        throw PkgError(withGitError("could not unpack the " + what, err));
    }
    if (!fs::is_directory(tmp + "/" + src.lib, ec)) {
        fs::remove_all(tmp, ec);
        throw PkgError("the " + what + " has no directory '" + src.lib + "' (lib in its eskiu.toml)");
    }
    std::string bad = nativeLibDirProblem(tmp, src.lib);
    if (!bad.empty()) {
        fs::remove_all(tmp, ec);
        throw PkgError("the " + what + " is not used: " + bad);
    }
#if defined(__APPLE__)
    // Ensure any .dylib in the archive has an @rpath install name (upstream archives such as
    // wgpu-native's iOS builds sometimes record the builder's absolute path in LC_ID_DYLIB).
    for (const auto& ent : fs::directory_iterator(tmp + "/" + src.lib, ec)) {
        if (ent.is_regular_file(ec) && !ent.is_symlink(ec) && ent.path().extension() == ".dylib") {
            std::string p = ent.path().generic_string();
            std::string id = "@rpath/" + ent.path().filename().string();
            if (runTool("install_name_tool", {"-id", id, p}, nullptr) == 0)
                runTool("codesign", {"--force", "--sign", "-", p}, nullptr);
        }
    }
#endif
    fs::rename(tmp, dir, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        throw PkgError("could not create " + dir);
    }
}

// The stdout of `name args` (trimmed), or "" when it fails or is not on PATH.
std::string toolOutput(const std::string& name, const std::vector<std::string>& args) {
    auto found = llvm::sys::findProgramByName(name);
    if (!found) return "";
    llvm::SmallString<128> outFile;
    if (llvm::sys::fs::createTemporaryFile("eskiu-tool", "out", outFile)) return "";
    std::vector<std::string> argv = {*found};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<llvm::StringRef> refs(argv.begin(), argv.end());
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(outFile),
                                                  llvm::StringRef("")};
    int rc = llvm::sys::ExecuteAndWait(*found, refs, std::nullopt, redirects, 20);
    std::string out = rc == 0 ? trim(readText(std::string(outFile))) : "";
    llvm::sys::fs::remove(outFile);
    return out;
}

}  // namespace
