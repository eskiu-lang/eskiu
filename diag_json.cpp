#include "diag_json.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <streambuf>
#include <vector>

bool g_jsonDiagnostics = false;

namespace {

bool isAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isIdentStart(char c) { return isAlpha(c) || c == '_'; }
bool isIdent(char c) { return isAlnum(c) || c == '_'; }

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

// `word` or `word-word...`: letters only.
bool isIdWord(const std::string& w) {
    if (w.empty() || !isAlpha(w[0]) || !isAlpha(w.back())) return false;
    for (size_t i = 0; i < w.size(); ++i) {
        if (isAlpha(w[i])) continue;
        if (w[i] != '-' || !isAlpha(w[i + 1])) return false;
    }
    return true;
}

void jsonString(std::string& out, const std::string& s) {
    static const char* hex = "0123456789abcdef";
    out += '"';
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (c == '\r') out += "\\r";
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += (char)c;
    }
    out += '"';
}

// `FILE:LINE:COL: ` at the start of `s`: the first `:<digits>:<digits>: ` after a
// non-empty file name.
bool splitLocation(const std::string& s, std::string& file, int& line, int& col, std::string& rest) {
    for (size_t p = 1; p < s.size(); ++p) {
        if (s[p] != ':') continue;
        size_t i = p + 1, a = i;
        while (i < s.size() && isDigit(s[i])) ++i;
        if (i == a || i >= s.size() || s[i] != ':') continue;
        size_t b = ++i;
        while (i < s.size() && isDigit(s[i])) ++i;
        if (i == b || i + 1 >= s.size() || s[i] != ':' || s[i + 1] != ' ') continue;
        if (i - a > 9 || i - b > 9) continue;
        file = s.substr(0, p);
        line = std::atoi(s.substr(a, b - 1 - a).c_str());
        col = std::atoi(s.substr(b, i - b).c_str());
        rest = s.substr(i + 2);
        return true;
    }
    return false;
}

struct Note {
    std::string message, file;
    int line = 0, col = 0;
};

struct Diag {
    std::string severity, message, file, option;
    int line = 0, col = 0, endLine = 0, endCol = 0;
    std::vector<std::string> hints;
    std::vector<Note> notes;
};

class Converter {
public:
    explicit Converter(std::streambuf* out) : out(out) {}

    void line(std::string s) {
        if (!s.empty() && s.back() == '\r') s.pop_back();
        if (s.empty()) return;
        std::string sev, rest, file;
        int ln = 0, col = 0;
        bool located = false;
        for (const char* p : {"error: ", "warning: ", "note: "}) {
            if (startsWith(s, p)) {
                sev = std::string(p, std::char_traits<char>::length(p) - 2);
                rest = s.substr(sev.size() + 2);
                std::string msg;
                if (splitLocation(rest, file, ln, col, msg)) { located = true; rest = msg; }
                break;
            }
        }
        if (sev.empty()) {
            std::string msg;
            if (splitLocation(s, file, ln, col, msg)) {
                for (const char* p : {"error: ", "warning: ", "note: "}) {
                    if (startsWith(msg, p)) {
                        sev = std::string(p, std::char_traits<char>::length(p) - 2);
                        rest = msg.substr(sev.size() + 2);
                        located = true;
                        break;
                    }
                }
            }
        }
        if (sev.empty()) {                       // not a diagnostic: the text of the one before
            if (pending) { pending->notes.push_back(Note{s, "", 0, 0}); return; }
            Diag d;
            d.severity = "note";
            d.message = s;
            emit(d, "output");
            return;
        }
        if (!located) file.clear();
        if (sev == "note" && pending) {
            pending->notes.push_back(Note{rest, file, located ? ln : 0, located ? col : 0});
            return;
        }
        flush();
        auto d = std::make_unique<Diag>();
        d->severity = sev;
        d->message = rest;
        if (located) {
            d->file = file;
            d->line = ln;
            d->col = col;
        }
        splitMessage(*d);
        if (located) tokenRange(*d);
        if (sev == "note") { emit(*d, ""); return; }
        pending = std::move(d);
    }

    void flush() {
        if (!pending) return;
        emit(*pending, "");
        pending.reset();
    }

private:
    std::streambuf* out;
    std::unique_ptr<Diag> pending;
    std::map<std::string, std::vector<std::string>> sources;

    // ` [-Wname]` at the end is the option; ` (did you mean 'x'?)` a hint.
    static void splitMessage(Diag& d) {
        std::string& m = d.message;
        size_t ob = m.rfind(" [-W");
        if (ob != std::string::npos && !m.empty() && m.back() == ']') {
            std::string opt = m.substr(ob + 2, m.size() - ob - 3);
            bool ok = opt.size() > 2;
            for (size_t i = 2; i < opt.size(); ++i)
                if (!(isAlnum(opt[i]) || opt[i] == '-' || opt[i] == '=')) ok = false;
            if (ok) { d.option = opt; m.erase(ob); }
        }
        size_t hp = m.rfind(" (did you mean '");
        if (hp != std::string::npos && m.size() >= 2 && m.compare(m.size() - 2, 2, "?)") == 0) {
            d.hints.push_back(m.substr(hp + 2, m.size() - hp - 3));
            m.erase(hp);
        }
    }

