// build_cache.cpp: the object cache (see build_cache.h).
//
// An entry is two files in the cache directory: KEY.o, the object, and KEY.meta, what the
// link needs and the warnings to print again. Each is written to a unique temporary file
// and renamed into place, the object first, so a reader that finds KEY.meta finds a
// complete KEY.o (unless an eviction removes it meanwhile: the reader then compiles).
// A hit touches KEY.meta; eviction removes the entries with the oldest KEY.meta first.
#include "build_cache.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/BLAKE3.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace fs = std::filesystem;

namespace objcache {

namespace {

struct Source {
    std::string path;
    uint64_t size;
    std::string digest;
};

bool recording = false;
std::vector<Source>& sources() {
    static std::vector<Source> s;
    return s;
}

std::string hexHash(llvm::StringRef data) {
    auto r = llvm::BLAKE3::hash(llvm::ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t*>(data.data()), data.size()));
    return llvm::toHex(llvm::ArrayRef<uint8_t>(r.data(), r.size()), /*LowerCase=*/true);
}

bool envSet(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::string(v) != "0";
}

// Write `data` to `dir/name` through a unique temporary file renamed into place.
bool writeAtomic(const std::string& dir, const std::string& name, const std::string& data) {
    llvm::SmallString<256> tmp;
    int fd = -1;
    if (llvm::sys::fs::createUniqueFile(dir + "/tmp-%%%%%%%%%%%%", fd, tmp)) return false;
    {
        llvm::raw_fd_ostream out(fd, /*shouldClose=*/true);
        out << data;
        out.close();
        if (out.has_error()) {
            out.clear_error();
            llvm::sys::fs::remove(tmp);
            return false;
        }
    }
    if (llvm::sys::fs::rename(tmp, dir + "/" + name)) {
        llvm::sys::fs::remove(tmp);
        return false;
    }
    return true;
}

bool copyAtomic(const std::string& from, const std::string& dir, const std::string& name) {
    llvm::SmallString<256> tmp;
    int fd = -1;
    if (llvm::sys::fs::createUniqueFile(dir + "/tmp-%%%%%%%%%%%%", fd, tmp)) return false;
    llvm::sys::fs::closeFile(fd);
    if (llvm::sys::fs::copy_file(from, tmp) || llvm::sys::fs::rename(tmp, dir + "/" + name)) {
        llvm::sys::fs::remove(tmp);
        return false;
    }
    return true;
}

uint64_t sizeLimit() {
    if (const char* v = std::getenv("ESKIU_CACHE_SIZE"); v && *v) {
        char* end = nullptr;
        unsigned long long mib = std::strtoull(v, &end, 10);
        if (end && !*end) return (uint64_t)mib << 20;
    }
    return 1024ull << 20;
}

void evict(const std::string& dir) {
    struct Entry {
        std::string key;
        fs::file_time_type used;
        uint64_t bytes = 0;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    std::error_code ec;
    auto now = fs::file_time_type::clock::now();
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        std::string name = f.path().filename().string();
        if (!f.is_regular_file(ec)) { ec.clear(); continue; }
        uint64_t bytes = f.file_size(ec);
        if (ec) { ec.clear(); continue; }
        // A temporary file a killed build left behind.
        if (name.compare(0, 4, "tmp-") == 0) {
            if (now - f.last_write_time(ec) > std::chrono::hours(1)) fs::remove(f.path(), ec);
            ec.clear();
            continue;
        }
        total += bytes;
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".meta") == 0) {
            Entry e;
            e.key = name.substr(0, name.size() - 5);
            e.used = f.last_write_time(ec);
            std::error_code oec;
            e.bytes = bytes + fs::file_size(dir + "/" + e.key + ".o", oec);
            entries.push_back(e);
        }
    }
    uint64_t limit = sizeLimit();
    if (total <= limit) return;
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.used < b.used; });
    uint64_t target = limit / 10 * 9;
    for (const auto& e : entries) {
        if (total <= target) break;
        fs::remove(dir + "/" + e.key + ".meta", ec);
        fs::remove(dir + "/" + e.key + ".o", ec);
        total -= std::min(total, e.bytes);
    }
}

}  // namespace

void setRecording(bool on) {
    if (on) sources().clear();
    recording = on;
}

std::string hostOSVersion() {
#ifdef __APPLE__
    char buf[64] = {0};
    size_t len = sizeof(buf) - 1;
    if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0) return buf;
#endif
    return "";
}

void noteSource(const std::string& path, const std::string& text) {
    if (!recording) return;
    sources().push_back({path, (uint64_t)text.size(), hexHash(text)});
}

bool wanted(bool cacheFlag, bool noCacheFlag) {
    if (noCacheFlag || envSet("ESKIU_NO_CACHE")) return false;
    return cacheFlag || envSet("ESKIU_CACHE");
}

