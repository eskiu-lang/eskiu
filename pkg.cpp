// The package manager (see pkg.h).
//
// eskiu.toml is read by a small TOML subset reader (tables, string values, inline
// tables of strings, comments). Dependencies are resolved breadth-first from the root
// manifest, each package's own eskiu.toml adding its dependencies; the result is written
// to eskiu.lock, one tab-separated line per package, sorted by name:
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

namespace {

struct PkgError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ── TOML subset ──────────────────────────────────────────────────────────────

struct TomlValue {
    int line = 0;
    bool isTable = false;
    std::string str;
    std::vector<std::pair<std::string, std::string>> table;   // inline table entries
};

// Table name ("" before the first header) -> its keys in order.
using TomlDoc = std::map<std::string, std::vector<std::pair<std::string, TomlValue>>>;

class TomlReader {
public:
    TomlReader(const std::string& src, const std::string& file) : src(src), file(file) {}

    TomlDoc parse() {
        TomlDoc doc;
        std::string current;
        std::set<std::string> headers;
        doc[current];
        while (i < src.size()) {
            skipSpace();
            if (i >= src.size()) break;
            if (src[i] == '\n' || src[i] == '\r' || src[i] == '#') { endLine(); continue; }
            if (src[i] == '[') {
                ++i;
                skipSpace();
                std::string name = key();
                skipSpace();
                while (i < src.size() && src[i] == '.') {
                    ++i;
                    name += "." + key();
                    skipSpace();
                }
                if (i >= src.size() || src[i] != ']') err("expected ']' after table name");
                ++i;
                if (!headers.insert(name).second) err("table [" + name + "] is defined twice");
                current = name;
                doc[current];
                endLine();
                continue;
            }
            std::string k = key();
            skipSpace();
            if (i >= src.size() || src[i] != '=') err("expected '=' after key '" + k + "'");
            ++i;
            TomlValue v = value();
            for (const auto& kv : doc[current])
                if (kv.first == k) err("duplicate key '" + k + "'");
            doc[current].push_back({k, v});
            endLine();
        }
        return doc;
    }

private:
    const std::string& src;
    std::string file;
    size_t i = 0;
    int line = 1;

    [[noreturn]] void err(const std::string& msg) {
        throw PkgError(file + ":" + std::to_string(line) + ": " + msg);
    }
    void skipSpace() {
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
    }
    void endLine() {
        skipSpace();
        if (i < src.size() && src[i] == '#')
            while (i < src.size() && src[i] != '\n') ++i;
        if (i < src.size() && src[i] == '\r') ++i;
        if (i < src.size() && src[i] != '\n')
            err(std::string("unexpected '") + src[i] + "'");
        if (i < src.size()) { ++i; ++line; }
    }
    static bool bareKeyChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    }
    std::string key() {
        skipSpace();
        if (i < src.size() && (src[i] == '"' || src[i] == '\'')) return quoted();
        size_t start = i;
        while (i < src.size() && bareKeyChar(src[i])) ++i;
        if (i == start) err("expected a key");
        return src.substr(start, i - start);
    }
    std::string quoted() {
        char q = src[i++];
        std::string out;
        while (true) {
            if (i >= src.size() || src[i] == '\n' || src[i] == '\r') err("unterminated string");
            char c = src[i++];
            if (c == q) break;
            if (q == '"' && c == '\\') {
                if (i >= src.size()) err("unterminated string");
                char e = src[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    default: err(std::string("unknown escape '\\") + e + "'");
                }
                continue;
            }
            out += c;
        }
        return out;
    }
    TomlValue value() {
        skipSpace();
        TomlValue v;
        v.line = line;
        if (i < src.size() && (src[i] == '"' || src[i] == '\'')) {
            v.str = quoted();
            return v;
        }
        if (i < src.size() && src[i] == '{') {
            ++i;
            v.isTable = true;
            skipSpace();
            if (i < src.size() && src[i] == '}') { ++i; return v; }
            while (true) {
                std::string k = key();
                skipSpace();
                if (i >= src.size() || src[i] != '=') err("expected '=' after key '" + k + "'");
                ++i;
                skipSpace();
                if (i >= src.size() || (src[i] != '"' && src[i] != '\''))
                    err("expected a string value for '" + k + "'");
                std::string s = quoted();
                for (const auto& kv : v.table)
                    if (kv.first == k) err("duplicate key '" + k + "'");
                v.table.push_back({k, s});
                skipSpace();
                if (i < src.size() && src[i] == ',') { ++i; continue; }
                if (i < src.size() && src[i] == '}') { ++i; break; }
                err("expected ',' or '}' in an inline table");
            }
            return v;
        }
        err("expected a string or an inline table");
    }
};

