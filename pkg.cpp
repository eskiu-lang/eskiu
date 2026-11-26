// The package manager (see pkg.h).
//
// eskiu.toml is read by a small TOML subset reader (tables, string values, inline
// tables of strings, arrays of strings, comments). Dependencies are resolved breadth-first
// from the root manifest, each package's own eskiu.toml adding its dependencies; the result
// is written to eskiu.lock, one tab-separated line per package, sorted by name:
//
//   NAME  git   URL  REV  COMMIT     fetched into $ESKIU_HOME/pkgs/NAME/COMMIT
//   NAME  path  ABSOLUTE-DIR         used in place
//   NAME  root  .                    the package itself (the lock's directory)
//
// A git package is cloned once into a bare mirror ($ESKIU_HOME/pkgs/NAME/.repo) and each
// commit is checked out from it into its own directory. A lock entry whose URL and rev
// still match the manifest is reused without running git. The compilers read only the
// lock (pkgLoadRoots here, pkg_load_roots in selfhost/parser.esk).
//
// A package may declare prebuilt native libraries, one archive per platform:
//
//   [native.onnxruntime]
//   macos-arm64 = { url = "https://...tgz", sha256 = "...", lib = "onnxruntime-osx-arm64-1.31.0/lib" }
//
// NAME is the library that `#pragma link("NAME")` or -lNAME links. When a host build links
// it, the driver asks pkgNativeLibDirs for its directory: the archive of this host's
// platform is downloaded, its SHA-256 checked and unpacked into $ESKIU_HOME/native/SHA256
// (named by its contents, so it is fetched once), and the build gets its `lib` directory
// as -L and an rpath. `eskiuc fetch` downloads every one ahead. A platform with no entry
// links the library from the system.

#include "pkg.h"
#include "cli.h"
#include "main_support.h"
#include "parser/parser.h"

#include <algorithm>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SHA256.h"

namespace fs = std::filesystem;

#include "pkg_toml.h"
#include "pkg_manifest.h"
#include "pkg_fetch.h"
#include "pkg_web.h"

bool pkgPreferSystemLib(const std::string& lib) {
    const char* env = std::getenv("ESKIU_SYSTEM_LIBS");
    if (!env) return false;
    std::string cur;
    for (const char* p = env;; ++p) {
        if (*p == ',' || *p == ' ' || *p == '\t' || *p == 0) {
            if (cur == "*" || cur == lib) return true;
            cur.clear();
            if (*p == 0) return false;
        } else {
            cur += *p;
        }
    }
}

std::string pkgFindSystemLib(const std::string& lib, std::string* file) {
    std::vector<std::string> dirs;
    auto addList = [&](const char* var) {
        const char* v = std::getenv(var);
        if (!v) return;
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        std::stringstream ss(v);
        std::string d;
        while (std::getline(ss, d, sep))
            if (!d.empty()) dirs.push_back(d);
    };
    addList("LIBRARY_PATH");
    std::string lower = lib;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const std::string& pc : {lib, lower}) {
        std::string d = toolOutput("pkg-config", {"--variable=libdir", pc});
        if (!d.empty()) dirs.push_back(d);
    }
#if defined(__APPLE__)
    for (const char* d : {"/opt/homebrew/lib", "/usr/local/lib", "/opt/local/lib"}) dirs.push_back(d);
    const char* names[] = {".dylib", ".tbd", ".a"};
#elif defined(_WIN32)
    if (const char* m = std::getenv("MINGW_PREFIX"); m && *m) dirs.push_back(std::string(m) + "/lib");
    const char* names[] = {".dll.a", ".a", ".lib"};
#else
    for (const char* d : {"/usr/local/lib", "/usr/lib", "/lib", "/usr/lib64", "/lib64",
                          "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu"})
        dirs.push_back(d);
    const char* names[] = {".so", ".a"};
#endif
    std::error_code ec;
    for (const auto& d : dirs)
        for (const char* ext : names) {
            std::string f = d + "/lib" + lib + ext;
            if (fs::exists(f, ec)) {
                if (file) *file = f;
                return d;
            }
        }
    return "";
}

