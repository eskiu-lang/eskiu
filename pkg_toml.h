// pkg_toml.h: package errors and the eskiu.toml subset reader (internal to pkg.cpp, which includes it once, in order).
#pragma once
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "pkg.h"
#include "main_support.h"

namespace {

struct PkgError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ── TOML subset ──────────────────────────────────────────────────────────────

struct TomlValue {
    int line = 0;
    bool isTable = false;
    bool isArray = false;
    std::string str;
    std::vector<std::pair<std::string, std::string>> table;   // inline table entries
    std::vector<std::string> items;                            // array entries
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
        if (i < src.size() && src[i] == '[') {
            // An array of strings, which may span lines and hold comments.
            ++i;
            v.isArray = true;
            auto skipBlank = [&]() {
                while (i < src.size()) {
                    char c = src[i];
                    if (c == ' ' || c == '\t' || c == '\r') ++i;
                    else if (c == '\n') { ++i; ++line; }
                    else if (c == '#') { while (i < src.size() && src[i] != '\n') ++i; }
                    else break;
                }
            };
            while (true) {
                skipBlank();
                if (i < src.size() && src[i] == ']') { ++i; break; }
                if (i >= src.size() || (src[i] != '"' && src[i] != '\''))
                    err("expected a string in an array");
                v.items.push_back(quoted());
                skipBlank();
                if (i < src.size() && src[i] == ',') { ++i; continue; }
                if (i < src.size() && src[i] == ']') { ++i; break; }
                err("expected ',' or ']' in an array");
            }
            return v;
        }
        err("expected a string, an array or an inline table");
    }
};

}  // namespace