    // The token at the diagnostic's position: an identifier or keyword, a number or a
    // string literal. end_col is the column just past it.
    void tokenRange(Diag& d) {
        if (d.line <= 0 || d.col <= 0) return;
        auto it = sources.find(d.file);
        if (it == sources.end()) {
            std::vector<std::string> lines;
            std::ifstream in(d.file, std::ios::binary);
            if (in) {
                std::string l;
                while (std::getline(in, l)) lines.push_back(l);
            }
            it = sources.emplace(d.file, std::move(lines)).first;
        }
        if ((size_t)d.line > it->second.size()) return;
        const std::string& s = it->second[d.line - 1];
        size_t i = d.col - 1, j = i;
        if (i >= s.size()) return;
        if (isIdentStart(s[i])) {
            while (j < s.size() && isIdent(s[j])) ++j;
        } else if (isDigit(s[i])) {
            while (j < s.size() && (isIdent(s[j]) || s[j] == '.')) ++j;
        } else if (s[i] == '"') {
            ++j;
            while (j < s.size() && s[j] != '"') j += s[j] == '\\' ? 2 : 1;
            if (j >= s.size()) return;
            ++j;
        } else {
            return;
        }
        d.endLine = d.line;
        d.endCol = (int)j + 1;
    }

    void emit(const Diag& d, const std::string& fixedId) {
        std::string o = "{\"severity\":";
        jsonString(o, d.severity);
        o += ",\"id\":";
        jsonString(o, fixedId.empty() ? diagnosticId(d.message, d.severity) : fixedId);
        o += ",\"message\":";
        jsonString(o, d.message);
        if (d.line > 0) {
            o += ",\"file\":";
            jsonString(o, d.file);
            o += ",\"line\":" + std::to_string(d.line) + ",\"col\":" + std::to_string(d.col);
            if (d.endLine > 0)
                o += ",\"end_line\":" + std::to_string(d.endLine) + ",\"end_col\":" + std::to_string(d.endCol);
        }
        if (!d.option.empty()) {
            o += ",\"option\":";
            jsonString(o, d.option);
        }
        o += ",\"hints\":[";
        for (size_t i = 0; i < d.hints.size(); ++i) {
            if (i) o += ',';
            jsonString(o, d.hints[i]);
        }
        o += "],\"notes\":[";
        for (size_t i = 0; i < d.notes.size(); ++i) {
            const Note& n = d.notes[i];
            if (i) o += ',';
            o += "{\"message\":";
            jsonString(o, n.message);
            if (n.line > 0) {
                o += ",\"file\":";
                jsonString(o, n.file);
                o += ",\"line\":" + std::to_string(n.line) + ",\"col\":" + std::to_string(n.col);
            }
            o += '}';
        }
        o += "]}\n";
        out->sputn(o.data(), (std::streamsize)o.size());
        out->pubsync();
    }
};

// std::cerr's buffer in JSON mode: whole lines go to the converter.
class JsonDiagBuf : public std::streambuf {
public:
    explicit JsonDiagBuf(std::streambuf* real) : conv(real) {}
    void finish() {
        std::lock_guard<std::mutex> lock(mu);
        if (!partial.empty()) { conv.line(partial); partial.clear(); }
        conv.flush();
    }

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return 0;
        char ch = (char)c;
        xsputn(&ch, 1);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lock(mu);
        for (std::streamsize i = 0; i < n; ++i) {
            if (s[i] == '\n') { conv.line(partial); partial.clear(); }
            else partial += s[i];
        }
        return n;
    }

private:
    Converter conv;
    std::string partial;
    std::mutex mu;
};

JsonDiagBuf* g_buf = nullptr;

}  // namespace

std::string diagnosticId(const std::string& message, const std::string& severity) {
    std::string m = message;
    for (const char* p : {"initializing '", "In struct '", "In union '"}) {
        if (startsWith(m, p)) {
            size_t j = m.find("': ", std::char_traits<char>::length(p));
            if (j != std::string::npos) m = m.substr(j + 3);
            break;
        }
    }
    std::vector<std::string> words;
    std::string cur;
    size_t n = m.size();
    for (size_t i = 0; i < n;) {
        char c = m[i];
        if (c == '\'' && (i == 0 || !isAlnum(m[i - 1]))) {   // a quoted name, type or value
            size_t j = i + 1;
            while (j < n && !(m[j] == '\'' && (j + 1 == n || !isAlnum(m[j + 1])))) ++j;
            if (j >= n) break;
            words.push_back(cur);
            cur.clear();
            i = j + 1;
            continue;
        }
        if (c == ':' || c == ';' || c == ',' || c == '(') break;
        if (c == ' ' || c == '\t') { words.push_back(cur); cur.clear(); ++i; continue; }
        if (c != '`') cur += c;
        ++i;
    }
    words.push_back(cur);
    std::string id;
    int kept = 0;
    for (std::string w : words) {
        if (w.size() >= 2 && w.compare(w.size() - 2, 2, "'s") == 0) w.erase(w.size() - 2);
        if (!w.empty() && w[0] == '#') w.erase(0, 1);
        if (!isIdWord(w)) continue;
        for (char& ch : w) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (w == "a" || w == "an" || w == "the") continue;
        if (kept) id += '-';
        id += w;
        if (++kept == 5) break;
    }
    return id.empty() ? severity : id;
}

void installJsonDiagnostics() {
    if (g_buf) return;
    std::cerr.flush();
    g_buf = new JsonDiagBuf(std::cerr.rdbuf());
    std::cerr.rdbuf(g_buf);
    g_jsonDiagnostics = true;
    std::atexit(flushJsonDiagnostics);
}

void flushJsonDiagnostics() {
    if (g_buf) g_buf->finish();
}
