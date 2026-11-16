#include "eir_as_int.h"

namespace eir {

namespace a64as {

void Assembler::addFixup(Fix k, int sym, uint8_t scaleLog2) {
    syms[static_cast<size_t>(sym)].referenced = true;
    sec().fixups.push_back(Fixup{static_cast<uint32_t>(sec().data.size()), k, scaleLog2, sym, line});
}

void Assembler::emit(uint32_t w) {
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

Reg Assembler::reg(SV s) {
    Reg r;
    if (!parseReg(s, r)) fail("expected a register, got '" + str(s) + "'");
    return r;
}

Reg Assembler::gp(SV s) {
    Reg r = reg(s);
    if (!isGp(r)) fail("expected a general register, got '" + str(s) + "'");
    return r;
}

Reg Assembler::gpz(SV s) {
    Reg r = gp(s);
    if (r.sp) fail("sp is not valid here");
    return r;
}

Reg Assembler::gps(SV s) {
    Reg r = gp(s);
    if (r.zr) fail("the zero register is not valid here");
    return r;
}

Reg Assembler::fpr(SV s) {
    Reg r = reg(s);
    if (r.cls != 's' && r.cls != 'd') fail("expected an s or d register, got '" + str(s) + "'");
    return r;
}

int64_t Assembler::imm(SV s) {
    int64_t v = 0;
    if (!parseInt(s, v)) fail("expected an immediate, got '" + str(s) + "'");
    return v;
}

int Assembler::cond(SV s) {
    int c = parseCond(s);
    if (c < 0) fail("expected a condition, got '" + str(s) + "'");
    return c;
}

void Assembler::addSub(bool sub, bool setf, const Reg& rd, const Reg& rnr, SV third, SV fourth) {
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

void Assembler::logical(uint32_t opc, bool invert, const Reg& rd, const Reg& rnr, SV third, SV fourth) {
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

void Assembler::moveWide(uint32_t opc, const Reg& rd, int64_t value, int shift) {
    if (shift % 16 || shift < 0 || shift >= (rd.cls == 'x' ? 64 : 32)) fail("bad shift");
    if (value < 0 || value > 0xffff) fail("immediate out of range");
    emit(sf(rd) | (opc << 29) | 0x12800000u | (static_cast<uint32_t>(shift / 16) << 21) |
         (static_cast<uint32_t>(value) << 5) | rn(rd));
}

void Assembler::movImm(const Reg& rd, int64_t value) {
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

void Assembler::bitfield(uint32_t opc, const Reg& rd, const Reg& rnr, int64_t immr, int64_t imms) {
    sameWidth(rd, rnr);
    if (rd.sp || rnr.sp) fail("sp is not valid here");
    int64_t size = rd.cls == 'x' ? 64 : 32;
    if (immr < 0 || immr >= size || imms < 0 || imms >= size) fail("bitfield out of range");
    uint32_t n = rd.cls == 'x' ? 1 : 0;
    emit(sf(rd) | (opc << 29) | 0x13000000u | (n << 22) | (static_cast<uint32_t>(immr) << 16) |
         (static_cast<uint32_t>(imms) << 10) | (rn(rnr) << 5) | rn(rd));
}

void Assembler::condSelect(uint32_t op, uint32_t o2, const Reg& rd, const Reg& a, const Reg& b, int c) {
    sameWidth(rd, a);
    sameWidth(rd, b);
    if (rd.sp || a.sp || b.sp) fail("sp is not valid here");
    emit(sf(rd) | (op << 30) | 0x1a800000u | (rn(b) << 16) | (static_cast<uint32_t>(c) << 12) | (o2 << 10) |
         (rn(a) << 5) | rn(rd));
}

Assembler::Ls Assembler::lsInfo(int mn, const Reg& rt, bool load) {
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

void Assembler::loadStore(int mn, bool load) {
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

void Assembler::pair(bool load) {
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

bool Assembler::exclusive(SV mn) {
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

bool Assembler::lse(SV mn) {
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

void Assembler::branchTo(SV target, Fix k, uint32_t word) {
    SymRef r;
    if (!parseSymRef(target, r) || r.mod != Mod::None) fail("bad branch target '" + str(target) + "'");
    addFixup(k, refSymbol(r.name));
    emit(word);
}

void Assembler::fpConvert(uint32_t rmode, uint32_t opcode, const Reg& gpReg, const Reg& fpReg, bool toFp) {
    if (gpReg.sp) fail("sp is not valid here");
    const Reg& d = toFp ? fpReg : gpReg;
    const Reg& n = toFp ? gpReg : fpReg;
    emit(sf(gpReg) | 0x1e200000u | (ftype(fpReg) << 22) | (rmode << 19) | (opcode << 16) | (rn(n) << 5) | rn(d));
}

} // namespace a64as

} // namespace eir
