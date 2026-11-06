#include "eir_as_arm64.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <vector>

// The integrated ARM64 assembler and object writer (see eir_as_arm64.h). One pass over the
// text encodes each instruction (every one is 4 bytes, so each label's offset is known when
// it is defined) and records a fixup for each symbol reference; at the end each fixup is
// either resolved in place (a branch to a label of the same section, as a real assembler
// does) or becomes a relocation, and the object is written: Mach-O (LC_SEGMENT_64,
// LC_BUILD_VERSION, LC_SYMTAB, LC_DYSYMTAB) or ELF64 (.text and the other sections,
// .rela.*, .symtab, .strtab, .note.GNU-stack). The choices follow LLVM's assembler, so the
// object disassembles and relocates as `cc -c` on the same text does (tests/eir/obj_check.sh).

namespace eir {

namespace {

using SV = std::string_view;

struct AsmError {
    std::string msg;
};

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

// An integer: `#` optional, a sign, decimal, 0x hex or 0b binary.
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

// ---- Registers, conditions, shifts ----

struct Reg {
    char cls = 0;       // x w (general), b h s d q (FP/SIMD)
    int n = 0;
    bool sp = false, zr = false;
};

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

// A shift (lsl 0, lsr 1, asr 2, ror 3) or an extend (4 + its option: uxtb ... sxtx).
struct Shift {
    int kind = -1;
    int amount = 0;
    bool hasAmount = false;
};

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

// ---- Symbol references ----

enum class Mod { None, Page, PageOff, GotPage, GotPageOff, Lo12, Got, GotLo12 };

struct SymRef {
    SV name;
    Mod mod = Mod::None;
};

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

// ---- Memory operands ----

struct Mem {
    Reg base;
    enum Kind { None, Imm, RegOff, Sym } kind = None;
    int64_t imm = 0;
    Reg index;
    Shift ext;
    SymRef sym;
    bool pre = false;
};

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

// ---- Logical immediates (LLVM's processLogicalImmediate) ----

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

// ---- Object model ----

enum class Fix : uint8_t { Jump26, Call26, CondBr19, TestBr14, Adr21, Page21, PageOff12, GotPage21, GotPageOff12, Abs64, Abs32 };

struct Fixup {
    uint32_t offset;
    Fix kind;
    uint8_t scaleLog2;    // PageOff12 of a load or store: log2 of its access size (0xff: add)
    int sym;
    int line;
};

struct Section {
    std::string segname, sectname;   // Mach-O
    std::string name;                // ELF
    uint32_t machoFlags = 0;
    uint32_t elfType = 1;
    uint64_t elfFlags = 0, entsize = 0;
    uint32_t alignLog2 = 0;
    bool code = false;
    std::vector<uint8_t> data;
    std::vector<Fixup> fixups;
    uint64_t addr = 0;
    int elfIndex = 0, elfSecSym = -1;
    bool secSymUsed = false;
};

struct Symbol {
    SV name;
    int section = -1;
    uint64_t offset = 0;
    bool defined = false, global = false, temp = false, inReloc = false, referenced = false;
    int out = -1;
};

struct Reloc {
    uint32_t offset = 0;
    uint32_t type = 0;
    int sym = -1;         // symbol index (Mach-O: extern), or -1 with secSym for an ELF section symbol
    int secSym = -1;
    int64_t addend = 0;
    bool pcrel = false;
    uint8_t length = 2;
};

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

// ---- Mnemonics ----

enum Mn {
    M_ADD, M_ADDS, M_SUB, M_SUBS, M_CMP, M_CMN, M_NEG, M_NEGS, M_MOV, M_MOVZ, M_MOVN, M_MOVK,
    M_AND, M_ANDS, M_ORR, M_EOR, M_BIC, M_BICS, M_ORN, M_EON, M_TST, M_MVN,
    M_LSL, M_LSR, M_ASR, M_ROR, M_SDIV, M_UDIV, M_MADD, M_MSUB, M_MUL, M_MNEG,
    M_SXTB, M_SXTH, M_SXTW, M_UXTB, M_UXTH, M_SBFX, M_UBFX, M_SBFIZ, M_UBFIZ, M_BFI, M_BFXIL,
    M_SBFM, M_UBFM, M_BFM,
    M_CSEL, M_CSINC, M_CSINV, M_CSNEG, M_CSET, M_CSETM, M_CINC, M_CINV, M_CNEG,
    M_LDR, M_STR, M_LDRB, M_STRB, M_LDRH, M_STRH, M_LDRSB, M_LDRSH, M_LDRSW,
    M_LDUR, M_STUR, M_LDURB, M_STURB, M_LDURH, M_STURH, M_LDURSB, M_LDURSH, M_LDURSW,
    M_LDP, M_STP,
    M_B, M_BL, M_BR, M_BLR, M_RET, M_CBZ, M_CBNZ, M_TBZ, M_TBNZ, M_ADR, M_ADRP,
    M_NOP, M_YIELD, M_WFE, M_WFI, M_SEV, M_SEVL, M_BRK, M_SVC, M_HLT, M_CLREX, M_DMB, M_DSB, M_ISB,
    M_FMOV, M_FADD, M_FSUB, M_FMUL, M_FDIV, M_FNMUL, M_FMAX, M_FMIN, M_FNEG, M_FABS, M_FSQRT,
    M_FCMP, M_FCMPE, M_FCVT, M_SCVTF, M_UCVTF, M_FCVTZS, M_FCVTZU, M_FCSEL,
};

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

// ---- The assembler ----

class Assembler {
public:
    explicit Assembler(const Arm64ObjTarget& t) : tgt(t) {
        cur = sectionFor(tgt.macho ? "__TEXT" : "", tgt.macho ? "__text" : ".text");
    }