namespace {

// A wrong command line (exit code 2, cli.h): `error: CMD: MSG (see 'eskiuc CMD --help')`.
struct PkgUsage : PkgError {
    using PkgError::PkgError;
};

// A flag of the compiler passed through a package command; `i` steps over its value.
void checkCompilerFlag(const std::vector<std::string>& args, size_t& i) {
    bool takesValue = false;
    if (!isCompilerOption(args[i], &takesValue)) throw PkgUsage("unknown option '" + args[i] + "'");
    if (takesValue) {
        if (i + 1 >= args.size()) throw PkgUsage("option '" + args[i] + "' needs a value");
        ++i;
    }
}

struct HostNative {
    std::string pkg, lib;
    NativeSource src;
};

// The native libraries `triple`'s platform (or this host's when `triple` is empty) gets from
// the package at `lockDir` and the dependencies its lock names.
std::vector<HostNative> hostNatives(const std::string& lockDir,
                                    const std::map<std::string, LockEntry>& lock,
                                    const std::string& triple = "") {
    std::vector<HostNative> out;
    std::string platform = targetPlatform(triple);
    if (platform.empty()) return out;
    std::set<std::string> seen;
    for (const auto& kv : lock) {
        std::string root = kv.second.root(lockDir);
        std::error_code ec;
        if (!fs::is_regular_file(root + "/eskiu.toml", ec)) continue;
        for (const auto& lib : readManifest(root).natives) {
            auto it = lib.platforms.find(platform);
            if (it == lib.platforms.end() || !seen.insert(it->second.sha256).second) continue;
            out.push_back({kv.first, lib.name, it->second});
        }
    }
    return out;
}

void fetchNatives(const std::string& lockDir) {
    for (const auto& n : hostNatives(lockDir, readLock(lockDir)))
        if (!pkgPreferSystemLib(n.lib)) fetchNative(n.pkg, n.lib, n.src);
}

std::string describe(const LockEntry& e) {
    if (e.kind == "git") return "commit " + e.commit;
    return "path " + e.path;
}

// Resolve every dependency of `root` (transitively), fetching what is missing, and
// write eskiu.lock. `update` ignores the locked commits and fetches the mirrors.
std::vector<LockEntry> resolveAndLock(const Manifest& root, bool update) {
    std::map<std::string, LockEntry> lock = update ? std::map<std::string, LockEntry>()
                                                   : readLock(root.dir);
    Fetcher fetcher;
    fetcher.update = update;
    struct Item { Dep dep; std::string requirer, dir; };
    std::deque<Item> queue;
    for (const auto& d : root.deps) queue.push_back({d, root.name, root.dir});
    std::map<std::string, std::pair<LockEntry, std::string>> done;   // name -> entry, requirer
    while (!queue.empty()) {
        Item it = queue.front();
        queue.pop_front();
        const Dep& d = it.dep;
        if (d.name == root.name) continue;
        LockEntry e;
        e.name = d.name;
        if (!d.isGit) {
            e.kind = "path";
            fs::path p(d.path);
            if (p.is_relative()) p = fs::path(it.dir) / p;
            std::error_code ec;
            if (!fs::is_directory(p, ec))
                throw PkgError("path dependency '" + d.name + "' not found: " + p.generic_string() +
                               " (required by '" + it.requirer + "')");
            e.path = fs::weakly_canonical(p, ec).generic_string();
        } else {
            e.kind = "git";
            e.url = d.git;
            e.rev = d.rev.empty() ? "HEAD" : d.rev;
            auto l = lock.find(d.name);
            if (l != lock.end() && l->second.kind == "git" && l->second.url == e.url &&
                l->second.rev == e.rev)
                e.commit = l->second.commit;
            else
                e.commit = fetcher.resolveRev(d.name, e.url, e.rev);
            fetcher.checkout(d.name, e.url, e.commit);
        }
        auto prev = done.find(d.name);
        if (prev != done.end()) {
            const LockEntry& p = prev->second.first;
            bool same = p.kind == e.kind && (e.kind == "git" ? p.commit == e.commit : p.path == e.path);
            if (!same)
                throw PkgError("package '" + d.name + "' is required twice with different sources: " +
                               describe(p) + " by '" + prev->second.second + "' and " + describe(e) +
                               " by '" + it.requirer + "'");
            continue;
        }
        done[d.name] = {e, it.requirer};
        std::string depRoot = e.root(root.dir);
        std::error_code ec;
        if (fs::is_regular_file(depRoot + "/eskiu.toml", ec)) {
            Manifest m = readManifest(depRoot);
            for (const auto& dd : m.deps) queue.push_back({dd, d.name, depRoot});
        }
    }
    std::vector<LockEntry> entries;
    for (const auto& kv : done) entries.push_back(kv.second.first);
    if (!done.count(root.name)) {
        LockEntry self;
        self.name = root.name;
        self.kind = "root";
        self.path = ".";
        entries.push_back(self);
    }
    std::sort(entries.begin(), entries.end(),
              [](const LockEntry& a, const LockEntry& b) { return a.name < b.name; });
    writeLock(root.dir, entries);
    return entries;
}

// Whether every direct dependency of m has its lock entry: the same kind, URL and rev for a
// git one, the same directory for a path one. A dependency edited in eskiu.toml (git to a
// local checkout, another rev) does not, and the lock is resolved again.
bool lockMatchesManifest(const Manifest& m, const std::map<std::string, LockEntry>& lock) {
    for (const auto& d : m.deps) {
        if (d.name == m.name) continue;
        auto l = lock.find(d.name);
        if (l == lock.end()) return false;
        const LockEntry& e = l->second;
        if (d.isGit) {
            if (e.kind != "git" || e.url != d.git || e.rev != (d.rev.empty() ? "HEAD" : d.rev)) return false;
        } else {
            fs::path p(d.path);
            if (p.is_relative()) p = fs::path(m.dir) / p;
            std::error_code ec;
            if (e.kind != "path" || fs::weakly_canonical(p, ec).generic_string() != e.path) return false;
        }
    }
    return true;
}

Manifest manifestFromCwd() {
    std::string root = findPackageRoot(".");
    if (root.empty())
        throw PkgError("no eskiu.toml in this directory or above (create a package with: eskiuc new NAME)");
    return readManifest(root);
}

std::string selfExe(const char* argv0) {
    return llvm::sys::fs::getMainExecutable(argv0, (void*)&pkgCommand);
}

// ── Commands ─────────────────────────────────────────────────────────────────

int cmdNew(const std::vector<std::string>& args) {
    for (const auto& a : args)
        if (a.size() > 1 && a[0] == '-') throw PkgUsage("unknown option '" + a + "'");
    if (args.size() != 1) throw PkgUsage(args.empty() ? "missing the package NAME" : "takes one NAME");
    const std::string& name = args[0];
    if (!validName(name)) throw PkgError("invalid package name '" + name + "' (letters, digits and '_')");
    std::error_code ec;
    if (fs::exists(name, ec)) throw PkgError("'" + name + "' already exists");
    fs::create_directories(name + "/src", ec);
    if (ec) throw PkgError("cannot create " + name + "/src: " + ec.message());
    bool ok = writeText(name + "/eskiu.toml",
                        "[package]\nname = \"" + name + "\"\nversion = \"0.1.0\"\n"
                        "main = \"src/main.esk\"\n\n[deps]\n") &&
              writeText(name + "/src/main.esk",
                        "import <io>;\n\nint main() {\n    println(\"Hello from " + name +
                            "!\");\n    return 0;\n}\n") &&
              writeText(name + "/.gitignore", "/build/\n");
    if (!ok) throw PkgError("cannot write the files of " + name);
    std::cout << "created package '" << name << "' in " << name << "/" << std::endl;
    return 0;
}

std::string tomlString(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out + "\"";
}

int cmdAdd(const std::vector<std::string>& args) {
    std::string name, git, rev, path;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw PkgUsage(a + " needs a value");
            return args[++i];
        };
        if (a == "--git") git = next();
        else if (a == "--rev") rev = next();
        else if (a == "--path") path = next();
        else if (!a.empty() && a[0] == '-') throw PkgUsage("unknown option '" + a + "'");
        else if (name.empty()) name = a;
        else throw PkgUsage("unexpected argument '" + a + "'");
    }
    if (name.empty()) throw PkgUsage("missing the package NAME");
    if (git.empty() == path.empty()) throw PkgUsage("needs one of --git URL and --path DIR");
    if (!rev.empty() && git.empty()) throw PkgUsage("--rev goes with --git");
    if (!validName(name)) throw PkgError("invalid package name '" + name + "' (letters, digits and '_')");
    Manifest m = manifestFromCwd();
    if (name == m.name) throw PkgError("a package cannot depend on itself ('" + name + "')");
    std::string entry = name + " = { ";
    if (!git.empty()) {
        entry += "git = " + tomlString(git);
        if (!rev.empty()) entry += ", rev = " + tomlString(rev);
    } else {
        // A relative path is written relative to the manifest, as it is read.
        std::error_code ec;
        fs::path p(path);
        if (p.is_relative()) {
            fs::path rel = fs::relative(fs::weakly_canonical(fs::absolute(p, ec), ec), m.dir, ec);
            if (!ec && !rel.empty()) p = rel;
        }
        entry += "path = " + tomlString(p.generic_string());
    }
    entry += " }";

    std::string file = m.dir + "/eskiu.toml";
    std::string old = readText(file);
    std::vector<std::string> lines;
    {
        std::istringstream in(old);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
    }
    auto trimmed = [](const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    int header = -1;
    for (size_t i = 0; i < lines.size(); ++i)
        if (trimmed(lines[i]) == "[deps]") { header = (int)i; break; }
    if (header < 0) {
        if (!lines.empty() && !trimmed(lines.back()).empty()) lines.push_back("");
        lines.push_back("[deps]");
        lines.push_back(entry);
    } else {
        int insertAt = header + 1, replace = -1;
        for (size_t i = header + 1; i < lines.size(); ++i) {
            std::string t = trimmed(lines[i]);
            if (!t.empty() && t[0] == '[') break;
            if (t.empty() || t[0] == '#') continue;
            insertAt = (int)i + 1;
            size_t k = t.find_first_of(" \t=");
            if (t.substr(0, k) == name) replace = (int)i;
        }
        if (replace >= 0) lines[replace] = entry;
        else lines.insert(lines.begin() + insertAt, entry);
    }
    std::string text;
    for (const auto& l : lines) text += l + "\n";
    if (!writeText(file, text)) throw PkgError("cannot write " + displayPath(file));
    try {
        auto entries = resolveAndLock(readManifest(m.dir), false);
        for (const auto& e : entries)
            if (e.name == name)
                std::cout << "added " << name << " ("
                          << (e.kind == "git" ? "commit " + e.commit.substr(0, 12) : e.path) << ")"
                          << std::endl;
    } catch (...) {
        writeText(file, old);
        throw;
    }
    return 0;
}