std::string directory() {
    if (const char* d = std::getenv("ESKIU_CACHE_DIR"); d && *d) return d;
    std::string home;
    if (const char* h = std::getenv("ESKIU_HOME"); h && *h) {
        home = h;
    } else {
        const char* u = std::getenv("HOME");
#ifdef _WIN32
        if (!u || !*u) u = std::getenv("USERPROFILE");
#endif
        home = std::string(u ? u : ".") + "/.eskiu";
    }
    return home + "/cache/obj";
}

Key::Key() { add("eskiu-objcache-1"); }

void Key::add(const std::string& field) {
    text_ += std::to_string(field.size());
    text_ += ':';
    text_ += field;
}

std::string Key::finish() {
    add("sources");
    add(std::to_string(sources().size()));
    for (const auto& s : sources()) {
        add(s.path);
        add(std::to_string(s.size));
        add(s.digest);
    }
    return hexHash(text_);
}

std::string compilerIdentity() {
    if (const char* id = std::getenv("ESKIU_CACHE_COMPILER_ID"); id && *id) return std::string("id:") + id;
    static int anchor;
    std::string exe = llvm::sys::fs::getMainExecutable("eskiuc", (void*)&anchor);
    llvm::sys::fs::file_status st;
    if (exe.empty() || llvm::sys::fs::status(exe, st)) return "";
    std::string stamp = exe + "|" + std::to_string(st.getSize()) + "|" +
                        std::to_string(st.getLastModificationTime().time_since_epoch().count());
    std::string dir = directory() + "/compiler-ids";
    std::string memo = hexHash(stamp);
    std::error_code ec;
    {
        std::ifstream in(dir + "/" + memo);
        std::string id;
        if (in >> id && id.size() == 64) return id;
    }
    auto buf = llvm::MemoryBuffer::getFile(exe, /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!buf) return "";
    std::string id = hexHash((*buf)->getBuffer());
    fs::create_directories(dir, ec);
    if (!ec) writeAtomic(dir, memo, id + "\n");
    return id;
}

bool lookup(const std::string& key, const std::string& objOut, Meta& meta) {
    std::string dir = directory();
    std::string metaPath = dir + "/" + key + ".meta";
    auto buf = llvm::MemoryBuffer::getFile(metaPath, /*IsText=*/false, /*RequiresNullTerminator=*/false);
    if (!buf) return false;
    std::istringstream in(std::string((*buf)->getBuffer()));
    std::string magic, word;
    size_t diagBytes = 0;
    int eh = 0, threads = 0;
    if (!std::getline(in, magic) || magic != "eskiu-objcache 1") return false;
    if (!(in >> word >> eh) || word != "uses-eh") return false;
    if (!(in >> word >> threads) || word != "uses-threads") return false;
    if (!(in >> word) || word != "macos-min") return false;
    in.get();
    std::getline(in, meta.macosMin);
    if (!(in >> word >> diagBytes) || word != "diagnostics") return false;
    in.get();
    meta.diagnostics.assign(diagBytes, '\0');
    if (diagBytes && !in.read(&meta.diagnostics[0], (std::streamsize)diagBytes)) return false;
    meta.usesEH = eh != 0;
    meta.usesThreads = threads != 0;
    if (llvm::sys::fs::copy_file(dir + "/" + key + ".o", objOut)) return false;
    std::error_code ec;
    fs::last_write_time(metaPath, fs::file_time_type::clock::now(), ec);
    return true;
}

void store(const std::string& key, const std::string& objFile, const Meta& meta) {
    std::string dir = directory();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return;
    std::string m = "eskiu-objcache 1\nuses-eh " + std::to_string(meta.usesEH ? 1 : 0) +
                    "\nuses-threads " + std::to_string(meta.usesThreads ? 1 : 0) +
                    "\nmacos-min " + meta.macosMin + "\ndiagnostics " +
                    std::to_string(meta.diagnostics.size()) + "\n" + meta.diagnostics;
    if (!copyAtomic(objFile, dir, key + ".o")) return;
    if (!writeAtomic(dir, key + ".meta", m)) return;
    evict(dir);
}

CerrCapture::CerrCapture() : prev_(std::cerr.rdbuf()) { std::cerr.rdbuf(this); }

CerrCapture::~CerrCapture() {
    std::cerr.flush();
    std::cerr.rdbuf(prev_);
}

int CerrCapture::overflow(int c) {
    if (c == EOF) return 0;
    text_ += (char)c;
    return prev_ ? prev_->sputc((char)c) : c;
}

std::streamsize CerrCapture::xsputn(const char* s, std::streamsize n) {
    text_.append(s, (size_t)n);
    return prev_ ? prev_->sputn(s, n) : n;
}

int CerrCapture::sync() { return prev_ ? prev_->pubsync() : 0; }

}  // namespace objcache
