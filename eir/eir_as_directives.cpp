#include "eir_as_int.h"

namespace eir {

namespace a64as {

int Assembler::sectionFor(const std::string& seg, const std::string& name) {
    for (size_t i = 0; i < sections.size(); ++i)
        if (sections[i].segname == seg && (tgt.macho ? sections[i].sectname : sections[i].name) == name)
            return static_cast<int>(i);
    Section s;
    if (tgt.macho) {
        s.segname = seg;
        s.sectname = name;
        if (name == "__text" && seg == "__TEXT") s.machoFlags = 0x80000000u;
    } else {
        s.name = name;
        if (name == ".text" || startsWith(name, ".text.")) s.elfFlags = 6;
        else if (name == ".data" || startsWith(name, ".data.")) s.elfFlags = 3;
        else if (name == ".rodata" || startsWith(name, ".rodata.")) s.elfFlags = 2;
        else s.elfFlags = 0;
    }
    sections.push_back(std::move(s));
    return static_cast<int>(sections.size() - 1);
}

int Assembler::symbol(SV name) {
    auto it = symIndex.find(name);
    if (it != symIndex.end()) return it->second;
    Symbol s;
    s.name = name;
    s.temp = tgt.macho ? (!name.empty() && name[0] == 'L') : startsWith(name, ".L");
    syms.push_back(s);
    int idx = static_cast<int>(syms.size() - 1);
    symIndex.emplace(name, idx);
    return idx;
}

int Assembler::ownedSymbol(const std::string& name) {
    auto it = symIndex.find(SV(name));
    if (it != symIndex.end()) return it->second;
    ownedNames.push_back(name);
    int idx = symbol(SV(ownedNames.back()));
    syms[static_cast<size_t>(idx)].temp = true;
    return idx;
}

int Assembler::refSymbol(SV name) {
    if (std::isdigit(static_cast<unsigned char>(name[0]))) {
        std::string num = str(name.substr(0, name.size() - 1));
        int defs = numericDefs[num];
        int k = name.back() == 'f' ? defs : defs - 1;
        if (k < 0) fail("no earlier definition of local label " + num);
        return ownedSymbol("\x01" + num + ":" + std::to_string(k));
    }
    return symbol(name);
}

void Assembler::defineLabel(SV name) {
    int idx;
    bool numeric = true;
    for (char c : name)
        if (!std::isdigit(static_cast<unsigned char>(c))) numeric = false;
    if (numeric) {
        std::string num = str(name);
        int k = numericDefs[num]++;
        idx = ownedSymbol("\x01" + num + ":" + std::to_string(k));
    } else {
        if (!isSymName(name)) fail("invalid label name '" + str(name) + "'");
        idx = symbol(name);
    }
    Symbol& s = syms[static_cast<size_t>(idx)];
    if (s.defined) fail("label '" + str(name) + "' defined twice");
    s.defined = true;
    s.section = cur;
    s.offset = sec().data.size();
}

void Assembler::processLine(SV ln) {
    // Comments (`//`, and `;` on Darwin) and, on ELF, `;` between statements.
    bool inStr = false;
    size_t start = 0;
    for (size_t i = 0; i < ln.size(); ++i) {
        char c = ln[i];
        if (inStr) {
            if (c == '\\') ++i;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '/' && i + 1 < ln.size() && ln[i + 1] == '/') {
            statement(ln.substr(start, i - start));
            return;
        } else if (c == ';') {
            statement(ln.substr(start, i - start));
            if (tgt.macho) return;
            start = i + 1;
        }
    }
    statement(ln.substr(start));
}

void Assembler::statement(SV s) {
    s = trim(s);
    while (!s.empty()) {
        size_t i = 0;
        while (i < s.size() && isIdentChar(s[i])) ++i;
        if (i == 0 || i >= s.size() || s[i] != ':') break;
        defineLabel(s.substr(0, i));
        s = trim(s.substr(i + 1));
    }
    if (s.empty()) return;
    size_t i = 0;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    SV head = s.substr(0, i), rest = trim(s.substr(i));
    if (head[0] == '.') directive(head, rest);
    else instruction(head, rest);
}

void Assembler::emitBytes(const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    sec().data.insert(sec().data.end(), b, b + n);
}

void Assembler::align(uint32_t log2) {
    if (log2 > 16) fail("alignment too large");
    Section& s = sec();
    s.alignLog2 = std::max(s.alignLog2, log2);
    uint64_t a = 1ULL << log2;
    size_t target = static_cast<size_t>(alignTo(s.data.size(), a));
    if (s.code && s.data.size() % 4 == 0) {
        while (s.data.size() + 4 <= target) emitLE(0xd503201fu, 4);
    }
    s.data.resize(target, 0);
}

void Assembler::stringData(SV rest, bool zero) {
    std::vector<SV> items;
    size_t i = 0;
    while (true) {
        while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
        if (i >= rest.size() || rest[i] != '"') fail("expected a string");
        ++i;
        std::string out;
        while (true) {
            if (i >= rest.size()) fail("unterminated string");
            char c = rest[i++];
            if (c == '"') break;
            if (c != '\\') { out += c; continue; }
            if (i >= rest.size()) fail("unterminated string");
            char e = rest[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'v': out += '\v'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'x': case 'X': {
                    int v = 0, n = 0;
                    while (i < rest.size() && hexDigit(rest[i]) >= 0) { v = v * 16 + hexDigit(rest[i++]); ++n; }
                    if (n == 0) fail("bad \\x escape");
                    out += static_cast<char>(v & 0xff);
                    break;
                }
                default:
                    if (e >= '0' && e <= '7') {
                        int v = e - '0', n = 1;
                        while (n < 3 && i < rest.size() && rest[i] >= '0' && rest[i] <= '7') { v = v * 8 + (rest[i++] - '0'); ++n; }
                        out += static_cast<char>(v & 0xff);
                    } else {
                        fail(std::string("unknown escape \\") + e);
                    }
            }
        }
        emitBytes(out.data(), out.size());
        if (zero) emitLE(0, 1);
        while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
        if (i >= rest.size()) return;
        if (rest[i] != ',') fail("expected ',' between strings");
        ++i;
    }
}

void Assembler::dataValues(SV rest, int size) {
    splitTop(rest, ops);
    if (ops.empty()) fail("expected a value");
    for (SV v : ops) {
        int64_t n = 0;
        if (parseInt(v, n)) {
            emitLE(static_cast<uint64_t>(n), size);
            continue;
        }
        SymRef r;
        if ((size == 8 || size == 4) && parseSymRef(v, r) && r.mod == Mod::None) {
            addFixup(size == 8 ? Fix::Abs64 : Fix::Abs32, refSymbol(r.name));
            emitLE(0, size);
            continue;
        }
        fail("unsupported data value '" + str(v) + "'");
    }
}

void Assembler::sectionDirective(SV rest) {
    splitTop(rest, ops);
    if (ops.empty()) fail("expected a section name");
    if (tgt.macho) {
        if (ops.size() < 2) fail("expected segment,section");
        std::string seg = str(ops[0]), name = str(ops[1]);
        uint32_t type = 0, attrs = 0;
        if (ops.size() > 2) {
            SV t = ops[2];
            if (t == "regular") type = 0;
            else if (t == "cstring_literals") type = 2;
            else if (t == "4byte_literals") type = 3;
            else if (t == "8byte_literals") type = 4;
            else if (t == "16byte_literals") type = 0xe;
            else fail("unsupported section type '" + str(t) + "'");
        }
        if (ops.size() > 3) {
            SV a = ops[3];
            while (!a.empty()) {
                size_t p = a.find('+');
                SV one = trim(a.substr(0, p));
                if (one == "pure_instructions") attrs |= 0x80000000u;
                else if (one == "no_dead_strip") attrs |= 0x10000000u;
                else fail("unsupported section attribute '" + str(one) + "'");
                a = p == SV::npos ? SV() : a.substr(p + 1);
            }
        }
        if (ops.size() > 4) fail("unsupported section directive");
        cur = sectionFor(seg, name);
        if (ops.size() > 2) sec().machoFlags = type | attrs;
        return;
    }
    std::string name = str(ops[0]);
    cur = sectionFor("", name);
    if (ops.size() == 1) return;
    Section& s = sec();
    SV flags = ops[1];
    if (flags.size() < 2 || flags.front() != '"' || flags.back() != '"') fail("expected section flags");
    uint64_t f = 0;
    for (char c : flags.substr(1, flags.size() - 2)) {
        if (c == 'a') f |= 2;
        else if (c == 'w') f |= 1;
        else if (c == 'x') f |= 4;
        else if (c == 'M') f |= 0x10;
        else if (c == 'S') f |= 0x20;
        else fail(std::string("unsupported section flag '") + c + "'");
    }
    s.elfFlags = f;
    if (ops.size() > 2) {
        if (ops[2] == "@progbits" || ops[2] == "%progbits") s.elfType = 1;
        else if (ops[2] == "@note" || ops[2] == "%note") s.elfType = 7;
        else fail("unsupported section type '" + str(ops[2]) + "'");
    }
    if (ops.size() > 3) {
        int64_t es = 0;
        if (!parseInt(ops[3], es) || es < 0) fail("bad section entry size");
        s.entsize = static_cast<uint64_t>(es);
    }
    if (ops.size() > 4) fail("unsupported section directive");
}

void Assembler::directive(SV name, SV rest) {
    if (name == ".section") return sectionDirective(rest);
    if (name == ".text") {
        if (!rest.empty()) fail("unexpected operand");
        cur = tgt.macho ? sectionFor("__TEXT", "__text") : sectionFor("", ".text");
        return;
    }
    if (name == ".data") {
        if (!rest.empty()) fail("unexpected operand");
        cur = tgt.macho ? sectionFor("__DATA", "__data") : sectionFor("", ".data");
        return;
    }
    if (name == ".globl" || name == ".global") {
        if (!isSymName(rest) || std::isdigit(static_cast<unsigned char>(rest[0]))) fail("bad symbol name");
        syms[static_cast<size_t>(symbol(rest))].global = true;
        return;
    }
    if (name == ".p2align" || name == ".align") {
        int64_t v = 0;
        if (!parseInt(rest, v) || v < 0) fail("bad alignment");
        align(static_cast<uint32_t>(v));
        return;
    }
    if (name == ".byte") return dataValues(rest, 1);
    if (name == ".short" || name == ".hword" || name == ".2byte") return dataValues(rest, 2);
    if (name == ".long" || name == ".word" || name == ".4byte") return dataValues(rest, 4);
    if (name == ".quad" || name == ".xword" || name == ".8byte") return dataValues(rest, 8);
    if (name == ".asciz" || name == ".string") return stringData(rest, true);
    if (name == ".ascii") return stringData(rest, false);
    if (name == ".zero" || name == ".space" || name == ".skip") {
        splitTop(rest, ops);
        int64_t n = 0, fill = 0;
        if (ops.empty() || ops.size() > 2 || !parseInt(ops[0], n) || n < 0) fail("bad size");
        if (ops.size() == 2 && !parseInt(ops[1], fill)) fail("bad fill value");
        sec().data.resize(sec().data.size() + static_cast<size_t>(n), static_cast<uint8_t>(fill));
        return;
    }
    fail("unsupported directive '" + str(name) + "'");
}

} // namespace a64as

} // namespace eir
