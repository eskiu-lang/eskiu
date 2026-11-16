#include "eir_as_int.h"

// The integrated ARM64 assembler and object writer (see eir_as_arm64.h). One pass over the
// text encodes each instruction (every one is 4 bytes, so each label's offset is known when
// it is defined) and records a fixup for each symbol reference; at the end each fixup is
// either resolved in place (a branch to a label of the same section, as a real assembler
// does) or becomes a relocation, and the object is written: Mach-O (LC_SEGMENT_64,
// LC_BUILD_VERSION, LC_SYMTAB, LC_DYSYMTAB) or ELF64 (.text and the other sections,
// .rela.*, .symtab, .strtab, .note.GNU-stack). The choices follow LLVM's assembler, so the
// object disassembles and relocates as `cc -c` on the same text does (tests/eir/obj_check.sh).

namespace eir {

namespace a64as {

[[noreturn]] void fail(const std::string& m) { throw AsmError{m}; }

std::string str(SV s) { return std::string(s.data(), s.size()); }

SV trim(SV s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$'; }

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$'; }

bool ieq(SV a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
    return true;
}

bool startsWith(SV s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseInt(SV s, int64_t& out) {
    s = trim(s);
    if (!s.empty() && s[0] == '#') s = trim(s.substr(1));
    bool neg = false;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) {
        neg = s[0] == '-';
        s = s.substr(1);
    }
    if (s.empty()) return false;
    uint64_t v = 0;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        for (size_t i = 2; i < s.size(); ++i) {
            int d = hexDigit(s[i]);
            if (d < 0) return false;
            v = v * 16 + static_cast<uint64_t>(d);
        }
    } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        for (size_t i = 2; i < s.size(); ++i) {
            if (s[i] != '0' && s[i] != '1') return false;
            v = v * 2 + static_cast<uint64_t>(s[i] - '0');
        }
    } else {
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
    }
    out = neg ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v);
    return true;
}

bool parseReg(SV s, Reg& r) {
    s = trim(s);
    r = Reg();
    if (ieq(s, "sp")) { r.cls = 'x'; r.n = 31; r.sp = true; return true; }
    if (ieq(s, "wsp")) { r.cls = 'w'; r.n = 31; r.sp = true; return true; }
    if (ieq(s, "xzr")) { r.cls = 'x'; r.n = 31; r.zr = true; return true; }
    if (ieq(s, "wzr")) { r.cls = 'w'; r.n = 31; r.zr = true; return true; }
    if (ieq(s, "fp")) { r.cls = 'x'; r.n = 29; return true; }
    if (ieq(s, "lr")) { r.cls = 'x'; r.n = 30; return true; }
    if (s.size() < 2 || s.size() > 3) return false;
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    if (!std::strchr("xwbhsdq", c)) return false;
    int n = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        n = n * 10 + (s[i] - '0');
    }
    if (s.size() == 3 && s[1] == '0') return false;
    if ((c == 'x' || c == 'w') ? n > 30 : n > 31) return false;
    r.cls = c;
    r.n = n;
    return true;
}

bool isGp(const Reg& r) { return r.cls == 'x' || r.cls == 'w'; }

int parseCond(SV s) {
    static const char* names[] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                  "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
    s = trim(s);
    for (int i = 0; i < 16; ++i)
        if (ieq(s, names[i])) return i;
    if (ieq(s, "cs")) return 2;
    if (ieq(s, "cc")) return 3;
    return -1;
}

bool parseShift(SV s, Shift& sh) {
    static const char* names[] = {"lsl", "lsr", "asr", "ror", "uxtb", "uxth", "uxtw", "uxtx",
                                  "sxtb", "sxth", "sxtw", "sxtx"};
    s = trim(s);
    size_t i = 0;
    while (i < s.size() && std::isalpha(static_cast<unsigned char>(s[i]))) ++i;
    SV name = s.substr(0, i);
    sh = Shift();
    for (int k = 0; k < 12; ++k)
        if (ieq(name, names[k])) sh.kind = k;
    if (sh.kind < 0) return false;
    SV rest = trim(s.substr(i));
    if (rest.empty()) return sh.kind >= 4;   // an extend may omit its amount
    int64_t v = 0;
    if (!parseInt(rest, v) || v < 0 || v > 63) return false;
    sh.amount = static_cast<int>(v);
    sh.hasAmount = true;
    return true;
}