int cmdFetch(const std::vector<std::string>& args) {
    if (!args.empty())
        throw PkgUsage(args[0][0] == '-' ? "unknown option '" + args[0] + "'"
                                         : "unexpected argument '" + args[0] + "'");
    Manifest m = manifestFromCwd();
    for (const auto& e : resolveAndLock(m, true)) {
        if (e.kind == "root") continue;
        std::cout << e.name << "\t" << (e.kind == "git" ? e.commit : e.path) << std::endl;
    }
    fetchNatives(m.dir);
    return 0;
}

// Compile `file` to `out` with this compiler; returns its exit code.
int compileWithSelf(const std::string& self, const std::string& file, const std::string& out,
                    const std::vector<std::string>& flags) {
    std::vector<std::string> a = {file, "-o", out};
    a.insert(a.end(), flags.begin(), flags.end());
    return runExecutable(self, a);
}

bool endsWithEsk(const std::string& s) {
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".esk") == 0;
}

// Build the package's main program, or the program `file.esk` names in its place (an
// example or a tool of the package, with the package's dependencies); `out` receives the
// executable path.
int buildPackage(const char* argv0, const std::vector<std::string>& args, std::string& out) {
    std::vector<std::string> flags;
    std::string program;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) throw PkgUsage("-o needs a value");
            out = args[++i];
        } else if (!args[i].empty() && args[i][0] == '-') {
            size_t first = i;
            checkCompilerFlag(args, i);
            for (size_t k = first; k <= i; ++k) flags.push_back(args[k]);
        } else if (endsWithEsk(args[i])) {
            if (!program.empty())
                throw PkgError("eskiuc build takes one program file; to compile several files "
                               "into one program, run eskiuc " + program + " " + args[i] + " -o OUT");
            program = args[i];
        } else {
            throw PkgUsage("unexpected argument '" + args[i] + "' (the program is a .esk file)");
        }
    }
    Manifest m = manifestFromCwd();
    std::error_code ec;
    if (!program.empty() && !fs::is_regular_file(program, ec))
        throw PkgError("no file " + program);
    resolveAndLock(m, false);
    std::string main = program.empty() ? m.dir + "/" + m.main : program;
    if (!fs::is_regular_file(main, ec))
        throw PkgError("main file " + displayPath(main) + " not found (set main in [package])");
    if (out.empty()) {
        fs::create_directories(m.dir + "/build", ec);
        out = m.dir + "/build/" + (program.empty() ? m.name : fs::path(program).stem().string());
    }
    return compileWithSelf(selfExe(argv0), main, out, flags);
}