// ── Manifest ─────────────────────────────────────────────────────────────────

struct Dep {
    std::string name;
    bool isGit = false;
    std::string git, rev, path;
};

// One prebuilt archive of a native library, for one platform.
struct NativeSource {
    std::string url, sha256, lib = "lib";
};

struct NativeLib {
    std::string name;
    std::map<std::string, NativeSource> platforms;   // "macos-arm64" -> its archive
};

struct Manifest {
    std::string dir;     // absolute directory holding eskiu.toml
    std::string name, version, main;
    std::vector<Dep> deps;
    std::vector<NativeLib> natives;
};

const char* const kNativePlatforms[] = {"macos-arm64",         "macos-x86_64",        "linux-x86_64",
                                        "linux-arm64",         "windows-x86_64",      "windows-arm64",
                                        "ios-arm64",           "ios-arm64-simulator", "ios-x86_64-simulator",
                                        "android-arm64",       "android-x86_64"};

// The platform key of the machine eskiuc runs on ("" when no key names it).
std::string hostPlatform() {
#if defined(__APPLE__)
    std::string os = "macos";
#elif defined(_WIN32)
    std::string os = "windows";
#elif defined(__linux__)
    std::string os = "linux";
#else
    return "";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    return os + "-arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return os + "-x86_64";
#else
    return "";
#endif
}

// The platform key of `triple` (or of the machine eskiuc runs on when `triple` is empty;
// "" when no key names it).
std::string targetPlatform(const std::string& triple) {
    if (triple.empty()) return hostPlatform();
    std::string arch;
    if (triple.rfind("aarch64", 0) == 0 || triple.rfind("arm64", 0) == 0) arch = "arm64";
    else if (triple.rfind("x86_64", 0) == 0 || triple.rfind("amd64", 0) == 0) arch = "x86_64";
    else return "";
    if (triple.find("-ios") != std::string::npos) {
        if (triple.find("simulator") != std::string::npos) return "ios-" + arch + "-simulator";
        return arch == "arm64" ? "ios-arm64" : "";
    }
    if (triple.find("android") != std::string::npos) return "android-" + arch;
    if (triple.find("apple") != std::string::npos || triple.find("darwin") != std::string::npos ||
        triple.find("macos") != std::string::npos)
        return "macos-" + arch;
    if (triple.find("windows") != std::string::npos || triple.find("win32") != std::string::npos ||
        triple.find("mingw") != std::string::npos)
        return "windows-" + arch;
    if (triple.find("linux") != std::string::npos) return "linux-" + arch;
    return "";
}