bool isSymName(SV s) {
    if (s.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(s[0]))) {   // a numeric local label: 1f, 1b
        size_t i = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        return i + 1 == s.size() && (s[i] == 'f' || s[i] == 'b');
    }
    if (!isIdentStart(s[0])) return false;
    for (char c : s)
        if (!isIdentChar(c)) return false;
    return true;
}

bool parseSymRef(SV s, SymRef& r) {
    s = trim(s);
    r = SymRef();
    if (!s.empty() && s[0] == ':') {
        size_t e = s.find(':', 1);
        if (e == SV::npos) return false;
        SV m = s.substr(1, e - 1);
        if (m == "lo12") r.mod = Mod::Lo12;
        else if (m == "got") r.mod = Mod::Got;
        else if (m == "got_lo12") r.mod = Mod::GotLo12;
        else return false;
        s = s.substr(e + 1);
    } else {
        size_t at = s.find('@');
        if (at != SV::npos) {
            SV m = s.substr(at + 1);
            if (m == "PAGE") r.mod = Mod::Page;
            else if (m == "PAGEOFF") r.mod = Mod::PageOff;
            else if (m == "GOTPAGE") r.mod = Mod::GotPage;
            else if (m == "GOTPAGEOFF") r.mod = Mod::GotPageOff;
            else return false;
            s = s.substr(0, at);
        }
    }
    if (!isSymName(s)) return false;
    r.name = s;
    return true;
}

void splitTop(SV s, std::vector<SV>& out) {
    out.clear();
    s = trim(s);
    if (s.empty()) return;
    int depth = 0;
    size_t start = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '[') depth++;
        else if (s[i] == ']') depth--;
        else if (s[i] == ',' && depth == 0) {
            out.push_back(trim(s.substr(start, i - start)));
            start = i + 1;
        }
    }
    out.push_back(trim(s.substr(start)));
}

bool parseMem(SV s, Mem& m) {
    s = trim(s);
    m = Mem();
    if (s.empty() || s[0] != '[') return false;
    size_t close = s.find(']');
    if (close == SV::npos) return false;
    SV after = trim(s.substr(close + 1));
    if (after == "!") m.pre = true;
    else if (!after.empty()) return false;
    std::vector<SV> parts;
    splitTop(s.substr(1, close - 1), parts);
    if (parts.empty() || parts.size() > 3) return false;
    if (!parseReg(parts[0], m.base) || m.base.cls != 'x' || m.base.zr) return false;
    if (parts.size() == 1) return true;
    int64_t v = 0;
    if (parts.size() == 2 && parseInt(parts[1], v) && (parts[1][0] == '#' || std::isdigit(static_cast<unsigned char>(parts[1][0])) || parts[1][0] == '-')) {
        m.kind = Mem::Imm;
        m.imm = v;
        return true;
    }
    if (parseReg(parts[1], m.index)) {
        if (!isGp(m.index) || m.index.sp) return false;
        m.kind = Mem::RegOff;
        if (parts.size() == 3 && !parseShift(parts[2], m.ext)) return false;
        return true;
    }
    if (parts.size() == 2 && parseSymRef(parts[1], m.sym) && m.sym.mod != Mod::None) {
        m.kind = Mem::Sym;
        return true;
    }
    return false;
}

bool isShiftedMask(uint64_t v) {
    if (v == 0) return false;
    uint64_t filled = (v - 1) | v;            // fill the trailing zeros
    return ((filled + 1) & filled) == 0;      // then a contiguous run from bit 0
}

