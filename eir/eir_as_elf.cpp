#include "eir_as_int.h"

namespace eir {

namespace a64as {

std::string Assembler::writeElf() {
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

} // namespace a64as

} // namespace eir