int cmdTest(const char* argv0, const std::vector<std::string>& flags) {
    for (size_t i = 0; i < flags.size(); ++i) {
        if (flags[i].empty() || flags[i][0] != '-') throw PkgUsage("unexpected argument '" + flags[i] + "'");
        checkCompilerFlag(flags, i);
    }
    Manifest m = manifestFromCwd();
    resolveAndLock(m, false);
    std::vector<std::string> tests;
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(m.dir + "/tests", ec))
        if (ent.path().extension() == ".esk" && ent.is_regular_file(ec))
            tests.push_back(ent.path().generic_string());
    std::sort(tests.begin(), tests.end());
    if (tests.empty()) {
        std::cout << "no tests in " << displayPath(m.dir + "/tests") << std::endl;
        return 0;
    }
    fs::create_directories(m.dir + "/build/tests", ec);
    std::string self = selfExe(argv0);
    int passed = 0, failed = 0;
    for (const auto& t : tests) {
        std::string stem = fs::path(t).stem().string();
        std::string exe = m.dir + "/build/tests/" + stem;
        int rc = compileWithSelf(self, t, exe, flags);
        if (rc != 0) {
            std::cout << "FAIL " << stem << " (build failed)" << std::endl;
            ++failed;
            continue;
        }
        rc = runExecutable(exe, {});
        if (rc == 0) {
            std::cout << "PASS " << stem << std::endl;
            ++passed;
        } else {
            std::cout << "FAIL " << stem << " (exit " << rc << ")" << std::endl;
            ++failed;
        }
    }
    std::cout << passed << " passed, " << failed << " failed" << std::endl;
    return failed ? 1 : 0;
}

}  // namespace

