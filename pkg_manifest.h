// pkg_manifest.h: the manifest, platforms and the lock file (internal to pkg.cpp, which includes it once, in order).
#pragma once
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "pkg.h"
#include "main_support.h"

namespace {

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
    std::vector<std::string> webAssets;   // [web] assets: files a web build copies next to its page
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

// Whether `s` holds a control character (a byte below 0x20, or 0x7f). eskiu.lock
// separates fields with tabs and entries with newlines, so a dependency's git URL,
// rev or path holding one could forge an entry: they are rejected.
bool hasControlChar(const std::string& s) {
    for (unsigned char c : s)
        if (c < 0x20 || c == 0x7f) return true;
    return false;
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
        if (kv.second.isTable || kv.second.isArray)
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
            if (hasControlChar(e.second))
                throw fail(v.line, "dependency '" + kv.first + "': " + e.first +
                                       " may not hold a control character (tab, newline, ...)");
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
    for (const auto& kv : doc["web"]) {
        const TomlValue& v = kv.second;
        if (kv.first != "assets") throw fail(v.line, "unknown key '" + kv.first + "' in [web] (assets)");
        if (!v.isArray)
            throw fail(v.line, "assets in [web] must be an array of paths: assets = [\"web/app.wasm\"]");
        for (const auto& a : v.items) {
            if (a.empty() || fs::path(a).is_absolute() || a.find("..") != std::string::npos)
                throw fail(v.line, "web asset '" + a + "' must be a path inside the package");
            m.webAssets.push_back(a);
        }
    }
    for (const auto& t : doc)
        if (!t.first.empty() && t.first != "package" && t.first != "deps" && t.first != "web" &&
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
        for (const std::string* f : {&e.name, &e.kind, &e.url, &e.rev, &e.commit, &e.path})
            if (hasControlChar(*f))
                throw PkgError("cannot write " + displayPath(dir + "/eskiu.lock") + ": package '" +
                               e.name + "' has a control character in its source");
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

}  // namespace