bool validName(const std::string& n) {
    if (n.empty() || !(std::isalpha((unsigned char)n[0]) || n[0] == '_')) return false;
    for (char c : n)
        if (!(std::isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

// `path` as the user should read it: relative to the current directory when inside it.
std::string displayPath(const std::string& path) {
    std::error_code ec;
    fs::path rel = fs::relative(path, fs::current_path(ec), ec);
    if (!ec && !rel.empty() && rel.generic_string().rfind("..", 0) != 0) return rel.generic_string();
    return path;
}

std::string readText(const std::string& path, bool* ok = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (ok) *ok = f.is_open();
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    f << text;
    return bool(f);
}

Manifest readManifest(const std::string& dir) {
    std::string file = dir + "/eskiu.toml";
    std::string shown = displayPath(file);
    bool ok = false;
    std::string text = readText(file, &ok);
    if (!ok) throw PkgError("cannot read " + shown);
    TomlDoc doc = TomlReader(text, shown).parse();
    auto fail = [&](int line, const std::string& msg) -> PkgError {
        return PkgError(shown + ":" + std::to_string(line) + ": " + msg);
    };
    Manifest m;
    m.dir = dir;
    m.main = "src/main.esk";
    for (const auto& kv : doc[""])
        throw fail(kv.second.line, "key '" + kv.first + "' outside a table");
    bool sawName = false;
    for (const auto& kv : doc["package"]) {
        if (kv.second.isTable)
            throw fail(kv.second.line, "'" + kv.first + "' must be a string");
        if (kv.first == "name") {
            if (!validName(kv.second.str))
                throw fail(kv.second.line, "invalid package name '" + kv.second.str +
                                               "' (letters, digits and '_')");
            m.name = kv.second.str;
            sawName = true;
        } else if (kv.first == "version") {
            m.version = kv.second.str;
        } else if (kv.first == "main") {
            m.main = kv.second.str;
        } else {
            throw fail(kv.second.line, "unknown key '" + kv.first + "' in [package]");
        }
    }
    if (!sawName) throw fail(1, "missing name in [package]");
    for (const auto& kv : doc["deps"]) {
        const TomlValue& v = kv.second;
        if (!validName(kv.first))
            throw fail(v.line, "invalid package name '" + kv.first + "' (letters, digits and '_')");
        if (!v.isTable)
            throw fail(v.line, "dependency '" + kv.first +
                                   "' must be an inline table: { git = \"URL\" } or { path = \"DIR\" }");
        Dep d;
        d.name = kv.first;
        bool hasGit = false, hasPath = false, hasRev = false;
        for (const auto& e : v.table) {
            if (e.first == "git") { d.git = e.second; hasGit = true; }
            else if (e.first == "rev") { d.rev = e.second; hasRev = true; }
            else if (e.first == "path") { d.path = e.second; hasPath = true; }
            else throw fail(v.line, "unknown key '" + e.first + "' in dependency '" + kv.first + "'");
        }
        if (hasGit == hasPath)
            throw fail(v.line, "dependency '" + kv.first + "' needs exactly one of git or path");
        if (hasRev && !hasGit)
            throw fail(v.line, "dependency '" + kv.first + "': rev needs git");
        d.isGit = hasGit;
        m.deps.push_back(d);
    }
    for (const auto& t : doc) {
        if (t.first.rfind("native.", 0) != 0) continue;
        NativeLib lib;
        lib.name = t.first.substr(7);
        if (!validName(lib.name))
            throw PkgError(shown + ": invalid native library name '" + lib.name + "' in [" + t.first + "]");
        for (const auto& kv : t.second) {
            const TomlValue& v = kv.second;
            bool known = false;
            for (const char* p : kNativePlatforms) known = known || kv.first == p;
            if (!known)
                throw fail(v.line, "unknown platform '" + kv.first + "' in [" + t.first +
                                       "] (macos-arm64, macos-x86_64, linux-x86_64, linux-arm64, "
                                       "windows-x86_64, windows-arm64, ios-arm64, ios-arm64-simulator, "
                                       "ios-x86_64-simulator, android-arm64, android-x86_64)");
            if (!v.isTable)
                throw fail(v.line, "'" + kv.first + "' in [" + t.first +
                                       "] must be an inline table: { url = \"URL\", sha256 = \"HEX\" }");
            NativeSource src;
            for (const auto& e : v.table) {
                if (e.first == "url") src.url = e.second;
                else if (e.first == "sha256") src.sha256 = e.second;
                else if (e.first == "lib") src.lib = e.second;
                else throw fail(v.line, "unknown key '" + e.first + "' in [" + t.first + "] " + kv.first);
            }
            if (src.url.empty()) throw fail(v.line, "[" + t.first + "] " + kv.first + " needs a url");
            bool hex = src.sha256.size() == 64;
            for (char& c : src.sha256) {
                c = (char)std::tolower((unsigned char)c);
                hex = hex && std::isxdigit((unsigned char)c);
            }
            if (!hex)
                throw fail(v.line, "[" + t.first + "] " + kv.first + " needs a sha256 of 64 hex digits");
            if (src.lib.empty() || fs::path(src.lib).is_absolute() ||
                src.lib.find("..") != std::string::npos)
                throw fail(v.line, "[" + t.first + "] " + kv.first +
                                       ": lib must be a directory inside the archive");
            lib.platforms[kv.first] = src;
        }
        m.natives.push_back(lib);
    }
    for (const auto& t : doc)
        if (!t.first.empty() && t.first != "package" && t.first != "deps" &&
            t.first.rfind("native.", 0) != 0)
            throw PkgError(shown + ": unknown table [" + t.first + "]");
    return m;
}

// The directory holding eskiu.toml: `start` or the nearest one above it ("" if none).
std::string findPackageRoot(const std::string& start) {
    std::error_code ec;
    fs::path p = fs::absolute(start, ec);
    if (ec) return "";
    p = fs::weakly_canonical(p, ec);
    if (ec) return "";
    while (true) {
        if (fs::is_regular_file(p / "eskiu.toml", ec)) {
            std::string s = p.generic_string();
            if (s.size() > 1 && s.back() == '/') s.pop_back();
            return s;
        }
        fs::path parent = p.parent_path();
        if (parent == p || parent.empty()) return "";
        p = parent;
    }
}

std::string pkgHome() {
    if (const char* h = std::getenv("ESKIU_HOME"); h && *h) return h;
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) home = std::getenv("USERPROFILE");
#endif
    return std::string(home ? home : ".") + "/.eskiu";
}

// ── Lock ─────────────────────────────────────────────────────────────────────

struct LockEntry {
    std::string name, kind, url, rev, commit, path;
    std::string root(const std::string& lockDir) const {
        if (kind == "git") return pkgHome() + "/pkgs/" + name + "/" + commit;
        if (kind == "root") return lockDir;
        return path;
    }
};

std::map<std::string, LockEntry> readLock(const std::string& dir) {
    std::map<std::string, LockEntry> out;
    bool ok = false;
    std::string text = readText(dir + "/eskiu.lock", &ok);
    if (!ok) return out;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t s = 0;
        while (true) {
            size_t t = line.find('\t', s);
            f.push_back(line.substr(s, t == std::string::npos ? std::string::npos : t - s));
            if (t == std::string::npos) break;
            s = t + 1;
        }
        LockEntry e;
        e.name = f[0];
        e.kind = f.size() > 1 ? f[1] : "";
        if (e.kind == "git" && f.size() == 5) { e.url = f[2]; e.rev = f[3]; e.commit = f[4]; }
        else if ((e.kind == "path" || e.kind == "root") && f.size() == 3) {
            // A path dependency is stored relative to the lock's directory, so a lock
            // committed with the package works on another machine.
            e.path = f[2];
            if (e.kind == "path" && fs::path(e.path).is_relative()) {
                std::error_code ec;
                e.path = fs::weakly_canonical(fs::path(dir) / e.path, ec).generic_string();
            }
        }
        else throw PkgError(displayPath(dir + "/eskiu.lock") + ":" + std::to_string(lineNo) +
                            ": malformed entry (run eskiuc fetch)");
        out[e.name] = e;
    }
    return out;
}

void writeLock(const std::string& dir, const std::vector<LockEntry>& entries) {
    std::string text = "# eskiu.lock: written by eskiuc (eskiuc fetch updates it); do not edit.\n";
    for (const auto& e : entries) {
        text += e.name + "\t" + e.kind + "\t";
        if (e.kind == "git") {
            text += e.url + "\t" + e.rev + "\t" + e.commit;
        } else if (e.kind == "path") {
            std::error_code ec;
            fs::path rel = fs::relative(e.path, fs::weakly_canonical(dir, ec), ec);
            text += ec || rel.empty() ? e.path : rel.generic_string();
        } else {
            text += e.path;
        }
        text += "\n";
    }
    bool ok = false;
    if (readText(dir + "/eskiu.lock", &ok) == text && ok) return;
    if (!writeText(dir + "/eskiu.lock", text))
        throw PkgError("cannot write " + displayPath(dir + "/eskiu.lock"));
}

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

// Download, check and unpack `src` into nativeDir(src) unless it is already there.
void fetchNative(const std::string& pkg, const std::string& lib, const NativeSource& src) {
    std::string dir = nativeDir(src);
    std::error_code ec;
    if (fs::is_directory(dir, ec)) return;
    std::string what = "native library '" + lib + "' of package '" + pkg + "'";
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
    for (const auto& n : hostNatives(lockDir, readLock(lockDir))) fetchNative(n.pkg, n.lib, n.src);
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
    if (args.size() != 1) throw PkgError("usage: eskiuc new NAME");
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
            if (i + 1 >= args.size()) throw PkgError(a + " needs a value");
            return args[++i];
        };
        if (a == "--git") git = next();
        else if (a == "--rev") rev = next();
        else if (a == "--path") path = next();
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
        else throw PkgError("usage: eskiuc add NAME --git URL [--rev REV] | --path DIR");
    }
    if (name.empty() || git.empty() == path.empty() || (!rev.empty() && git.empty()))
        throw PkgError("usage: eskiuc add NAME --git URL [--rev REV] | --path DIR");
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
    if (!args.empty()) throw PkgError("usage: eskiuc fetch");
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