    void run(const std::string& text) {
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

    std::string write() {
        resolve();
        return tgt.macho ? writeMachO() : writeElf();
    }

    int line = 0;
    SV lineText;

private:
    Arm64ObjTarget tgt;
    std::vector<Section> sections;
    int cur = 0;
    std::vector<Symbol> syms;
    std::unordered_map<SV, int> symIndex;
    std::deque<std::string> ownedNames;
    std::unordered_map<std::string, int> numericDefs;
    std::vector<SV> ops;
    std::vector<std::vector<Reloc>> relocs;   // per section

    // ---- Sections ----

    int sectionFor(const std::string& seg, const std::string& name) {
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

    Section& sec() { return sections[static_cast<size_t>(cur)]; }

    // ---- Symbols ----

    int symbol(SV name) {
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

    int ownedSymbol(const std::string& name) {
        auto it = symIndex.find(SV(name));
        if (it != symIndex.end()) return it->second;
        ownedNames.push_back(name);
        int idx = symbol(SV(ownedNames.back()));
        syms[static_cast<size_t>(idx)].temp = true;
        return idx;
    }

    // The symbol a reference names: a numeric local label (`1f`, `1b`) is the next or the
    // previous definition of that number.
    int refSymbol(SV name) {
        if (std::isdigit(static_cast<unsigned char>(name[0]))) {
            std::string num = str(name.substr(0, name.size() - 1));
            int defs = numericDefs[num];
            int k = name.back() == 'f' ? defs : defs - 1;
            if (k < 0) fail("no earlier definition of local label " + num);
            return ownedSymbol("\x01" + num + ":" + std::to_string(k));
        }
        return symbol(name);
    }

    void defineLabel(SV name) {
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

    // ---- Lines ----

    void processLine(SV ln) {
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

    void statement(SV s) {
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

    // ---- Directives ----

    void emitBytes(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        sec().data.insert(sec().data.end(), b, b + n);
    }

    void emitLE(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) sec().data.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    void align(uint32_t log2) {
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

    void stringData(SV rest, bool zero) {
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

    void dataValues(SV rest, int size) {
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

    void sectionDirective(SV rest) {
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

    void directive(SV name, SV rest) {
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

    // ---- Instructions ----

    void addFixup(Fix k, int sym, uint8_t scaleLog2 = 0xff) {
        syms[static_cast<size_t>(sym)].referenced = true;
        sec().fixups.push_back(Fixup{static_cast<uint32_t>(sec().data.size()), k, scaleLog2, sym, line});
    }

    void emit(uint32_t w) {
        Section& s = sec();
        if (s.data.size() % 4) fail("instruction is not 4-byte aligned");
        if (!s.code) {
            s.code = true;
            s.alignLog2 = std::max<uint32_t>(s.alignLog2, 2);
            if (tgt.macho) s.machoFlags |= 0x400;   // S_ATTR_SOME_INSTRUCTIONS
            else s.elfFlags |= 4;
        }
        emitLE(w, 4);
    }

    void need(size_t n) {
        if (ops.size() != n) fail("expected " + std::to_string(n) + " operands");
    }

    Reg reg(SV s) {
        Reg r;
        if (!parseReg(s, r)) fail("expected a register, got '" + str(s) + "'");
        return r;
    }

    Reg gp(SV s) {
        Reg r = reg(s);
        if (!isGp(r)) fail("expected a general register, got '" + str(s) + "'");
        return r;
    }

    // A general register where 31 means the zero register (not sp).
    Reg gpz(SV s) {
        Reg r = gp(s);
        if (r.sp) fail("sp is not valid here");
        return r;
    }

    // A general register where 31 means sp (not the zero register).
    Reg gps(SV s) {
        Reg r = gp(s);
        if (r.zr) fail("the zero register is not valid here");
        return r;
    }

    Reg fpr(SV s) {
        Reg r = reg(s);
        if (r.cls != 's' && r.cls != 'd') fail("expected an s or d register, got '" + str(s) + "'");
        return r;
    }

    int64_t imm(SV s) {
        int64_t v = 0;
        if (!parseInt(s, v)) fail("expected an immediate, got '" + str(s) + "'");
        return v;
    }

    static uint32_t sf(const Reg& r) { return r.cls == 'x' ? 0x80000000u : 0; }
    static uint32_t rn(const Reg& r) { return static_cast<uint32_t>(r.n); }

    void sameWidth(const Reg& a, const Reg& b) {
        if (a.cls != b.cls) fail("operands must be the same width");
    }

    int cond(SV s) {
        int c = parseCond(s);
        if (c < 0) fail("expected a condition, got '" + str(s) + "'");
        return c;
    }

    // add/sub (and adds/subs) of a register, an immediate or a :lo12:/@PAGEOFF reference.
    void addSub(bool sub, bool setf, const Reg& rd, const Reg& rnr, SV third, SV fourth) {
        sameWidth(rd, rnr);
        if (setf ? rd.sp : rd.zr) fail("invalid destination register");
        if (rnr.zr) fail("invalid source register");
        uint32_t base = sf(rd) | (sub ? 0x40000000u : 0) | (setf ? 0x20000000u : 0);
        int64_t v = 0;
        if (parseInt(third, v) && (third[0] == '#' || third[0] == '-' || std::isdigit(static_cast<unsigned char>(third[0])))) {
            uint32_t sh = 0;
            if (!fourth.empty()) {
                Shift s;
                if (!parseShift(fourth, s) || s.kind != 0 || (s.amount != 0 && s.amount != 12)) fail("bad shift");
                sh = s.amount == 12 ? 1 : 0;
                if (v < 0) fail("bad immediate");
            } else if (v < 0) {
                v = -v;
                base ^= 0x40000000u;
            }
            if (!sh && v > 4095 && (v & 0xfff) == 0 && (v >> 12) <= 4095) {
                sh = 1;
                v >>= 12;
            }
            if (v < 0 || v > 4095) fail("immediate out of range");
            emit(base | 0x11000000u | (sh << 22) | (static_cast<uint32_t>(v) << 10) | (rn(rnr) << 5) | rn(rd));
            return;
        }
        Reg rm;
        if (parseReg(third, rm)) {
            if (!isGp(rm) || rm.sp) fail("invalid register operand");
            Shift s;
            if (!fourth.empty() && !parseShift(fourth, s)) fail("bad shift or extend");
            bool extended = rd.sp || rnr.sp || rm.cls != rd.cls || s.kind >= 4;
            if (extended) {
                int option;
                if (s.kind >= 4) option = s.kind - 4;
                else if (s.kind == 0 || s.kind < 0) option = rd.cls == 'x' ? 3 : 2;
                else fail("bad extend");
                if (rm.cls == 'w' && rd.cls == 'x' && (option & 3) == 3) fail("a 32-bit index needs uxtw or sxtw");
                if (s.amount > 4) fail("extend amount out of range");
                emit(base | 0x0b200000u | (rn(rm) << 16) | (static_cast<uint32_t>(option) << 13) |
                     (static_cast<uint32_t>(s.amount) << 10) | (rn(rnr) << 5) | rn(rd));
            } else {
                if (s.kind == 3) fail("ror is not valid here");
                int kind = s.kind < 0 ? 0 : s.kind;
                if (s.amount >= (rd.cls == 'x' ? 64 : 32)) fail("shift out of range");
                emit(base | 0x0b000000u | (static_cast<uint32_t>(kind) << 22) | (rn(rm) << 16) |
                     (static_cast<uint32_t>(s.amount) << 10) | (rn(rnr) << 5) | rn(rd));
            }
            return;
        }
        SymRef r;
        if (parseSymRef(third, r) && (r.mod == Mod::PageOff || r.mod == Mod::Lo12) && fourth.empty() && !sub && !setf) {
            addFixup(Fix::PageOff12, refSymbol(r.name), 0xff);
            emit(base | 0x11000000u | (rn(rnr) << 5) | rn(rd));
            return;
        }
        fail("bad operand '" + str(third) + "'");
    }

    // and/orr/eor/ands (n: the inverted bic/orn/eon/bics) of a register or a bitmask.
    void logical(uint32_t opc, bool invert, const Reg& rd, const Reg& rnr, SV third, SV fourth) {
        sameWidth(rd, rnr);
        if (rnr.sp) fail("sp is not valid here");
        Reg rm;
        if (parseReg(third, rm)) {
            if (rd.sp) fail("sp is not valid here");
            if (!isGp(rm) || rm.sp) fail("invalid register operand");
            sameWidth(rd, rm);
            Shift s;
            if (!fourth.empty() && (!parseShift(fourth, s) || s.kind > 3)) fail("bad shift");
            int kind = s.kind < 0 ? 0 : s.kind;
            if (s.amount >= (rd.cls == 'x' ? 64 : 32)) fail("shift out of range");
            emit(sf(rd) | (opc << 29) | 0x0a000000u | (static_cast<uint32_t>(kind) << 22) | ((invert ? 1u : 0u) << 21) |
                 (rn(rm) << 16) | (static_cast<uint32_t>(s.amount) << 10) | (rn(rnr) << 5) | rn(rd));
            return;
        }
        if (invert || !fourth.empty()) fail("bad operand '" + str(third) + "'");
        if (opc == 3 ? rd.sp : rd.zr) fail("invalid destination register");
        uint32_t enc = 0;
        if (!encodeLogicalImm(static_cast<uint64_t>(imm(third)), rd.cls == 'x' ? 64 : 32, enc))
            fail("immediate is not a valid bitmask");
        emit(sf(rd) | (opc << 29) | 0x12000000u | (enc << 10) | (rn(rnr) << 5) | rn(rd));
    }

    void moveWide(uint32_t opc, const Reg& rd, int64_t value, int shift) {
        if (shift % 16 || shift < 0 || shift >= (rd.cls == 'x' ? 64 : 32)) fail("bad shift");
        if (value < 0 || value > 0xffff) fail("immediate out of range");
        emit(sf(rd) | (opc << 29) | 0x12800000u | (static_cast<uint32_t>(shift / 16) << 21) |
             (static_cast<uint32_t>(value) << 5) | rn(rd));
    }

    // `mov Rd, #imm`: movz, else movn, else orr of a bitmask (as LLVM picks).
    void movImm(const Reg& rd, int64_t value) {
        unsigned bits = rd.cls == 'x' ? 64 : 32;
        uint64_t v = static_cast<uint64_t>(value);
        if (bits == 32) {
            if (value > 0xffffffffLL || value < -0x80000000LL) fail("immediate out of range");
            v &= 0xffffffffULL;
        }
        uint64_t mask = bits == 64 ? ~0ULL : 0xffffffffULL;
        for (int pass = 0; pass < 2; ++pass) {
            uint64_t x = pass == 0 ? v : (~v & mask);
            for (unsigned sh = 0; sh < bits; sh += 16) {
                if ((x & ~(0xffffULL << sh)) == 0) {
                    moveWide(pass == 0 ? 2 : 0, rd, static_cast<int64_t>((x >> sh) & 0xffff), static_cast<int>(sh));
                    return;
                }
            }
        }
        uint32_t enc = 0;
        if (rd.zr || !encodeLogicalImm(v, bits, enc)) fail("immediate cannot be moved in one instruction");
        emit(sf(rd) | (1u << 29) | 0x12000000u | (enc << 10) | (31u << 5) | rn(rd));
    }

    void bitfield(uint32_t opc, const Reg& rd, const Reg& rnr, int64_t immr, int64_t imms) {
        sameWidth(rd, rnr);
        if (rd.sp || rnr.sp) fail("sp is not valid here");
        int64_t size = rd.cls == 'x' ? 64 : 32;
        if (immr < 0 || immr >= size || imms < 0 || imms >= size) fail("bitfield out of range");
        uint32_t n = rd.cls == 'x' ? 1 : 0;
        emit(sf(rd) | (opc << 29) | 0x13000000u | (n << 22) | (static_cast<uint32_t>(immr) << 16) |
             (static_cast<uint32_t>(imms) << 10) | (rn(rnr) << 5) | rn(rd));
    }

    void condSelect(uint32_t op, uint32_t o2, const Reg& rd, const Reg& a, const Reg& b, int c) {
        sameWidth(rd, a);
        sameWidth(rd, b);
        if (rd.sp || a.sp || b.sp) fail("sp is not valid here");
        emit(sf(rd) | (op << 30) | 0x1a800000u | (rn(b) << 16) | (static_cast<uint32_t>(c) << 12) | (o2 << 10) |
             (rn(a) << 5) | rn(rd));
    }

    // ---- Loads and stores ----

    struct Ls {
        uint32_t size = 0, v = 0, opc = 0;
        int scale = 0;
    };

    Ls lsInfo(int mn, const Reg& rt, bool load) {
        Ls l;
        switch (mn) {
            case M_LDR: case M_STR: case M_LDUR: case M_STUR:
                switch (rt.cls) {
                    case 'x': l.size = 3; break;
                    case 'w': l.size = 2; break;
                    case 'b': l.size = 0; l.v = 1; break;
                    case 'h': l.size = 1; l.v = 1; break;
                    case 's': l.size = 2; l.v = 1; break;
                    case 'd': l.size = 3; l.v = 1; break;
                    case 'q': l.size = 0; l.v = 1; l.opc = 2; l.scale = 4; break;
                }
                l.opc |= load ? 1 : 0;
                if (rt.cls != 'q') l.scale = static_cast<int>(l.size);
                if (rt.sp) fail("sp is not valid here");
                return l;
            case M_LDRB: case M_STRB: case M_LDURB: case M_STURB:
                l.size = 0; l.opc = load ? 1 : 0; break;
            case M_LDRH: case M_STRH: case M_LDURH: case M_STURH:
                l.size = 1; l.opc = load ? 1 : 0; break;
            case M_LDRSB: case M_LDURSB:
                l.size = 0; l.opc = rt.cls == 'x' ? 2 : 3; break;
            case M_LDRSH: case M_LDURSH:
                l.size = 1; l.opc = rt.cls == 'x' ? 2 : 3; break;
            case M_LDRSW: case M_LDURSW:
                if (rt.cls != 'x') fail("ldrsw needs an x register");
                l.size = 2; l.opc = 2; break;
        }
        if (!isGp(rt) || rt.sp) fail("expected a general register");
        if ((mn == M_LDRB || mn == M_STRB || mn == M_LDRH || mn == M_STRH || mn == M_LDURB || mn == M_STURB ||
             mn == M_LDURH || mn == M_STURH) && rt.cls != 'w')
            fail("expected a w register");
        l.scale = static_cast<int>(l.size);
        return l;
    }

    void loadStore(int mn, bool load) {
        if (ops.size() < 2 || ops.size() > 3) fail("expected 2 or 3 operands");
        Reg rt = reg(ops[0]);
        Ls l = lsInfo(mn, rt, load);
        bool unscaledOnly = mn >= M_LDUR && mn <= M_LDURSW;
        Mem m;
        if (!parseMem(ops[1], m)) {
            SymRef r;
            if (parseSymRef(ops[1], r)) fail("pc-relative literal loads are not supported");
            fail("bad memory operand '" + str(ops[1]) + "'");
        }
        uint32_t base = (l.size << 30) | (7u << 27) | (l.v << 26) | (l.opc << 22) | (rn(m.base) << 5) | rn(rt);
        auto prePost = [&](int64_t off, uint32_t idx) {
            if (off < -256 || off > 255) fail("offset out of range");
            emit(base | ((static_cast<uint32_t>(off) & 0x1ff) << 12) | (idx << 10));
        };
        if (ops.size() == 3) {
            if (m.kind != Mem::None || m.pre || unscaledOnly) fail("bad post-indexed operand");
            prePost(imm(ops[2]), 1);
            return;
        }
        if (m.pre) {
            if (m.kind != Mem::Imm || unscaledOnly) fail("bad pre-indexed operand");
            prePost(m.imm, 3);
            return;
        }
        if (m.kind == Mem::RegOff) {
            if (unscaledOnly) fail("bad operand");
            int option = 3;
            uint32_t s = 0;
            if (m.ext.kind >= 0) {
                if (m.ext.kind == 0) option = 3;
                else if (m.ext.kind == 4 + 2 || m.ext.kind == 4 + 6 || m.ext.kind == 4 + 7) option = m.ext.kind - 4;
                else fail("bad extend");
                if (m.ext.hasAmount) {
                    if (m.ext.amount != 0 && m.ext.amount != l.scale) fail("bad shift amount");
                    s = (m.ext.amount != 0 || l.scale == 0) ? 1 : 0;
                }
            }
            if ((option == 3 || option == 7) != (m.index.cls == 'x')) fail("index register does not match its extend");
            emit(base | (1u << 21) | (rn(m.index) << 16) | (static_cast<uint32_t>(option) << 13) | (s << 12) | (2u << 10));
            return;
        }
        if (m.kind == Mem::Sym) {
            if (unscaledOnly) fail("bad operand");
            Fix k = Fix::PageOff12;
            if (m.sym.mod == Mod::GotPageOff || m.sym.mod == Mod::GotLo12) {
                if (!load || rt.cls != 'x' || l.v) fail("a GOT load needs ldr of an x register");
                k = Fix::GotPageOff12;
            } else if (m.sym.mod == Mod::PageOff || m.sym.mod == Mod::Lo12) {
                k = Fix::PageOff12;
            } else {
                fail("bad relocation in a memory operand");
            }
            addFixup(k, refSymbol(m.sym.name), static_cast<uint8_t>(l.scale));
            emit(base | (1u << 24));
            return;
        }
        int64_t off = m.kind == Mem::Imm ? m.imm : 0;
        int64_t unit = 1LL << l.scale;
        if (!unscaledOnly && off >= 0 && off % unit == 0 && off / unit <= 4095) {
            emit(base | (1u << 24) | (static_cast<uint32_t>(off / unit) << 10));
            return;
        }
        prePost(off, 0);
    }

    void pair(bool load) {
        if (ops.size() < 3 || ops.size() > 4) fail("expected 3 or 4 operands");
        Reg a = reg(ops[0]), b = reg(ops[1]);
        if (a.cls != b.cls || a.sp || b.sp) fail("bad register pair");
        uint32_t opc = 0, v = 0;
        int scale = 0;
        switch (a.cls) {
            case 'w': opc = 0; scale = 2; break;
            case 'x': opc = 2; scale = 3; break;
            case 's': opc = 0; v = 1; scale = 2; break;
            case 'd': opc = 1; v = 1; scale = 3; break;
            case 'q': opc = 2; v = 1; scale = 4; break;
            default: fail("bad register pair");
        }
        Mem m;
        if (!parseMem(ops[2], m) || (m.kind != Mem::None && m.kind != Mem::Imm)) fail("bad memory operand");
        uint32_t mode;
        int64_t off = m.imm;
        if (ops.size() == 4) {
            if (m.kind != Mem::None || m.pre) fail("bad post-indexed operand");
            mode = 1;
            off = imm(ops[3]);
        } else {
            mode = m.pre ? 3 : 2;
        }
        int64_t unit = 1LL << scale;
        if (off % unit || off / unit < -64 || off / unit > 63) fail("offset out of range");
        emit((opc << 30) | (5u << 27) | (v << 26) | (mode << 23) | ((load ? 1u : 0u) << 22) |
             ((static_cast<uint32_t>(off / unit) & 0x7f) << 15) | (rn(b) << 10) | (rn(m.base) << 5) | rn(a));
    }

    // Exclusive and acquire/release accesses: ld[a]xr, st[l]xr, ldar, stlr (+ b, h).
    bool exclusive(SV mn) {
        struct E { const char* name; uint32_t o2, l, o0; bool status; };
        static const E table[] = {
            {"ldxr", 0, 1, 0, false}, {"ldaxr", 0, 1, 1, false}, {"stxr", 0, 0, 0, true},
            {"stlxr", 0, 0, 1, true}, {"ldar", 1, 1, 1, false}, {"stlr", 1, 0, 1, false},
        };
        for (const E& e : table) {
            size_t n = std::strlen(e.name);
            if (mn.size() < n || mn.compare(0, n, e.name) != 0) continue;
            SV suffix = mn.substr(n);
            if (!(suffix.empty() || suffix == "b" || suffix == "h")) continue;
            size_t want = e.status ? 3 : 2;
            if (ops.size() != want) fail("expected " + std::to_string(want) + " operands");
            Reg rs;
            if (e.status) {
                rs = gpz(ops[0]);
                if (rs.cls != 'w') fail("the status register is a w register");
            }
            Reg rt = gpz(ops[want - 2]);
            Mem m;
            if (!parseMem(ops[want - 1], m) || m.pre || !(m.kind == Mem::None || (m.kind == Mem::Imm && m.imm == 0)))
                fail("expected [xN]");
            uint32_t size = suffix == "b" ? 0 : suffix == "h" ? 1 : rt.cls == 'x' ? 3 : 2;
            if (!suffix.empty() && rt.cls != 'w') fail("expected a w register");
            uint32_t rsBits = e.status ? rn(rs) : 31;
            emit((size << 30) | (0x08u << 24) | (e.o2 << 23) | (e.l << 22) | (rsBits << 16) | (e.o0 << 15) |
                 (31u << 10) | (rn(m.base) << 5) | rn(rt));
            return true;
        }
        return false;
    }

    // LSE atomics: ld<op>[a][l][b|h], st<op>[l][b|h], swp..., cas[a][l][b|h].
    bool lse(SV mn) {
        static const char* ldops[] = {"add", "clr", "eor", "set", "smax", "smin", "umax", "umin"};
        auto orderSize = [&](SV rest, bool allowA, uint32_t& a, uint32_t& r, int& sz) -> bool {
            a = r = 0;
            sz = -1;
            if (!rest.empty() && (rest.back() == 'b' || rest.back() == 'h')) {
                sz = rest.back() == 'b' ? 0 : 1;
                rest = rest.substr(0, rest.size() - 1);
            }
            if (rest.empty()) return true;
            if (rest == "a" && allowA) { a = 1; return true; }
            if (rest == "l") { r = 1; return true; }
            if (rest == "al" && allowA) { a = 1; r = 1; return true; }
            return false;
        };
        auto memBase = [&](SV s) {
            Mem m;
            if (!parseMem(s, m) || m.pre || !(m.kind == Mem::None || (m.kind == Mem::Imm && m.imm == 0))) fail("expected [xN]");
            return m.base;
        };
        auto sizeOf = [&](int sz, const Reg& r) -> uint32_t {
            if (sz >= 0) {
                if (r.cls != 'w') fail("expected a w register");
                return static_cast<uint32_t>(sz);
            }
            return r.cls == 'x' ? 3 : 2;
        };
        uint32_t a, r;
        int sz;
        bool isLd = startsWith(mn, "ld"), isSt = startsWith(mn, "st");
        if (isLd || isSt) {
            for (int op = 0; op < 8; ++op) {
                size_t n = std::strlen(ldops[op]);
                if (mn.size() < 2 + n || mn.compare(2, n, ldops[op]) != 0) continue;
                if (!orderSize(mn.substr(2 + n), isLd, a, r, sz)) continue;
                if (ops.size() != (isLd ? 3u : 2u)) fail("bad operands");
                Reg rs = gpz(ops[0]);
                Reg rt = isLd ? gpz(ops[1]) : Reg{rs.cls, 31, false, true};
                sameWidth(rs, rt);
                Reg base = memBase(ops[isLd ? 2 : 1]);
                emit((sizeOf(sz, rs) << 30) | (0x38u << 24) | (a << 23) | (r << 22) | (1u << 21) | (rn(rs) << 16) |
                     (static_cast<uint32_t>(op) << 12) | (rn(base) << 5) | rn(rt));
                return true;
            }
            return false;
        }
        if (startsWith(mn, "swp") && orderSize(mn.substr(3), true, a, r, sz)) {
            need(3);
            Reg rs = gpz(ops[0]), rt = gpz(ops[1]);
            sameWidth(rs, rt);
            Reg base = memBase(ops[2]);
            emit((sizeOf(sz, rs) << 30) | (0x38u << 24) | (a << 23) | (r << 22) | (1u << 21) | (rn(rs) << 16) |
                 (1u << 15) | (rn(base) << 5) | rn(rt));
            return true;
        }
        if (startsWith(mn, "cas") && orderSize(mn.substr(3), true, a, r, sz)) {
            need(3);
            Reg rs = gpz(ops[0]), rt = gpz(ops[1]);
            sameWidth(rs, rt);
            Reg base = memBase(ops[2]);
            emit((sizeOf(sz, rs) << 30) | (0x11u << 23) | (a << 22) | (1u << 21) | (rn(rs) << 16) | (r << 15) |
                 (31u << 10) | (rn(base) << 5) | rn(rt));
            return true;
        }
        return false;
    }

    // ---- Branches ----

    void branchTo(SV target, Fix k, uint32_t word) {
        SymRef r;
        if (!parseSymRef(target, r) || r.mod != Mod::None) fail("bad branch target '" + str(target) + "'");
        addFixup(k, refSymbol(r.name));
        emit(word);
    }

    // ---- Floating point ----

    static uint32_t ftype(const Reg& r) { return r.cls == 'd' ? 1u : 0u; }

    void fpConvert(uint32_t rmode, uint32_t opcode, const Reg& gpReg, const Reg& fpReg, bool toFp) {
        if (gpReg.sp) fail("sp is not valid here");
        const Reg& d = toFp ? fpReg : gpReg;
        const Reg& n = toFp ? gpReg : fpReg;
        emit(sf(gpReg) | 0x1e200000u | (ftype(fpReg) << 22) | (rmode << 19) | (opcode << 16) | (rn(n) << 5) | rn(d));
    }

    void instruction(SV head, SV rest) {
        char buf[16];
        if (head.size() >= sizeof buf) fail("unknown instruction '" + str(head) + "'");
        for (size_t i = 0; i < head.size(); ++i) buf[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(head[i])));
        SV mn(buf, head.size());
        splitTop(rest, ops);

        if (mn.size() > 2 && mn[0] == 'b' && mn[1] == '.') {
            int c = cond(mn.substr(2));
            need(1);
            branchTo(ops[0], Fix::CondBr19, 0x54000000u | static_cast<uint32_t>(c));
            return;
        }
        const auto& table = mnemonics();
        auto it = table.find(std::string(mn));
        if (it == table.end()) {
            if (exclusive(mn) || lse(mn)) return;
            fail("unsupported instruction '" + str(head) + "'");
        }
        int m = it->second;
        size_t n = ops.size();
        switch (m) {
            case M_ADD: case M_ADDS: case M_SUB: case M_SUBS: {
                if (n != 3 && n != 4) fail("expected 3 or 4 operands");
                addSub(m == M_SUB || m == M_SUBS, m == M_ADDS || m == M_SUBS, gp(ops[0]), gp(ops[1]), ops[2],
                       n == 4 ? ops[3] : SV());
                return;
            }
            case M_CMP: case M_CMN: {
                if (n != 2 && n != 3) fail("expected 2 or 3 operands");
                Reg a = gp(ops[0]);
                Reg zr{a.cls, 31, false, true};
                addSub(m == M_CMP, true, zr, a, ops[1], n == 3 ? ops[2] : SV());
                return;
            }
            case M_NEG: case M_NEGS: {
                if (n != 2 && n != 3) fail("expected 2 or 3 operands");
                Reg d = gp(ops[0]);
                Reg src = gpz(ops[1]);
                sameWidth(d, src);
                Shift s;
                if (n == 3 && (!parseShift(ops[2], s) || s.kind > 2)) fail("bad shift");
                if (m == M_NEG ? d.zr : d.sp) fail("invalid destination register");
                emit(sf(d) | 0x40000000u | (m == M_NEGS ? 0x20000000u : 0) | 0x0b000000u |
                     (static_cast<uint32_t>(s.kind < 0 ? 0 : s.kind) << 22) | (rn(src) << 16) |
                     (static_cast<uint32_t>(s.amount) << 10) | (31u << 5) | rn(d));
                return;
            }
            case M_MOV: {
                need(2);
                Reg d = gp(ops[0]);
                Reg src;
                if (parseReg(ops[1], src)) {
                    if (!isGp(src)) fail("bad source register");
                    sameWidth(d, src);
                    if (d.sp || src.sp) {
                        emit(sf(d) | 0x11000000u | (rn(src) << 5) | rn(d));
                    } else {
                        emit(sf(d) | 0x2a000000u | (rn(src) << 16) | (31u << 5) | rn(d));
                    }
                    return;
                }
                if (d.sp) fail("bad destination register");
                movImm(d, imm(ops[1]));
                return;
            }
            case M_MOVZ: case M_MOVN: case M_MOVK: {
                if (n != 2 && n != 3) fail("expected 2 or 3 operands");
                Reg d = gpz(ops[0]);
                int shift = 0;
                if (n == 3) {
                    Shift s;
                    if (!parseShift(ops[2], s) || s.kind != 0) fail("bad shift");
                    shift = s.amount;
                }
                moveWide(m == M_MOVN ? 0 : m == M_MOVZ ? 2 : 3, d, imm(ops[1]), shift);
                return;
            }
            case M_AND: case M_ANDS: case M_ORR: case M_EOR: case M_BIC: case M_BICS: case M_ORN: case M_EON: {
                if (n != 3 && n != 4) fail("expected 3 or 4 operands");
                uint32_t opc = (m == M_AND || m == M_BIC) ? 0 : (m == M_ORR || m == M_ORN) ? 1 : (m == M_EOR || m == M_EON) ? 2 : 3;
                bool inv = m == M_BIC || m == M_BICS || m == M_ORN || m == M_EON;
                logical(opc, inv, gp(ops[0]), gp(ops[1]), ops[2], n == 4 ? ops[3] : SV());
                return;
            }
            case M_TST: {
                if (n != 2 && n != 3) fail("expected 2 or 3 operands");
                Reg a = gpz(ops[0]);
                logical(3, false, Reg{a.cls, 31, false, true}, a, ops[1], n == 3 ? ops[2] : SV());
                return;
            }
            case M_MVN: {
                if (n != 2 && n != 3) fail("expected 2 or 3 operands");
                Reg d = gpz(ops[0]);
                logical(1, true, d, Reg{d.cls, 31, false, true}, ops[1], n == 3 ? ops[2] : SV());
                return;
            }
            case M_LSL: case M_LSR: case M_ASR: case M_ROR: {
                need(3);
                Reg d = gpz(ops[0]), a = gpz(ops[1]);
                sameWidth(d, a);
                Reg b;
                if (parseReg(ops[2], b)) {
                    if (!isGp(b) || b.sp) fail("bad register");
                    sameWidth(d, b);
                    uint32_t op = m == M_LSL ? 8 : m == M_LSR ? 9 : m == M_ASR ? 10 : 11;
                    emit(sf(d) | 0x1ac00000u | (rn(b) << 16) | (op << 10) | (rn(a) << 5) | rn(d));
                    return;
                }
                int64_t s = imm(ops[2]), size = d.cls == 'x' ? 64 : 32;
                if (s < 0 || s >= size) fail("shift out of range");
                if (m == M_LSL) bitfield(2, d, a, (size - s) % size, size - 1 - s);
                else if (m == M_LSR) bitfield(2, d, a, s, size - 1);
                else if (m == M_ASR) bitfield(0, d, a, s, size - 1);
                else emit(sf(d) | 0x13800000u | (d.cls == 'x' ? 1u << 22 : 0) | (rn(a) << 16) |
                          (static_cast<uint32_t>(s) << 10) | (rn(a) << 5) | rn(d));
                return;
            }
            case M_SDIV: case M_UDIV: {
                need(3);
                Reg d = gpz(ops[0]), a = gpz(ops[1]), b = gpz(ops[2]);
                sameWidth(d, a);
                sameWidth(d, b);
                emit(sf(d) | 0x1ac00000u | (rn(b) << 16) | ((m == M_UDIV ? 2u : 3u) << 10) | (rn(a) << 5) | rn(d));
                return;
            }
            case M_MADD: case M_MSUB: case M_MUL: case M_MNEG: {
                bool three = m == M_MUL || m == M_MNEG;
                need(three ? 3 : 4);
                Reg d = gpz(ops[0]), a = gpz(ops[1]), b = gpz(ops[2]);
                Reg c = three ? Reg{d.cls, 31, false, true} : gpz(ops[3]);
                sameWidth(d, a);
                sameWidth(d, b);
                sameWidth(d, c);
                uint32_t o0 = (m == M_MSUB || m == M_MNEG) ? 1 : 0;
                emit(sf(d) | 0x1b000000u | (rn(b) << 16) | (o0 << 15) | (rn(c) << 10) | (rn(a) << 5) | rn(d));
                return;
            }
            case M_SXTB: case M_SXTH: case M_SXTW: case M_UXTB: case M_UXTH: {
                need(2);
                Reg d = gpz(ops[0]), a = gpz(ops[1]);
                if (a.cls != 'w') fail("the source is a w register");
                if (m == M_SXTW && d.cls != 'x') fail("sxtw writes an x register");
                if ((m == M_UXTB || m == M_UXTH) && d.cls != 'w') fail("expected a w register");
                Reg src{d.cls, a.n, false, a.zr};
                int64_t top = (m == M_SXTB || m == M_UXTB) ? 7 : (m == M_SXTH || m == M_UXTH) ? 15 : 31;
                bitfield((m == M_UXTB || m == M_UXTH) ? 2 : 0, d, src, 0, top);
                return;
            }
            case M_SBFX: case M_UBFX: case M_SBFIZ: case M_UBFIZ: case M_BFI: case M_BFXIL: {
                need(4);
                Reg d = gpz(ops[0]), a = gpz(ops[1]);
                int64_t lsb = imm(ops[2]), width = imm(ops[3]), size = d.cls == 'x' ? 64 : 32;
                if (lsb < 0 || width < 1 || lsb + width > size) fail("bitfield out of range");
                uint32_t opc = (m == M_SBFX || m == M_SBFIZ) ? 0 : (m == M_BFI || m == M_BFXIL) ? 1 : 2;
                if (m == M_SBFX || m == M_UBFX || m == M_BFXIL) bitfield(opc, d, a, lsb, lsb + width - 1);
                else bitfield(opc, d, a, (size - lsb) % size, width - 1);
                return;
            }
            case M_SBFM: case M_UBFM: case M_BFM: {
                need(4);
                bitfield(m == M_SBFM ? 0 : m == M_BFM ? 1 : 2, gpz(ops[0]), gpz(ops[1]), imm(ops[2]), imm(ops[3]));
                return;
            }
            case M_CSEL: case M_CSINC: case M_CSINV: case M_CSNEG: {
                need(4);
                uint32_t op = (m == M_CSINV || m == M_CSNEG) ? 1 : 0, o2 = (m == M_CSINC || m == M_CSNEG) ? 1 : 0;
                condSelect(op, o2, gpz(ops[0]), gpz(ops[1]), gpz(ops[2]), cond(ops[3]));
                return;
            }
            case M_CSET: case M_CSETM: {
                need(2);
                Reg d = gpz(ops[0]);
                int c = cond(ops[1]);
                if (c >= 14) fail("bad condition");
                Reg zr{d.cls, 31, false, true};
                condSelect(m == M_CSETM ? 1 : 0, m == M_CSETM ? 0 : 1, d, zr, zr, c ^ 1);
                return;
            }
            case M_CINC: case M_CINV: case M_CNEG: {
                need(3);
                Reg d = gpz(ops[0]), a = gpz(ops[1]);
                int c = cond(ops[2]);
                if (c >= 14) fail("bad condition");
                uint32_t op = m == M_CINC ? 0 : 1, o2 = m == M_CINV ? 0 : 1;
                condSelect(op, o2, d, a, a, c ^ 1);
                return;
            }
            case M_LDR: case M_LDRB: case M_LDRH: case M_LDRSB: case M_LDRSH: case M_LDRSW:
            case M_LDUR: case M_LDURB: case M_LDURH: case M_LDURSB: case M_LDURSH: case M_LDURSW:
                loadStore(m, true);
                return;
            case M_STR: case M_STRB: case M_STRH: case M_STUR: case M_STURB: case M_STURH:
                loadStore(m, false);
                return;
            case M_LDP: case M_STP:
                pair(m == M_LDP);
                return;
            case M_B:
                need(1);
                branchTo(ops[0], Fix::Jump26, 0x14000000u);
                return;
            case M_BL:
                need(1);
                branchTo(ops[0], Fix::Call26, 0x94000000u);
                return;
            case M_BR: case M_BLR: case M_RET: {
                uint32_t base = m == M_BR ? 0xd61f0000u : m == M_BLR ? 0xd63f0000u : 0xd65f0000u;
                Reg t{'x', 30, false, false};
                if (m == M_RET ? n > 1 : n != 1) fail("bad operands");
                if (n == 1) t = gpz(ops[0]);
                if (t.cls != 'x') fail("expected an x register");
                emit(base | (rn(t) << 5));
                return;
            }
            case M_CBZ: case M_CBNZ: {
                need(2);
                Reg t = gpz(ops[0]);
                branchTo(ops[1], Fix::CondBr19, sf(t) | 0x34000000u | (m == M_CBNZ ? 0x01000000u : 0) | rn(t));
                return;
            }
            case M_TBZ: case M_TBNZ: {
                need(3);
                Reg t = gpz(ops[0]);
                int64_t bit = imm(ops[1]);
                if (bit < 0 || bit >= (t.cls == 'x' ? 64 : 32)) fail("bit number out of range");
                branchTo(ops[2], Fix::TestBr14, (static_cast<uint32_t>(bit >> 5) << 31) | 0x36000000u |
                         (m == M_TBNZ ? 0x01000000u : 0) | (static_cast<uint32_t>(bit & 31) << 19) | rn(t));
                return;
            }
            case M_ADR: {
                need(2);
                Reg d = gpz(ops[0]);
                if (d.cls != 'x') fail("expected an x register");
                branchTo(ops[1], Fix::Adr21, 0x10000000u | rn(d));
                return;
            }
            case M_ADRP: {
                need(2);
                Reg d = gpz(ops[0]);
                if (d.cls != 'x') fail("expected an x register");
                SymRef r;
                if (!parseSymRef(ops[1], r)) fail("bad adrp operand '" + str(ops[1]) + "'");
                Fix k = Fix::Page21;
                if (r.mod == Mod::GotPage || r.mod == Mod::Got) k = Fix::GotPage21;
                else if (r.mod == (tgt.macho ? Mod::Page : Mod::None)) k = Fix::Page21;
                else fail("bad adrp operand '" + str(ops[1]) + "'");
                if ((r.mod == Mod::GotPage) != tgt.macho && k == Fix::GotPage21) fail("bad adrp operand '" + str(ops[1]) + "'");
                addFixup(k, refSymbol(r.name));
                emit(0x90000000u | rn(d));
                return;
            }
            case M_NOP: case M_YIELD: case M_WFE: case M_WFI: case M_SEV: case M_SEVL: {
                need(0);
                static const uint32_t hints[] = {0xd503201fu, 0xd503203fu, 0xd503205fu, 0xd503207fu, 0xd503209fu, 0xd50320bfu};
                emit(hints[m - M_NOP]);
                return;
            }
            case M_BRK: case M_SVC: case M_HLT: {
                need(1);
                int64_t v = imm(ops[0]);
                if (v < 0 || v > 0xffff) fail("immediate out of range");
                uint32_t base = m == M_BRK ? 0xd4200000u : m == M_SVC ? 0xd4000001u : 0xd4400000u;
                emit(base | (static_cast<uint32_t>(v) << 5));
                return;
            }
            case M_CLREX: case M_DMB: case M_DSB: case M_ISB: {
                if (n > 1) fail("bad operands");
                int64_t crm = 15;
                if (n == 1) {
                    static const char* names[] = {"", "oshld", "oshst", "osh", "", "nshld", "nshst", "nsh",
                                                  "", "ishld", "ishst", "ish", "", "ld", "st", "sy"};
                    crm = -1;
                    for (int i = 0; i < 16; ++i)
                        if (names[i][0] && ieq(ops[0], names[i])) crm = i;
                    if (crm < 0 && !parseInt(ops[0], crm)) fail("bad barrier option");
                    if (crm < 0 || crm > 15) fail("bad barrier option");
                } else if (m == M_DMB || m == M_DSB) {
                    fail("expected a barrier option");
                }
                uint32_t base = m == M_CLREX ? 0xd503305fu : m == M_DMB ? 0xd50330bfu : m == M_DSB ? 0xd503309fu : 0xd50330dfu;
                emit(base | (static_cast<uint32_t>(crm) << 8));
                return;
            }
            case M_FMOV: {
                need(2);
                Reg d = reg(ops[0]);
                Reg s;
                if (!parseReg(ops[1], s)) fail("fmov of an immediate is not supported");
                if ((d.cls == 's' || d.cls == 'd') && d.cls == s.cls) {
                    emit(0x1e204000u | (ftype(d) << 22) | (rn(s) << 5) | rn(d));
                } else if ((d.cls == 's' && s.cls == 'w') || (d.cls == 'd' && s.cls == 'x')) {
                    fpConvert(0, 7, s, d, true);
                } else if ((d.cls == 'w' && s.cls == 's') || (d.cls == 'x' && s.cls == 'd')) {
                    fpConvert(0, 6, d, s, false);
                } else {
                    fail("bad fmov operands");
                }
                return;
            }
            case M_FADD: case M_FSUB: case M_FMUL: case M_FDIV: case M_FNMUL: case M_FMAX: case M_FMIN: {
                need(3);
                Reg d = fpr(ops[0]), a = fpr(ops[1]), b = fpr(ops[2]);
                sameWidth(d, a);
                sameWidth(d, b);
                uint32_t op = m == M_FMUL ? 0 : m == M_FDIV ? 1 : m == M_FADD ? 2 : m == M_FSUB ? 3 : m == M_FMAX ? 4 : m == M_FMIN ? 5 : 8;
                emit(0x1e200800u | (ftype(d) << 22) | (rn(b) << 16) | (op << 12) | (rn(a) << 5) | rn(d));
                return;
            }
            case M_FNEG: case M_FABS: case M_FSQRT: {
                need(2);
                Reg d = fpr(ops[0]), a = fpr(ops[1]);
                sameWidth(d, a);
                uint32_t op = m == M_FABS ? 1 : m == M_FNEG ? 2 : 3;
                emit(0x1e204000u | (ftype(d) << 22) | (op << 15) | (rn(a) << 5) | rn(d));
                return;
            }
            case M_FCMP: case M_FCMPE: {
                need(2);
                Reg a = fpr(ops[0]);
                Reg b;
                uint32_t opc = m == M_FCMPE ? 0x10 : 0;
                if (parseReg(ops[1], b)) {
                    if (b.cls != a.cls) fail("operands must be the same width");
                } else {
                    SV z = trim(ops[1]);
                    if (!(z == "#0.0" || z == "#0" || z == "0.0" || z == "#0.0e0")) fail("bad fcmp operand");
                    opc |= 8;
                }
                emit(0x1e202000u | (ftype(a) << 22) | (rn(b) << 16) | (rn(a) << 5) | opc);
                return;
            }
            case M_FCVT: {
                need(2);
                Reg d = fpr(ops[0]), a = fpr(ops[1]);
                if (d.cls == a.cls) fail("fcvt converts between widths");
                emit(0x1e204000u | (ftype(a) << 22) | ((d.cls == 'd' ? 5u : 4u) << 15) | (rn(a) << 5) | rn(d));
                return;
            }
            case M_SCVTF: case M_UCVTF: {
                need(2);
                Reg d = fpr(ops[0]), a = gpz(ops[1]);
                fpConvert(0, m == M_SCVTF ? 2 : 3, a, d, true);
                return;
            }
            case M_FCVTZS: case M_FCVTZU: {
                need(2);
                Reg d = gpz(ops[0]), a = fpr(ops[1]);
                fpConvert(3, m == M_FCVTZS ? 0 : 1, d, a, false);
                return;
            }
            case M_FCSEL: {
                need(4);
                Reg d = fpr(ops[0]), a = fpr(ops[1]), b = fpr(ops[2]);
                sameWidth(d, a);
                sameWidth(d, b);
                emit(0x1e200c00u | (ftype(d) << 22) | (rn(b) << 16) | (static_cast<uint32_t>(cond(ops[3])) << 12) |
                     (rn(a) << 5) | rn(d));
                return;
            }
        }
        fail("unsupported instruction '" + str(head) + "'");
    }

    // ---- Resolution ----

    // Whether a branch to `s` from section `secIdx` is resolved here, not left to the linker:
    // a label of the same section, except (on ELF) a global one, which another definition may
    // preempt.
    bool resolvesLocally(const Symbol& s, int secIdx) const {
        if (!s.defined || s.section != secIdx) return false;
        return tgt.macho || s.temp || !s.global;
    }

    void patchBranch(Section& s, const Fixup& f, int64_t delta) {
        uint32_t w = get32(s.data, f.offset);
        auto range = [&](int bits) {
            int64_t lim = 1LL << (bits + 1);
            if (delta % 4 || delta < -lim || delta >= lim) {
                line = f.line;
                fail("branch target out of range");
            }
        };
        switch (f.kind) {
            case Fix::Jump26: case Fix::Call26:
                range(26);
                w |= static_cast<uint32_t>(delta >> 2) & 0x3ffffff;
                break;
            case Fix::CondBr19:
                range(19);
                w |= (static_cast<uint32_t>(delta >> 2) & 0x7ffff) << 5;
                break;
            case Fix::TestBr14:
                range(14);
                w |= (static_cast<uint32_t>(delta >> 2) & 0x3fff) << 5;
                break;
            case Fix::Adr21:
                if (delta < -(1LL << 20) || delta >= (1LL << 20)) {
                    line = f.line;
                    fail("adr target out of range");
                }
                w |= (static_cast<uint32_t>(delta) & 3) << 29;
                w |= (static_cast<uint32_t>(delta >> 2) & 0x7ffff) << 5;
                break;
            default:
                break;
        }
        put32(s.data, f.offset, w);
    }

    void resolve() {
        relocs.assign(sections.size(), {});
        for (size_t si = 0; si < sections.size(); ++si) {
            Section& sec = sections[si];
            for (const Fixup& f : sec.fixups) {
                Symbol& s = syms[static_cast<size_t>(f.sym)];
                bool branch = f.kind == Fix::Jump26 || f.kind == Fix::Call26 || f.kind == Fix::CondBr19 ||
                              f.kind == Fix::TestBr14 || f.kind == Fix::Adr21;
                if (branch && resolvesLocally(s, static_cast<int>(si))) {
                    patchBranch(sec, f, static_cast<int64_t>(s.offset) - static_cast<int64_t>(f.offset));
                    continue;
                }
                line = f.line;
                if (s.temp && !s.defined) fail("undefined local label '" + displayName(s) + "'");
                Reloc r;
                r.offset = f.offset;
                r.sym = f.sym;
                if (tgt.macho) machoReloc(f, r);
                else elfReloc(f, s, r);
                relocs[si].push_back(r);
            }
        }
        line = 0;
    }

    std::string displayName(const Symbol& s) const {
        std::string n = str(s.name);
        if (!n.empty() && n[0] == '\x01') {
            n = n.substr(1, n.find(':') - 1);
        }
        return n;
    }

    void machoReloc(const Fixup& f, Reloc& r) {
        switch (f.kind) {
            case Fix::Jump26: case Fix::Call26: r.type = 2; r.pcrel = true; break;
            case Fix::Page21: r.type = 3; r.pcrel = true; break;
            case Fix::PageOff12: r.type = 4; break;
            case Fix::GotPage21: r.type = 5; r.pcrel = true; break;
            case Fix::GotPageOff12: r.type = 6; break;
            case Fix::Abs64: r.type = 0; r.length = 3; break;
            case Fix::Abs32: r.type = 0; r.length = 2; break;
            default: fail("this branch needs a label in the same section");
        }
        syms[static_cast<size_t>(r.sym)].inReloc = true;
    }

    void elfReloc(const Fixup& f, const Symbol& s, Reloc& r) {
        bool viaSection = s.defined && (s.temp || !s.global);
        switch (f.kind) {
            case Fix::Jump26: r.type = 282; break;
            case Fix::Call26: r.type = 283; break;
            case Fix::CondBr19: r.type = 280; break;
            case Fix::TestBr14: r.type = 279; break;
            case Fix::Adr21: r.type = 274; break;
            case Fix::Page21: r.type = 275; break;
            case Fix::PageOff12:
                switch (f.scaleLog2) {
                    case 0xff: r.type = 277; break;
                    case 0: r.type = 278; break;
                    case 1: r.type = 284; break;
                    case 2: r.type = 285; break;
                    case 3: r.type = 286; break;
                    default: r.type = 299; break;
                }
                break;
            case Fix::GotPage21: r.type = 311; viaSection = false; break;
            case Fix::GotPageOff12: r.type = 312; viaSection = false; break;
            case Fix::Abs64: r.type = 257; break;
            case Fix::Abs32: r.type = 258; break;
        }
        if (viaSection) {
            r.secSym = s.section;
            r.addend = static_cast<int64_t>(s.offset);
            sections[static_cast<size_t>(s.section)].secSymUsed = true;
            r.sym = -1;
        } else {
            if (s.temp) fail("a GOT reference to a local label");
            syms[static_cast<size_t>(r.sym)].inReloc = true;
        }
    }

    // ---- Mach-O ----

    std::string writeMachO() {
        uint64_t addr = 0;
        for (Section& s : sections) {
            addr = alignTo(addr, 1ULL << s.alignLog2);
            s.addr = addr;
            addr += s.data.size();
        }
        uint64_t vmsize = addr;

        // Symbols: locals in order of appearance, then defined globals and undefined ones,
        // each sorted by name. A local label is kept only when a relocation names it.
        std::vector<int> locals, extdefs, undefs;
        for (size_t i = 0; i < syms.size(); ++i) {
            const Symbol& s = syms[i];
            if (!s.defined) {
                if (s.referenced || s.global) undefs.push_back(static_cast<int>(i));
            } else if (s.global && !s.temp) {
                extdefs.push_back(static_cast<int>(i));
            } else if (!s.temp || s.inReloc) {
                locals.push_back(static_cast<int>(i));
            }
        }
        auto byName = [&](int a, int b) { return syms[static_cast<size_t>(a)].name < syms[static_cast<size_t>(b)].name; };
        std::sort(extdefs.begin(), extdefs.end(), byName);
        std::sort(undefs.begin(), undefs.end(), byName);
        std::vector<int> order;
        order.insert(order.end(), locals.begin(), locals.end());
        order.insert(order.end(), extdefs.begin(), extdefs.end());
        order.insert(order.end(), undefs.begin(), undefs.end());
        std::string strtab(1, '\0');
        std::vector<uint32_t> strx(order.size());
        for (size_t k = 0; k < order.size(); ++k) {
            Symbol& s = syms[static_cast<size_t>(order[k])];
            s.out = static_cast<int>(k);
            strx[k] = static_cast<uint32_t>(strtab.size());
            strtab.append(s.name.data(), s.name.size());
            strtab.push_back('\0');
        }
        strtab.resize(static_cast<size_t>(alignTo(strtab.size(), 8)), '\0');

        uint32_t nsects = static_cast<uint32_t>(sections.size());
        uint32_t segSize = 72 + 80 * nsects;
        uint32_t sizeofcmds = segSize + 24 + 24 + 80;
        uint64_t dataStart = 32 + sizeofcmds;
        uint64_t relocStart = alignTo(dataStart + vmsize, 8);
        std::vector<uint64_t> relOff(sections.size());
        uint64_t off = relocStart;
        for (size_t i = 0; i < sections.size(); ++i) {
            relOff[i] = relocs[i].empty() ? 0 : off;
            off += 8 * relocs[i].size();
        }
        uint64_t symoff = alignTo(off, 8);
        uint64_t stroff = symoff + 16 * order.size();

        std::string o;
        o.reserve(static_cast<size_t>(stroff + strtab.size()));
        out32(o, 0xfeedfacfu);
        out32(o, 0x0100000cu);   // CPU_TYPE_ARM64
        out32(o, 0);
        out32(o, 1);             // MH_OBJECT
        out32(o, 4);
        out32(o, sizeofcmds);
        out32(o, 0);
        out32(o, 0);

        out32(o, 0x19);          // LC_SEGMENT_64
        out32(o, segSize);
        outName16(o, "");
        out64(o, 0);
        out64(o, vmsize);
        out64(o, dataStart);
        out64(o, vmsize);
        out32(o, 7);
        out32(o, 7);
        out32(o, nsects);
        out32(o, 0);
        for (size_t i = 0; i < sections.size(); ++i) {
            const Section& s = sections[i];
            outName16(o, s.sectname);
            outName16(o, s.segname);
            out64(o, s.addr);
            out64(o, s.data.size());
            out32(o, static_cast<uint32_t>(dataStart + s.addr));
            out32(o, s.alignLog2);
            out32(o, static_cast<uint32_t>(relOff[i]));
            out32(o, static_cast<uint32_t>(relocs[i].size()));
            out32(o, s.machoFlags);
            out32(o, 0);
            out32(o, 0);
            out32(o, 0);
        }

        out32(o, 0x32);          // LC_BUILD_VERSION
        out32(o, 24);
        out32(o, tgt.platform);
        out32(o, tgt.minOS);
        out32(o, 0);
        out32(o, 0);

        out32(o, 0x2);           // LC_SYMTAB
        out32(o, 24);
        out32(o, static_cast<uint32_t>(symoff));
        out32(o, static_cast<uint32_t>(order.size()));
        out32(o, static_cast<uint32_t>(stroff));
        out32(o, static_cast<uint32_t>(strtab.size()));

        out32(o, 0xb);           // LC_DYSYMTAB
        out32(o, 80);
        out32(o, 0);
        out32(o, static_cast<uint32_t>(locals.size()));
        out32(o, static_cast<uint32_t>(locals.size()));
        out32(o, static_cast<uint32_t>(extdefs.size()));
        out32(o, static_cast<uint32_t>(locals.size() + extdefs.size()));
        out32(o, static_cast<uint32_t>(undefs.size()));
        for (int i = 0; i < 12; ++i) out32(o, 0);

        for (const Section& s : sections) {
            padTo(o, static_cast<size_t>(dataStart + s.addr));
            o.append(reinterpret_cast<const char*>(s.data.data()), s.data.size());
        }
        padTo(o, static_cast<size_t>(relocStart));
        for (size_t i = 0; i < sections.size(); ++i) {
            // In reverse order of the fixups, as LLVM writes them.
            for (auto it = relocs[i].rbegin(); it != relocs[i].rend(); ++it) {
                const Reloc& r = *it;
                out32(o, r.offset);
                uint32_t symnum = static_cast<uint32_t>(syms[static_cast<size_t>(r.sym)].out);
                out32(o, symnum | ((r.pcrel ? 1u : 0u) << 24) | (static_cast<uint32_t>(r.length) << 25) | (1u << 27) |
                         (r.type << 28));
            }
        }
        padTo(o, static_cast<size_t>(symoff));
        for (size_t k = 0; k < order.size(); ++k) {
            const Symbol& s = syms[static_cast<size_t>(order[k])];
            out32(o, strx[k]);
            uint8_t type = s.defined ? 0x0e : 0x00;
            if (!s.defined || (s.global && !s.temp)) type |= 0x01;
            out8(o, type);
            out8(o, s.defined ? static_cast<uint8_t>(s.section + 1) : 0);
            out16(o, 0);
            out64(o, s.defined ? sections[static_cast<size_t>(s.section)].addr + s.offset : 0);
        }
        o += strtab;
        return o;
    }

    // ---- ELF ----

    std::string writeElf() {
        // Section indexes: 0 null, then each section and its .rela, .note.GNU-stack,
        // .symtab, .strtab.
        struct Hdr {
            uint32_t name = 0, type = 0;
            uint64_t flags = 0, offset = 0, size = 0;
            uint32_t link = 0, info = 0;
            uint64_t align = 1, entsize = 0;
        };
        int next = 1;
        std::vector<int> relaIndex(sections.size(), 0);
        for (size_t i = 0; i < sections.size(); ++i) {
            sections[i].elfIndex = next++;
            if (!relocs[i].empty()) relaIndex[i] = next++;
        }
        int noteIdx = next++, symtabIdx = next++, strtabIdx = next++;
        int shnum = next;

        std::string strtab(1, '\0');
        auto addStr = [&](SV s) {
            uint32_t at = static_cast<uint32_t>(strtab.size());
            strtab.append(s.data(), s.size());
            strtab.push_back('\0');
            return at;
        };

        // Symbols: null; per section its section symbol (when a relocation uses it) and its
        // mapping symbol ($x code, $d data); the other local labels; then the globals.
        struct ESym {
            uint32_t name;
            uint8_t info;
            uint16_t shndx;
            uint64_t value;
        };
        std::vector<ESym> es;
        es.push_back(ESym{0, 0, 0, 0});
        uint32_t mapX = 0, mapD = 0;
        for (Section& s : sections) {
            if (s.secSymUsed) {
                s.elfSecSym = static_cast<int>(es.size());
                es.push_back(ESym{0, 3, static_cast<uint16_t>(s.elfIndex), 0});
            }
            if (s.data.empty()) continue;
            uint32_t& nm = s.code ? mapX : mapD;
            if (!nm) nm = addStr(s.code ? "$x" : "$d");
            es.push_back(ESym{nm, 0, static_cast<uint16_t>(s.elfIndex), 0});
        }
        for (Symbol& s : syms) {
            if (!s.defined || s.temp || s.global) continue;
            s.out = static_cast<int>(es.size());
            es.push_back(ESym{addStr(s.name), 0, static_cast<uint16_t>(sections[static_cast<size_t>(s.section)].elfIndex), s.offset});
        }
        uint32_t firstGlobal = static_cast<uint32_t>(es.size());
        for (Symbol& s : syms) {
            if (s.temp) continue;
            if (s.defined ? !s.global : !(s.referenced || s.global)) continue;
            s.out = static_cast<int>(es.size());
            uint16_t shndx = s.defined ? static_cast<uint16_t>(sections[static_cast<size_t>(s.section)].elfIndex) : 0;
            es.push_back(ESym{addStr(s.name), 0x10, shndx, s.defined ? s.offset : 0});
        }

        std::vector<Hdr> hdr(static_cast<size_t>(shnum));
        std::string o(64, '\0');
        auto place = [&](uint64_t align) {
            padTo(o, static_cast<size_t>(alignTo(o.size(), align)));
            return static_cast<uint64_t>(o.size());
        };
        for (size_t i = 0; i < sections.size(); ++i) {
            const Section& s = sections[i];
            Hdr& h = hdr[static_cast<size_t>(s.elfIndex)];
            h.name = addStr(s.name);
            h.type = s.elfType;
            h.flags = s.elfFlags;
            h.align = 1ULL << s.alignLog2;
            h.entsize = s.entsize;
            h.offset = place(h.align);
            h.size = s.data.size();
            o.append(reinterpret_cast<const char*>(s.data.data()), s.data.size());
        }
        for (size_t i = 0; i < sections.size(); ++i) {
            if (relocs[i].empty()) continue;
            Hdr& h = hdr[static_cast<size_t>(relaIndex[i])];
            h.name = addStr(".rela" + sections[i].name);
            h.type = 4;
            h.flags = 0x40;
            h.link = static_cast<uint32_t>(symtabIdx);
            h.info = static_cast<uint32_t>(sections[i].elfIndex);
            h.align = 8;
            h.entsize = 24;
            h.offset = place(8);
            for (const Reloc& r : relocs[i]) {
                uint64_t symIdx = r.sym >= 0 ? static_cast<uint64_t>(syms[static_cast<size_t>(r.sym)].out)
                                             : static_cast<uint64_t>(sections[static_cast<size_t>(r.secSym)].elfSecSym);
                out64(o, r.offset);
                out64(o, (symIdx << 32) | r.type);
                out64(o, static_cast<uint64_t>(r.addend));
            }
            h.size = o.size() - h.offset;
        }
        {
            Hdr& h = hdr[static_cast<size_t>(noteIdx)];
            h.name = addStr(".note.GNU-stack");
            h.type = 1;
            h.offset = o.size();
        }
        {
            Hdr& h = hdr[static_cast<size_t>(symtabIdx)];
            h.name = addStr(".symtab");
            h.type = 2;
            h.link = static_cast<uint32_t>(strtabIdx);
            h.info = firstGlobal;
            h.align = 8;
            h.entsize = 24;
            h.offset = place(8);
            for (const ESym& e : es) {
                out32(o, e.name);
                out8(o, e.info);
                out8(o, 0);
                out16(o, e.shndx);
                out64(o, e.value);
                out64(o, 0);
            }
            h.size = o.size() - h.offset;
        }
        {
            Hdr& h = hdr[static_cast<size_t>(strtabIdx)];
            h.name = addStr(".strtab");
            h.type = 3;
            h.offset = o.size();
            o += strtab;
            h.size = strtab.size();
        }
        uint64_t shoff = place(8);
        for (const Hdr& h : hdr) {
            out32(o, h.name);
            out32(o, h.type);
            out64(o, h.flags);
            out64(o, 0);
            out64(o, h.offset);
            out64(o, h.size);
            out32(o, h.link);
            out32(o, h.info);
            out64(o, h.align);
            out64(o, h.entsize);
        }

        std::string e;
        e += "\x7f" "ELF";
        out8(e, 2);              // ELFCLASS64
        out8(e, 1);              // little endian
        out8(e, 1);              // EV_CURRENT
        out8(e, 0);              // System V ABI
        e.append(8, '\0');
        out16(e, 1);             // ET_REL
        out16(e, 183);           // EM_AARCH64
        out32(e, 1);
        out64(e, 0);
        out64(e, 0);
        out64(e, shoff);
        out32(e, 0);
        out16(e, 64);
        out16(e, 0);
        out16(e, 0);
        out16(e, 64);
        out16(e, static_cast<uint16_t>(shnum));
        out16(e, static_cast<uint16_t>(strtabIdx));
        o.replace(0, 64, e);
        return o;
    }
};

} // namespace

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