int ctz64(uint64_t v) {
    int n = 0;
    while (n < 64 && !(v & (1ULL << n))) ++n;
    return n;
}

int cto64(uint64_t v) { return ctz64(~v); }

int clo64(uint64_t v) {
    int n = 0;
    while (n < 64 && (v & (1ULL << (63 - n)))) ++n;
    return n;
}

bool encodeLogicalImm(uint64_t imm, unsigned regSize, uint32_t& enc) {
    if (regSize == 32) imm &= 0xffffffffULL;
    if (imm == 0 || (regSize == 64 && imm == ~0ULL) || (regSize == 32 && imm == 0xffffffffULL)) return false;
    unsigned size = regSize;
    do {
        size /= 2;
        uint64_t mask = (1ULL << size) - 1;
        if ((imm & mask) != ((imm >> size) & mask)) {
            size *= 2;
            break;
        }
    } while (size > 2);
    uint64_t mask = ~0ULL >> (64 - size);
    imm &= mask;
    unsigned i, cto;
    if (isShiftedMask(imm)) {
        i = static_cast<unsigned>(ctz64(imm));
        cto = static_cast<unsigned>(cto64(imm >> i));
    } else {
        imm |= ~mask;
        if (!isShiftedMask(~imm)) return false;
        unsigned clo = static_cast<unsigned>(clo64(imm));
        i = 64 - clo;
        cto = clo + static_cast<unsigned>(cto64(imm)) - (64 - size);
    }
    unsigned immr = (size - i) & (size - 1);
    uint64_t nimms = ~(static_cast<uint64_t>(size) - 1) << 1;
    nimms |= (cto - 1);
    unsigned n = ((nimms >> 6) & 1) ^ 1;
    enc = (n << 12) | (immr << 6) | static_cast<unsigned>(nimms & 0x3f);
    return true;
}

void put32(std::vector<uint8_t>& d, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) d[off + i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t get32(const std::vector<uint8_t>& d, size_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(d[off + i]) << (8 * i);
    return v;
}

void out8(std::string& o, uint8_t v) { o.push_back(static_cast<char>(v)); }

void out16(std::string& o, uint16_t v) { for (int i = 0; i < 2; ++i) o.push_back(static_cast<char>(v >> (8 * i))); }

void out32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(static_cast<char>(v >> (8 * i))); }

void out64(std::string& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(static_cast<char>(v >> (8 * i))); }

void outName16(std::string& o, const std::string& n) {
    for (size_t i = 0; i < 16; ++i) o.push_back(i < n.size() ? n[i] : '\0');
}

void padTo(std::string& o, size_t off) { if (o.size() < off) o.append(off - o.size(), '\0'); }

