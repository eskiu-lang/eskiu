// build_cache.h: the object cache (build_cache.cpp, docs/dev/compile-speed.md). A compile
// that writes an executable or an object file stores the object it emitted under a key:
// a hash of the compiler (version and executable), the target, the command line, the
// working directory and every source file the loader read (path and contents). The next
// compile with the same key copies the object instead of type checking, generating,
// optimizing and emitting it; linking still runs. The warnings the compile printed are
// stored with the object and printed again on a hit.
#pragma once
#include <cstdint>
#include <streambuf>
#include <string>
#include <vector>

namespace objcache {

// The loader reports each source file it reads (root files and imports, the standard
// library's and packages' included) while recording is on; turning it on forgets the
// files of an earlier load.
void setRecording(bool on);
void noteSource(const std::string& path, const std::string& text);

// The host's OS version where the native target triple carries it (macOS), else "".
std::string hostOSVersion();

// Whether this compile uses the cache: on with --cache or ESKIU_CACHE=1, off with
// --no-cache or ESKIU_NO_CACHE=1 (which win).
bool wanted(bool cacheFlag, bool noCacheFlag);

// $ESKIU_CACHE_DIR, else $ESKIU_HOME/cache/obj, else ~/.eskiu/cache/obj.
std::string directory();

// Builds a key: length-prefixed fields, then the recorded sources.
class Key {
public:
    Key();
    void add(const std::string& field);
    std::string finish();   // adds the recorded sources; 64 hex digits
private:
    std::string text_;
};

// The compiler's identity: $ESKIU_CACHE_COMPILER_ID when set (tests), else a hash of its
// executable's contents, computed once per executable (path, size, modification time)
// and kept in the cache directory.
std::string compilerIdentity();

// What the link needs to know about the program besides its object.
struct Meta {
    bool usesEH = false, usesThreads = false;
    std::string macosMin;      // -mmacosx-version-min for the link, or ""
    std::string diagnostics;   // the warnings the compile printed
};

// On a hit, copy the cached object to `objOut` and fill `meta`; false on a miss (or when
// the entry could not be read, as when another build evicts it meanwhile).
bool lookup(const std::string& key, const std::string& objOut, Meta& meta);

// Store `objFile` under `key` (temp files renamed into place, so concurrent builds are
// safe), then evict the least recently used entries past the size limit
// ($ESKIU_CACHE_SIZE in MiB, default 1024). Failures are silent: the cache is an
// optimization.
void store(const std::string& key, const std::string& objFile, const Meta& meta);

// While alive, everything written to std::cerr is also kept in `text()`.
class CerrCapture : public std::streambuf {
public:
    CerrCapture();
    ~CerrCapture() override;
    const std::string& text() const { return text_; }
protected:
    int overflow(int c) override;
    std::streamsize xsputn(const char* s, std::streamsize n) override;
    int sync() override;
private:
    std::streambuf* prev_;
    std::string text_;
};

}  // namespace objcache