bool pkgCommand(int argc, char** argv, int& rc) {
    if (argc < 2) return false;
    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);
    if (cmd == "run") {
        // `eskiuc run script.esk ...` keeps compiling the script; with no .esk file
        // named, inside a package it builds and runs the package.
        for (const auto& a : args) {
            std::error_code ec;
            if (endsWithEsk(a) && fs::is_regular_file(a, ec)) return false;
        }
        if (findPackageRoot(".").empty()) return false;
    } else if (cmd != "new" && cmd != "add" && cmd != "build" && cmd != "fetch" && cmd != "test") {
        return false;
    }
    try {
        if (cmd == "new") rc = cmdNew(args);
        else if (cmd == "add") rc = cmdAdd(args);
        else if (cmd == "fetch") rc = cmdFetch(args);
        else if (cmd == "test") rc = cmdTest(argv[0], args);
        else if (cmd == "build") {
            std::string out;
            rc = buildPackage(argv[0], args, out);
        } else {
            // A leading --dev builds with the development profile; the rest (after an
            // optional `--`) are the program's arguments.
            std::vector<std::string> flags;
            if (!args.empty() && args[0] == "--dev") {
                flags.push_back(args[0]);
                args.erase(args.begin());
            }
            if (!args.empty() && args[0] == "--") args.erase(args.begin());
            std::string out;
            rc = buildPackage(argv[0], flags, out);
            if (rc == 0) rc = runExecutable(out, args);
        }
    } catch (const PkgUsage& e) {
        rc = cliUsageError(cmd, e.what());
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        rc = 1;
    }
    return true;
}

namespace {
std::vector<HostNative> packageNatives;   // this host's [native.NAME] archives, by pkgLoadRoots
bool packageTargetIsHost = true;          // the build runs here: a system library can stand in
std::vector<WebAsset> packageWebAssets;
}

void pkgLoadRoots(const std::string& input, const std::string& triple) {
    packageRoots = PackageRoots();
    packageNatives.clear();
    packageWebAssets.clear();
    packageTargetIsHost = triple.empty() || targetPlatform(triple) == hostPlatform();
    std::error_code ec;
    fs::path in = fs::absolute(input, ec);
    std::string root = findPackageRoot(in.parent_path().generic_string());
    if (root.empty()) return;
    packageRoots.active = true;
    std::map<std::string, LockEntry> lock;
    try {
        lock = readLock(root);
        // eskiu.toml edited since the lock was written: resolve it again, as eskiuc build
        // does (a git dependency whose URL and rev did not change needs no git call).
        Manifest m;
        bool manifestOk = true;
        try {
            m = readManifest(root);
        } catch (const PkgError&) {
            manifestOk = false;
        }
        if (manifestOk && !lockMatchesManifest(m, lock)) {
            resolveAndLock(m, false);
            lock = readLock(root);
        }
        for (const auto& kv : lock) packageRoots.roots[kv.first] = kv.second.root(root);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        exit(1);
    }
    // The native libraries, fetched only when a build links one (pkgNativeLibDirs). A
    // manifest error leaves them out here; the package commands report it.
    try {
        packageNatives = hostNatives(root, lock, triple);
    } catch (const std::exception&) {
    }
    for (const auto& kv : lock) {
        std::string dir = kv.second.root(root);
        if (!fs::is_regular_file(dir + "/eskiu.toml", ec)) continue;
        try {
            for (const auto& a : readManifest(dir).webAssets) packageWebAssets.push_back({kv.first, dir, a});
        } catch (const std::exception&) {
        }
    }
}

