// eir_as_int.h: the integrated ARM64 assembler's state (Assembler) and helpers, shared by
// eir_as_arm64.cpp and its parts eir_as_*.cpp.
#ifndef EIR_AS_INT_H
#define EIR_AS_INT_H

#include "eir_as_arm64.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace eir {

// The assembler's own names (their own namespace: the other EIR emitters have helpers of
// the same names).
namespace a64as {

using SV = std::string_view;

struct AsmError {
    std::string msg;
};

[[noreturn]] void fail(const std::string& m);

std::string str(SV s);

SV trim(SV s);

bool isIdentStart(char c);

bool isIdentChar(char c);

bool ieq(SV a, const char* b);

bool startsWith(SV s, const char* p);

int hexDigit(char c);

// An integer: `#` optional, a sign, decimal, 0x hex or 0b binary.
bool parseInt(SV s, int64_t& out);

struct Reg {
    char cls = 0;       // x w (general), b h s d q (FP/SIMD)
    int n = 0;
    bool sp = false, zr = false;
};

bool parseReg(SV s, Reg& r);

bool isGp(const Reg& r);

int parseCond(SV s);

// A shift (lsl 0, lsr 1, asr 2, ror 3) or an extend (4 + its option: uxtb ... sxtx).
struct Shift {
    int kind = -1;
    int amount = 0;
    bool hasAmount = false;
};

bool parseShift(SV s, Shift& sh);

enum class Mod { None, Page, PageOff, GotPage, GotPageOff, Lo12, Got, GotLo12 };

struct SymRef {
    SV name;
    Mod mod = Mod::None;
};

bool isSymName(SV s);

bool parseSymRef(SV s, SymRef& r);

struct Mem {
    Reg base;
    enum Kind { None, Imm, RegOff, Sym } kind = None;
    int64_t imm = 0;
    Reg index;
    Shift ext;
    SymRef sym;
    bool pre = false;
};

void splitTop(SV s, std::vector<SV>& out);

bool parseMem(SV s, Mem& m);

bool isShiftedMask(uint64_t v);

int ctz64(uint64_t v);

int cto64(uint64_t v);

int clo64(uint64_t v);

bool encodeLogicalImm(uint64_t imm, unsigned regSize, uint32_t& enc);

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

void put32(std::vector<uint8_t>& d, size_t off, uint32_t v);

uint32_t get32(const std::vector<uint8_t>& d, size_t off);

void out8(std::string& o, uint8_t v);

void out16(std::string& o, uint16_t v);

void out32(std::string& o, uint32_t v);

void out64(std::string& o, uint64_t v);

void outName16(std::string& o, const std::string& n);

void padTo(std::string& o, size_t off);

uint64_t alignTo(uint64_t v, uint64_t a);

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

const std::unordered_map<std::string, int>& mnemonics();

// ---- The assembler ----

class Assembler {
public:
    explicit Assembler(const Arm64ObjTarget& t) : tgt(t) {
        cur = sectionFor(tgt.macho ? "__TEXT" : "", tgt.macho ? "__text" : ".text");
    }

    void run(const std::string& text);

    std::string write();

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

    int sectionFor(const std::string& seg, const std::string& name);

    Section& sec() { return sections[static_cast<size_t>(cur)]; }

    // ---- Symbols ----

    int symbol(SV name);

    int ownedSymbol(const std::string& name);

    // The symbol a reference names: a numeric local label (`1f`, `1b`) is the next or the
    // previous definition of that number.
    int refSymbol(SV name);

    void defineLabel(SV name);

    // ---- Lines ----

    void processLine(SV ln);

    void statement(SV s);

    // ---- Directives ----

    void emitBytes(const void* p, size_t n);

    void emitLE(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) sec().data.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    void align(uint32_t log2);

    void stringData(SV rest, bool zero);

    void dataValues(SV rest, int size);

    void sectionDirective(SV rest);

    void directive(SV name, SV rest);

    // ---- Instructions ----

    void addFixup(Fix k, int sym, uint8_t scaleLog2 = 0xff);

    void emit(uint32_t w);

    void need(size_t n) {
        if (ops.size() != n) fail("expected " + std::to_string(n) + " operands");
    }

    Reg reg(SV s);

    Reg gp(SV s);

    // A general register where 31 means the zero register (not sp).
    Reg gpz(SV s);

    // A general register where 31 means sp (not the zero register).
    Reg gps(SV s);

    Reg fpr(SV s);

    int64_t imm(SV s);

    static uint32_t sf(const Reg& r) { return r.cls == 'x' ? 0x80000000u : 0; }
    static uint32_t rn(const Reg& r) { return static_cast<uint32_t>(r.n); }

    void sameWidth(const Reg& a, const Reg& b) {
        if (a.cls != b.cls) fail("operands must be the same width");
    }

    int cond(SV s);

    // add/sub (and adds/subs) of a register, an immediate or a :lo12:/@PAGEOFF reference.
    void addSub(bool sub, bool setf, const Reg& rd, const Reg& rnr, SV third, SV fourth);

    // and/orr/eor/ands (n: the inverted bic/orn/eon/bics) of a register or a bitmask.
    void logical(uint32_t opc, bool invert, const Reg& rd, const Reg& rnr, SV third, SV fourth);

    void moveWide(uint32_t opc, const Reg& rd, int64_t value, int shift);

    // `mov Rd, #imm`: movz, else movn, else orr of a bitmask (as LLVM picks).
    void movImm(const Reg& rd, int64_t value);

    void bitfield(uint32_t opc, const Reg& rd, const Reg& rnr, int64_t immr, int64_t imms);

    void condSelect(uint32_t op, uint32_t o2, const Reg& rd, const Reg& a, const Reg& b, int c);

    // ---- Loads and stores ----

    struct Ls {
        uint32_t size = 0, v = 0, opc = 0;
        int scale = 0;
    };

    Ls lsInfo(int mn, const Reg& rt, bool load);

    void loadStore(int mn, bool load);

    void pair(bool load);

    // Exclusive and acquire/release accesses: ld[a]xr, st[l]xr, ldar, stlr (+ b, h).
    bool exclusive(SV mn);

    // LSE atomics: ld<op>[a][l][b|h], st<op>[l][b|h], swp..., cas[a][l][b|h].
    bool lse(SV mn);

    // ---- Branches ----

    void branchTo(SV target, Fix k, uint32_t word);

    // ---- Floating point ----

    static uint32_t ftype(const Reg& r) { return r.cls == 'd' ? 1u : 0u; }

    void fpConvert(uint32_t rmode, uint32_t opcode, const Reg& gpReg, const Reg& fpReg, bool toFp);

    void instruction(SV head, SV rest);
    void dataProcessing(int m, size_t n);
    void conditional(int m);
    void memoryBranch(int m, size_t n);
    void systemInst(int m, size_t n);
    void floatInst(int m);

    // ---- Resolution ----

    // Whether a branch to `s` from section `secIdx` is resolved here, not left to the linker:
    // a label of the same section, except (on ELF) a global one, which another definition may
    // preempt.
    bool resolvesLocally(const Symbol& s, int secIdx) const;

    void patchBranch(Section& s, const Fixup& f, int64_t delta);

    void resolve();

    std::string displayName(const Symbol& s) const;

    void machoReloc(const Fixup& f, Reloc& r);

    void elfReloc(const Fixup& f, const Symbol& s, Reloc& r);

    // ---- Mach-O ----

    std::string writeMachO();

    // ---- ELF ----

    std::string writeElf();
};

} // namespace a64as

} // namespace eir

#endif // EIR_AS_INT_H