// Build the package's main program; `out` receives the executable path.
int buildPackage(const char* argv0, const std::vector<std::string>& args, std::string& out) {
    std::vector<std::string> flags;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (i + 1 >= args.size()) throw PkgError("-o needs a value");
            out = args[++i];
        } else {
            flags.push_back(args[i]);
        }
    }
    Manifest m = manifestFromCwd();
    resolveAndLock(m, false);
    std::string main = m.dir + "/" + m.main;
    std::error_code ec;
    if (!fs::is_regular_file(main, ec))
        throw PkgError("main file " + displayPath(main) + " not found (set main in [package])");
    if (out.empty()) {
        fs::create_directories(m.dir + "/build", ec);
        out = m.dir + "/build/" + m.name;
    }
    return compileWithSelf(selfExe(argv0), main, out, flags);
}

int cmdTest(const char* argv0, const std::vector<std::string>& flags) {
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

bool endsWithEsk(const std::string& s) {
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".esk") == 0;
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
            if (!args.empty() && args[0] == "--") args.erase(args.begin());
            std::string out;
            rc = buildPackage(argv[0], {}, out);
            if (rc == 0) rc = runExecutable(out, args);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        rc = 1;
    }
    return true;
}

namespace {
std::vector<HostNative> packageNatives;   // this host's [native.NAME] archives, by pkgLoadRoots
}

void pkgLoadRoots(const std::string& input, const std::string& triple) {
    packageRoots = PackageRoots();
    packageNatives.clear();
    std::error_code ec;
    fs::path in = fs::absolute(input, ec);
    std::string root = findPackageRoot(in.parent_path().generic_string());
    if (root.empty()) return;
    packageRoots.active = true;
    std::map<std::string, LockEntry> lock;
    try {
        lock = readLock(root);
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
}

std::vector<std::string> pkgNativeLibDirs(const std::vector<std::string>& linked) {
    std::vector<std::string> dirs;
    std::set<std::string> want(linked.begin(), linked.end());
    for (const auto& n : packageNatives) {
        if (!want.count(n.lib)) continue;
        fetchNative(n.pkg, n.lib, n.src);
        dirs.push_back(nativeDir(n.src) + "/" + n.src.lib);
    }
    return dirs;
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