uint64_t alignTo(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

const std::unordered_map<std::string, int>& mnemonics() {
    static const std::unordered_map<std::string, int> m = {
        {"add", M_ADD}, {"adds", M_ADDS}, {"sub", M_SUB}, {"subs", M_SUBS}, {"cmp", M_CMP}, {"cmn", M_CMN},
        {"neg", M_NEG}, {"negs", M_NEGS}, {"mov", M_MOV}, {"movz", M_MOVZ}, {"movn", M_MOVN}, {"movk", M_MOVK},
        {"and", M_AND}, {"ands", M_ANDS}, {"orr", M_ORR}, {"eor", M_EOR}, {"bic", M_BIC}, {"bics", M_BICS},
        {"orn", M_ORN}, {"eon", M_EON}, {"tst", M_TST}, {"mvn", M_MVN},
        {"lsl", M_LSL}, {"lsr", M_LSR}, {"asr", M_ASR}, {"ror", M_ROR}, {"sdiv", M_SDIV}, {"udiv", M_UDIV},
        {"madd", M_MADD}, {"msub", M_MSUB}, {"mul", M_MUL}, {"mneg", M_MNEG},
        {"sxtb", M_SXTB}, {"sxth", M_SXTH}, {"sxtw", M_SXTW}, {"uxtb", M_UXTB}, {"uxth", M_UXTH},
        {"sbfx", M_SBFX}, {"ubfx", M_UBFX}, {"sbfiz", M_SBFIZ}, {"ubfiz", M_UBFIZ}, {"bfi", M_BFI},
        {"bfxil", M_BFXIL}, {"sbfm", M_SBFM}, {"ubfm", M_UBFM}, {"bfm", M_BFM},
        {"csel", M_CSEL}, {"csinc", M_CSINC}, {"csinv", M_CSINV}, {"csneg", M_CSNEG}, {"cset", M_CSET},
        {"csetm", M_CSETM}, {"cinc", M_CINC}, {"cinv", M_CINV}, {"cneg", M_CNEG},
        {"ldr", M_LDR}, {"str", M_STR}, {"ldrb", M_LDRB}, {"strb", M_STRB}, {"ldrh", M_LDRH}, {"strh", M_STRH},
        {"ldrsb", M_LDRSB}, {"ldrsh", M_LDRSH}, {"ldrsw", M_LDRSW},
        {"ldur", M_LDUR}, {"stur", M_STUR}, {"ldurb", M_LDURB}, {"sturb", M_STURB}, {"ldurh", M_LDURH},
        {"sturh", M_STURH}, {"ldursb", M_LDURSB}, {"ldursh", M_LDURSH}, {"ldursw", M_LDURSW},
        {"ldp", M_LDP}, {"stp", M_STP},
        {"b", M_B}, {"bl", M_BL}, {"br", M_BR}, {"blr", M_BLR}, {"ret", M_RET}, {"cbz", M_CBZ}, {"cbnz", M_CBNZ},
        {"tbz", M_TBZ}, {"tbnz", M_TBNZ}, {"adr", M_ADR}, {"adrp", M_ADRP},
        {"nop", M_NOP}, {"yield", M_YIELD}, {"wfe", M_WFE}, {"wfi", M_WFI}, {"sev", M_SEV}, {"sevl", M_SEVL},
        {"brk", M_BRK}, {"svc", M_SVC}, {"hlt", M_HLT}, {"clrex", M_CLREX}, {"dmb", M_DMB}, {"dsb", M_DSB},
        {"isb", M_ISB},
        {"fmov", M_FMOV}, {"fadd", M_FADD}, {"fsub", M_FSUB}, {"fmul", M_FMUL}, {"fdiv", M_FDIV},
        {"fnmul", M_FNMUL}, {"fmax", M_FMAX}, {"fmin", M_FMIN}, {"fneg", M_FNEG}, {"fabs", M_FABS},
        {"fsqrt", M_FSQRT}, {"fcmp", M_FCMP}, {"fcmpe", M_FCMPE}, {"fcvt", M_FCVT}, {"scvtf", M_SCVTF},
        {"ucvtf", M_UCVTF}, {"fcvtzs", M_FCVTZS}, {"fcvtzu", M_FCVTZU}, {"fcsel", M_FCSEL},
    };
    return m;
}


void Assembler::run(const std::string& text) {
    size_t pos = 0;
    line = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        ++line;
        lineText = SV(text.data() + pos, e - pos);
        processLine(lineText);
        pos = e + 1;
    }
    lineText = SV();
}

std::string Assembler::write() {
    resolve();
    return tgt.macho ? writeMachO() : writeElf();
}

} // namespace a64as

using namespace a64as;

bool assembleArm64(const std::string& text, const Arm64ObjTarget& target, std::string& object,
                   std::string& error) {
    Assembler as(target);
    try {
        as.run(text);
        object = as.write();
        return true;
    } catch (const AsmError& e) {
        error = "integrated assembler: ";
        if (as.line > 0) error += "line " + std::to_string(as.line) + ": ";
        error += e.msg;
        if (!as.lineText.empty()) error += ": '" + str(trim(as.lineText)) + "'";
        return false;
    }
}

} // namespace eir
