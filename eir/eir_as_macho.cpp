#include "eir_as_int.h"

namespace eir {

namespace a64as {

bool Assembler::resolvesLocally(const Symbol& s, int secIdx) const {
    if (!s.defined || s.section != secIdx) return false;
    return tgt.macho || s.temp || !s.global;
}

void Assembler::patchBranch(Section& s, const Fixup& f, int64_t delta) {
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

void Assembler::resolve() {
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

std::string Assembler::displayName(const Symbol& s) const {
    std::string n = str(s.name);
    if (!n.empty() && n[0] == '\x01') {
        n = n.substr(1, n.find(':') - 1);
    }
    return n;
}

void Assembler::machoReloc(const Fixup& f, Reloc& r) {
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

void Assembler::elfReloc(const Fixup& f, const Symbol& s, Reloc& r) {
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

std::string Assembler::writeMachO() {
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

} // namespace a64as

} // namespace eir