std::vector<std::string> pkgNativeLibDirs(const std::vector<std::string>& linked,
                                          std::vector<std::string>* systemDirs) {
    std::vector<std::string> dirs;
    std::set<std::string> want(linked.begin(), linked.end());
    auto useSystem = [&](const std::string& dir) {
        if (systemDirs && !dir.empty() &&
            std::find(systemDirs->begin(), systemDirs->end(), dir) == systemDirs->end())
            systemDirs->push_back(dir);
    };
    for (const auto& n : packageNatives) {
        if (!want.count(n.lib)) continue;
        if (pkgPreferSystemLib(n.lib)) {
            // ESKIU_SYSTEM_LIBS: the linker finds it (a cross build gets it from its own -L).
            if (packageTargetIsHost) useSystem(pkgFindSystemLib(n.lib));
            continue;
        }
        try {
            fetchNative(n.pkg, n.lib, n.src);
        } catch (const PkgError& e) {
            std::string file;
            std::string dir = packageTargetIsHost ? pkgFindSystemLib(n.lib, &file) : "";
            if (dir.empty()) throw;
            std::cerr << "warning: " << e.what() << "; linking the system's " << file << " instead" << std::endl;
            useSystem(dir);
            continue;
        }
        dirs.push_back(nativeDir(n.src) + "/" + n.src.lib);
    }
    return dirs;
}

void pkgCopyWebAssets(const std::string& page) {
    std::error_code ec;
    fs::path to = fs::absolute(page, ec).parent_path();
    std::set<std::string> copied;
    for (const auto& a : packageWebAssets) {
        fs::path from(a.path());
        std::string name = from.filename().string();
        if (!fs::exists(from, ec)) {
            std::cerr << "warning: web asset " << displayPath(a.path()) << " of package '" << a.pkg
                      << "' ([web] assets in its eskiu.toml) does not exist" << std::endl;
            continue;
        }
        if (!copied.insert(name).second) {
            std::cerr << "warning: web asset " << name << " of package '" << a.pkg
                      << "' has the name of another package's; keeping the first" << std::endl;
            continue;
        }
        copyWebAsset(a, to, name);
    }
}

std::vector<KnownNative> pkgKnownNatives(const std::string& lib) {
    std::vector<KnownNative> out;
    std::string platform = hostPlatform();
    if (platform.empty()) return out;
    std::set<std::string> seen;
    auto scan = [&](const std::string& pkg, const std::string& dir) {
        try {
            for (const auto& n : readManifest(dir).natives) {
                auto it = n.platforms.find(platform);
                if (n.name != lib || it == n.platforms.end() || !seen.insert(it->second.sha256).second) continue;
                std::error_code ec;
                KnownNative k;
                k.pkg = pkg;
                k.url = it->second.url;
                k.dir = nativeDir(it->second) + "/" + it->second.lib;
                k.cached = fs::is_directory(k.dir, ec);
                out.push_back(k);
            }
        } catch (const std::exception&) {
        }
    };
    std::error_code ec;
    std::string root = findPackageRoot(".");
    if (!root.empty()) {
        try {
            for (const auto& kv : readLock(root)) scan(kv.first, kv.second.root(root));
        } catch (const std::exception&) {
        }
    }
    for (const auto& pkg : fs::directory_iterator(pkgHome() + "/pkgs", ec))
        for (const auto& rev : fs::directory_iterator(pkg.path(), ec))
            if (fs::is_regular_file(rev.path() / "eskiu.toml", ec))
                scan(pkg.path().filename().string(), rev.path().generic_string());
    return out;
}

void copyNativeDlls(const std::vector<std::string>& dirs, const std::string& exe) {
    std::error_code ec;
    fs::path to = fs::absolute(exe, ec).parent_path();
    for (const auto& d : dirs)
        for (const auto& ent : fs::directory_iterator(d, ec)) {
            std::string ext = ent.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".dll")
                fs::copy_file(ent.path(), to / ent.path().filename(),
                              fs::copy_options::update_existing, ec);
        }
}
