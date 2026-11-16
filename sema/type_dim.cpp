// type_dim.cpp: folding array dimensions as integer constant expressions (foldDim) (split from type.cpp).
#include "type.h"
#include <cctype>
#include <cstdint>
#include <climits>

namespace ty {

namespace {

// Recursive descent over a dimension's text (see foldDim); C precedence, typed values.
struct DimFolder {
    const std::string& s;
    const std::function<bool(const std::string&, long long&)>& name;
    size_t i = 0;
    bool ok = true;

    bool at(const std::string& op) const { return s.compare(i, op.size(), op) == 0; }
    static bool identStart(char c) { return std::isalpha((unsigned char)c) || c == '_'; }
    std::string ident() {
        size_t b = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '_')) ++i;
        return s.substr(b, i - b);
    }
    CInt fail() { ok = false; return CInt{}; }
    CInt unary() {
        if (!ok || i >= s.size()) return fail();
        char c = s[i];
        if (c == '-' && i + 1 < s.size() && std::isdigit((unsigned char)s[i + 1])) {   // `-5`: one literal
            ++i;
            CInt x = unary();
            if (!ok) return fail();
            return cintNegLiteral((unsigned long long)x.v);
        }
        if (c == '-' || c == '+' || c == '~' || c == '!') {
            ++i;
            CInt x = unary(), r;
            if (!ok || !cintUnary(std::string(1, c), x, r)) return fail();
            return r;
        }
        if (c == '(') {
            ++i;
            size_t save = i;
            if (i < s.size() && identStart(s[i])) {           // `(type)x`, a cast
                std::string t = ident();
                CInt probe;
                if (at(")") && cintCast(t, CInt{}, probe)) {
                    ++i;
                    CInt x = unary(), r;
                    if (!ok) return fail();
                    cintCast(t, x, r);
                    return r;
                }
                i = save;
            }
            CInt v = ternary();
            if (!at(")")) return fail();
            ++i;
            return v;
        }
        if (std::isdigit((unsigned char)c)) {
            size_t b = i;
            while (i < s.size() && std::isalnum((unsigned char)s[i])) ++i;
            std::string lit = s.substr(b, i - b);
            // C: a leading 0 is octal (`010` is 8), `0x` hex.
            int base = lit.size() > 1 && lit[0] == '0' && (lit[1] == 'x' || lit[1] == 'X') ? 16
                     : lit.size() > 1 && lit[0] == '0' ? 8 : 10;
            try {
                size_t used = 0;
                unsigned long long v = std::stoull(lit, &used, base);
                if (used != lit.size()) return fail();
                return cintLiteral(v);
            } catch (...) { return fail(); }
        }
        if (identStart(c)) {
            long long v = 0;
            std::string n = ident();
            // `sizeof(T)` goes to `name` whole, as the text "sizeof(T)"; its value is a
            // size (unsigned 64-bit, C's size_t).
            bool isSize = false;
            if (n == "sizeof" && at("(")) {
                size_t b = i, depth = 0;
                for (; i < s.size(); ++i) {
                    if (s[i] == '(') ++depth;
                    else if (s[i] == ')' && --depth == 0) break;
                }
                if (i >= s.size()) return fail();
                ++i;
                n += s.substr(b, i - b);
                isSize = true;
            }
            if (!name(n, v)) return fail();
            if (isSize) return cintMake(v, 64, true);
            return v >= INT32_MIN && v <= INT32_MAX ? cintMake(v, 32, false) : cintMake(v, 64, false);
        }
        return fail();
    }
    // The binary operator at the cursor for precedence `level`, or "".
    std::string opAt(int level) const {
        static const std::vector<std::vector<std::string>> ops = {
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<=", ">=", "<", ">"},
            {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"}};
        static const std::vector<std::string> two = {"||", "&&", "==", "!=", "<=", ">=", "<<", ">>"};
        std::string two_here;
        for (const auto& t : two) if (at(t)) two_here = t;
        for (const auto& o : ops[level]) {
            if (!at(o)) continue;
            if (o.size() == 1 && !two_here.empty()) continue;   // `|` of `||`, `<` of `<<`
            return o;
        }
        return "";
    }
    CInt binary(int level) {
        if (level == 10) return unary();
        CInt l = binary(level + 1);
        for (;;) {
            if (!ok) return fail();
            std::string o = opAt(level);
            if (o.empty()) return l;
            i += o.size();
            CInt r = binary(level + 1);
            if (!ok || !cintBinary(o, l, r, l)) return fail();
        }
    }
    CInt ternary() {
        CInt c = binary(0);
        if (!ok || !at("?")) return c;
        ++i;
        CInt a = ternary();
        if (!at(":")) return fail();
        ++i;
        CInt b = ternary();
        if (!ok) return fail();
        // The arms meet at their common type (the usual arithmetic conversions).
        int r = a.rank > b.rank ? a.rank : b.rank;
        bool u = a.rank == b.rank ? (a.uns || b.uns) : (a.rank > b.rank ? a.uns : b.uns);
        return cintMake(c.v ? a.v : b.v, r, u);
    }
};

}  // namespace

bool foldDim(const std::string& dim, const std::function<bool(const std::string&, long long&)>& name,
             long long& out) {
    std::string t;
    for (char c : dim) if (!std::isspace((unsigned char)c)) t += c;
    if (t.empty()) return false;
    DimFolder f{t, name};
    CInt v = f.ternary();
    if (!f.ok || f.i != t.size()) return false;
    out = v.v;
    return true;
}

}  // namespace ty
