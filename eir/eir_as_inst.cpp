#include "eir_as_int.h"

namespace eir {

namespace a64as {

void Assembler::instruction(SV head, SV rest) {
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
        case M_ADD: case M_ADDS: case M_SUB: case M_SUBS: case M_CMP: case M_CMN: case M_NEG:
        case M_NEGS: case M_MOV: case M_MOVZ: case M_MOVN: case M_MOVK: case M_AND: case M_ANDS:
        case M_ORR: case M_EOR: case M_BIC: case M_BICS: case M_ORN: case M_EON: case M_TST:
        case M_MVN: case M_LSL: case M_LSR: case M_ASR: case M_ROR: case M_SDIV: case M_UDIV:
        case M_MADD: case M_MSUB: case M_MUL: case M_MNEG: case M_SXTB: case M_SXTH: case M_SXTW:
        case M_UXTB: case M_UXTH: case M_SBFX: case M_UBFX: case M_SBFIZ: case M_UBFIZ: case M_BFI:
        case M_BFXIL: case M_SBFM: case M_UBFM: case M_BFM:
            dataProcessing(m, n);
            return;
        case M_CSEL: case M_CSINC: case M_CSINV: case M_CSNEG: case M_CSET: case M_CSETM:
        case M_CINC: case M_CINV: case M_CNEG:
            conditional(m);
            return;
        case M_LDR: case M_LDRB: case M_LDRH: case M_LDRSB: case M_LDRSH: case M_LDRSW:
        case M_LDUR: case M_LDURB: case M_LDURH: case M_LDURSB: case M_LDURSH: case M_LDURSW:
        case M_STR: case M_STRB: case M_STRH: case M_STUR: case M_STURB: case M_STURH: case M_LDP:
        case M_STP: case M_B: case M_BL: case M_BR: case M_BLR: case M_RET: case M_CBZ:
        case M_CBNZ: case M_TBZ: case M_TBNZ: case M_ADR: case M_ADRP:
            memoryBranch(m, n);
            return;
        case M_NOP: case M_YIELD: case M_WFE: case M_WFI: case M_SEV: case M_SEVL: case M_BRK:
        case M_SVC: case M_HLT: case M_CLREX: case M_DMB: case M_DSB: case M_ISB:
            systemInst(m, n);
            return;
        case M_FMOV: case M_FADD: case M_FSUB: case M_FMUL: case M_FDIV: case M_FNMUL: case M_FMAX:
        case M_FMIN: case M_FNEG: case M_FABS: case M_FSQRT: case M_FCMP: case M_FCMPE:
        case M_FCVT: case M_SCVTF: case M_UCVTF: case M_FCVTZS: case M_FCVTZU: case M_FCSEL:
            floatInst(m);
            return;
    }
    fail("unsupported instruction '" + str(head) + "'");
}

void Assembler::dataProcessing(int m, size_t n) {
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
        default:
            break;
    }
}

void Assembler::conditional(int m) {
    switch (m) {
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
        default:
            break;
    }
}

void Assembler::memoryBranch(int m, size_t n) {
    switch (m) {
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
        default:
            break;
    }
}

void Assembler::systemInst(int m, size_t n) {
    switch (m) {
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
        default:
            break;
    }
}

void Assembler::floatInst(int m) {
    switch (m) {
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
        default:
            break;
    }
}

} // namespace a64as

} // namespace eir
