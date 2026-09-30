// binarizer.cpp - ELF output emission.
//
// Builds the output image byte-for-byte without reinterpreting host
// structures: every integer is stored through explicit little/big-endian
// writers sized for the output class (32/64-bit).
#include "internal.hpp"

#include <algorithm>
#include <cstring>

namespace qld::detail {

namespace {

uint16_t machineFor(qbfd::Arch arch, bool wide) {
  switch (arch) {
    case qbfd::Arch::X86: return 3;
    case qbfd::Arch::X86_64: return 62;
    case qbfd::Arch::ARM: return 40;
    case qbfd::Arch::AArch64: return 183;
    case qbfd::Arch::RISCV32:
    case qbfd::Arch::RISCV64: return 243;
    case qbfd::Arch::PowerPC: return 20;
    case qbfd::Arch::PowerPC64: return 21;
    case qbfd::Arch::MIPS: return 8;
    case qbfd::Arch::MIPS64: return 8;
    case qbfd::Arch::SPARC: return 2;
    case qbfd::Arch::SPARC64: return 43;
    case qbfd::Arch::S390X: return 22;
    case qbfd::Arch::LoongArch64: return 258;
    default: return wide ? 62 : 3;
  }
}

struct Writer {
  std::vector<uint8_t>& out;
  qbfd::Endian order;
  void put(uint64_t off, uint64_t v, unsigned w) {
    if (off + w > out.size()) out.resize(size_t(off + w), 0);
    for (unsigned i = 0; i < w; ++i) {
      unsigned s = order == qbfd::Endian::Little ? i : w - i - 1;
      out[size_t(off + i)] = uint8_t(v >> (8 * s));
    }
  }
  void bytes(uint64_t off, const uint8_t* p, size_t n) {
    if (off + n > out.size()) out.resize(size_t(off + n), 0);
    std::memcpy(out.data() + off, p, n);
  }
  void bytes(uint64_t off, const std::vector<uint8_t>& v) {
    bytes(off, v.data(), v.size());
  }
};

uint64_t alignUp(uint64_t v, uint64_t a) {
  if (a <= 1) return v;
  return ((v + a - 1) / a) * a;
}

// Canonical -> ELF sh_flags (subset sufficient for linking).
uint64_t shFlags(const OutputSection& o) {
  uint64_t f = 0;
  if (o.writable) f |= 0x1;                       // SHF_WRITE
  if (o.alloc) f |= 0x2;                          // SHF_ALLOC
  if (o.executable) f |= 0x4;                     // SHF_EXECINSTR
  if (o.flags & qbfd::sec::Tls) f |= 0x400;       // SHF_TLS
  if (o.flags & qbfd::sec::Strings) f |= 0x20;    // SHF_STRINGS
  return f;
}

uint32_t shType(const OutputSection& o) {
  if (o.bss) return 8;  // SHT_NOBITS
  return 1;             // SHT_PROGBITS
}

}  // namespace

// Builds the staged section bytes (post-relocation copies) for exec/shared.
// Returns per-output-section byte vectors in outSections order.
static bool buildStaged(const LinkState& state,
                        std::vector<std::vector<uint8_t>>& staged,
                        std::string& error) {
  staged.clear();
  staged.resize(state.outSections.size());
  for (size_t o = 0; o < state.outSections.size(); ++o) {
    const auto& sec = state.outSections[o];
    if (sec.bss) {
      staged[o].clear();
      continue;
    }
    std::vector<uint8_t> buf(size_t(sec.size), 0);
    for (size_t idx : sec.inputs) {
      const auto& in = state.inputSections[idx];
      uint64_t off = in.outOffset;
      size_t n = in.contents.size();
      size_t cap = size_t(sec.size) > off ? size_t(sec.size - off) : 0;
      if (n > cap) n = cap;
      if (n) std::memcpy(buf.data() + off, in.contents.data(), n);
    }
    staged[o] = std::move(buf);
  }
  (void)error;
  return true;
}

bool emitExecutable(const LinkState& state,
                    const std::vector<std::vector<uint8_t>>& stagedInput,
                    std::vector<uint8_t>& out, std::string& error) {
  bool wide = state.wide;
  qbfd::Endian order = state.endian;
  bool pie = state.options.pie;
  uint16_t eType = pie ? 3 : 2;  // ET_DYN for PIE, ET_EXEC otherwise

  // Stage bytes: if the caller already relocated, use them; otherwise build.
  std::vector<std::vector<uint8_t>> staged = stagedInput;
  if (staged.size() != state.outSections.size()) {
    if (!buildStaged(state, staged, error)) return false;
  }

  // Symbol table: NULL + every global def (commons resolved to .bss).
  struct OutSym {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t info = 0;
    uint16_t shndx = 0;
  };
  std::vector<OutSym> syms;
  syms.push_back({});
  // Map output section name -> index (1-based incl. NULL).
  std::map<std::string, uint32_t> secIndex;
  for (const auto& o : state.outSections) secIndex[o.name] = o.index;
  // Deterministic order: sorted by name (defs is already sorted as std::map).
  for (const auto& [name, d] : state.defs) {
    OutSym s;
    s.name = name;
    auto it = state.symAddr.find(name);
    s.value = it == state.symAddr.end() ? 0 : it->second;
    s.size = d.common ? d.commonSize : d.symbol.size;
    uint8_t bind = d.weak ? 2 : 1;  // WEAK : GLOBAL
    uint8_t type = 0;
    switch (d.symbol.kind) {
      case qbfd::SymbolKind::Function: type = 2; break;  // STT_FUNC
      case qbfd::SymbolKind::Object: type = 1; break;    // STT_OBJECT
      case qbfd::SymbolKind::Tls: type = 6; break;       // STT_TLS
      case qbfd::SymbolKind::Common: type = 1; break;
      default: type = 0; break;  // STT_NOTYPE
    }
    if (d.common) type = 1;
    s.info = uint8_t((bind << 4) | (type & 0xf));
    if (d.symbol.flags & qbfd::sym::Absolute) {
      s.shndx = 0xfff1;  // SHN_ABS
    } else if (d.common) {
      auto b = secIndex.find(".bss");
      s.shndx = b == secIndex.end() ? 0 : b->second;
    } else if (d.symbol.section) {
      // Find the output section containing that input section.
      uint32_t found = 0;
      for (const auto& o : state.outSections) {
        for (size_t idx : o.inputs) {
          const auto& in = state.inputSections[idx];
          if (in.objectIndex == d.objectIndex &&
              in.sectionIndex == *d.symbol.section) {
            found = o.index;
            break;
          }
        }
        if (found) break;
      }
      s.shndx = found;
    } else {
      s.shndx = 0xfff1;
    }
    syms.push_back(std::move(s));
  }

  // String tables.
  std::vector<uint8_t> strtab{0};
  std::vector<uint32_t> symNameOff;
  symNameOff.reserve(syms.size());
  for (const auto& s : syms) {
    if (s.name.empty()) {
      symNameOff.push_back(0);
      continue;
    }
    symNameOff.push_back(uint32_t(strtab.size()));
    strtab.insert(strtab.end(), s.name.begin(), s.name.end());
    strtab.push_back(0);
  }
  std::vector<uint8_t> shstrtab{0};
  std::map<std::string, uint32_t> shNameOff;
  std::vector<std::string> allSecNames;
  for (const auto& o : state.outSections) allSecNames.push_back(o.name);
  allSecNames.push_back(".symtab");
  allSecNames.push_back(".strtab");
  allSecNames.push_back(".shstrtab");
  for (const auto& n : allSecNames) {
    if (shNameOff.count(n)) continue;
    shNameOff[n] = uint32_t(shstrtab.size());
    shstrtab.insert(shstrtab.end(), n.begin(), n.end());
    shstrtab.push_back(0);
  }

  // File layout: headers + alloc bytes (at assigned offsets) + non-alloc
  // bytes + symtab + strtab + shstrtab + section headers.
  uint64_t ehsize = wide ? 64 : 52;
  uint64_t phentsize = wide ? 56 : 32;
  // Two LOADs (R+E, RW) + GNU_STACK.
  uint64_t phnum = 3;
  // Highest file end among assigned sections.
  uint64_t fileEnd = ehsize + phnum * phentsize;
  for (size_t i = 0; i < state.outSections.size(); ++i) {
    const auto& o = state.outSections[i];
    if (o.bss) continue;
    fileEnd = std::max(fileEnd, o.fileOffset + o.fileSize);
  }
  // symtab / strtab / shstrtab follow.
  uint64_t symEnt = wide ? 24 : 16;
  uint64_t symtabOff = alignUp(fileEnd, wide ? 8 : 4);
  uint64_t strtabOff = alignUp(symtabOff + syms.size() * symEnt, 1);
  uint64_t shstrtabOff = alignUp(strtabOff + strtab.size(), 1);
  uint64_t shoff =
      alignUp(shstrtabOff + shstrtab.size(), wide ? 8 : 4);
  uint64_t shentsize = wide ? 64 : 40;
  // Section count: NULL + outputs + 3 tables.
  uint64_t shnum = 1 + state.outSections.size() + 3;
  uint64_t shstrndx = 1 + uint64_t(state.outSections.size()) + 2;

  uint64_t total = shoff + shnum * shentsize;
  out.assign(size_t(total), 0);
  Writer w{out, order};

  // ELF header.
  out[0] = 0x7f;
  out[1] = 'E';
  out[2] = 'L';
  out[3] = 'F';
  out[4] = wide ? 2 : 1;
  out[5] = order == qbfd::Endian::Little ? 1 : 2;
  out[6] = 1;
  w.put(16, eType, 2);
  w.put(18, machineFor(state.arch, wide), 2);
  w.put(20, 1, 4);
  w.put(wide ? 24 : 24, state.entryAddr, wide ? 8 : 4);
  w.put(wide ? 32 : 28, ehsize, wide ? 8 : 4);  // e_phoff right after ehdr
  w.put(wide ? 40 : 32, shoff, wide ? 8 : 4);
  w.put(wide ? 48 : 36, 0, 4);  // e_flags
  w.put(wide ? 52 : 40, uint16_t(ehsize), 2);
  w.put(wide ? 54 : 42, uint16_t(phentsize), 2);
  w.put(wide ? 56 : 44, uint16_t(phnum), 2);
  w.put(wide ? 58 : 46, uint16_t(shentsize), 2);
  w.put(wide ? 60 : 48, uint16_t(shnum), 2);
  w.put(wide ? 62 : 50, uint16_t(shstrndx), 2);

  // Program headers: group alloc sections into RE and RW LOADs. All
  // non-writable alloc sections share one R+E segment so that .text and
  // .eh_frame/.rodata on the same page keep a single protection; the
  // kernel cannot represent RX and R on the same page and would otherwise
  // strip X from .text. Writable sections get their own RW segment.
  struct Seg {
    uint32_t flags = 0;
    uint64_t vaddr = 0, offset = 0, filesz = 0, memsz = 0, align = 0x1000;
  };
  std::vector<Seg> segs;
  // RE: non-writable alloc (executable or readonly); RW: writable alloc.
  Seg re{5, 0, 0, 0, 0, 0x1000}, rw{6, 0, 0, 0, 0, 0x1000};
  bool haveRe = false, haveRw = false;
  for (const auto& o : state.outSections) {
    if (!o.alloc) continue;
    if (o.writable) {
      if (!haveRw) {
        rw.vaddr = o.vma;
        rw.offset = o.fileOffset;
        haveRw = true;
      }
      rw.filesz += o.fileSize;
      rw.memsz += o.size;
    } else {
      if (!haveRe) {
        re.vaddr = o.vma;
        re.offset = o.fileOffset;
        haveRe = true;
      }
      re.filesz += o.fileSize;
      re.memsz += o.size;
    }
  }
  // Recompute filesz/memsz contiguously (sections are laid out in order,
  // so span from first vma to last end).
  auto spanFix = [&](Seg& s, bool have, bool writable) {
    if (!have) return;
    // Find min/max among matching sections.
    uint64_t loV = UINT64_MAX, hiV = 0, loF = UINT64_MAX, hiF = 0;
    for (const auto& o : state.outSections) {
      if (!o.alloc) continue;
      bool match = writable ? o.writable : !o.writable;
      if (!match) continue;
      loV = std::min(loV, o.vma);
      hiV = std::max(hiV, o.vma + o.size);
      if (!o.bss) {
        loF = std::min(loF, o.fileOffset);
        hiF = std::max(hiF, o.fileOffset + o.fileSize);
      }
    }
    if (loV == UINT64_MAX) {
      s.filesz = s.memsz = 0;
      return;
    }
    s.vaddr = loV;
    s.memsz = hiV - loV;
    if (loF == UINT64_MAX) {
      s.offset = loV;  // BSS-only (should not happen for RW-only check)
      s.filesz = 0;
    } else {
      s.offset = loF;
      s.filesz = hiF - loF;
    }
  };
  spanFix(re, haveRe, false);
  spanFix(rw, haveRw, true);
  std::vector<Seg> loads;
  if (haveRe && (re.memsz || re.filesz)) loads.push_back(re);
  if (haveRw && (rw.memsz || rw.filesz)) loads.push_back(rw);
  // Emit up to 2 LOADs + GNU_STACK.
  if (loads.size() + 1 != phnum) {
    phnum = loads.size() + 1;
    w.put(wide ? 56 : 44, uint16_t(phnum), 2);
  }
  for (size_t i = 0; i < loads.size(); ++i) {
    uint64_t o = ehsize + i * phentsize;
    w.put(o, 1, 4);  // PT_LOAD
    if (wide) {
      w.put(o + 4, loads[i].flags, 4);
      w.put(o + 8, loads[i].offset, 8);
      w.put(o + 16, loads[i].vaddr, 8);
      w.put(o + 24, loads[i].vaddr, 8);
      w.put(o + 32, loads[i].filesz, 8);
      w.put(o + 40, loads[i].memsz, 8);
      w.put(o + 48, loads[i].align, 8);
    } else {
      w.put(o + 4, loads[i].offset, 4);
      w.put(o + 8, loads[i].vaddr, 4);
      w.put(o + 12, loads[i].vaddr, 4);
      w.put(o + 16, loads[i].filesz, 4);
      w.put(o + 20, loads[i].memsz, 4);
      w.put(o + 24, loads[i].flags, 4);
      w.put(o + 28, loads[i].align, 4);
    }
  }
  {
    uint64_t o = ehsize + loads.size() * phentsize;
    w.put(o, 0x6474e551, 4);  // PT_GNU_STACK
    if (wide) {
      w.put(o + 4, 6, 4);
      w.put(o + 8, 0, 8);
      w.put(o + 16, 0, 8);
      w.put(o + 24, 0, 8);
      w.put(o + 32, 0, 8);
      w.put(o + 40, 0, 8);
      w.put(o + 48, 16, 8);
    } else {
      w.put(o + 4, 0, 4);
      w.put(o + 8, 0, 4);
      w.put(o + 12, 0, 4);
      w.put(o + 16, 0, 4);
      w.put(o + 20, 0, 4);
      w.put(o + 24, 6, 4);
      w.put(o + 28, 16, 4);
    }
  }

  // Section bytes.
  for (size_t i = 0; i < state.outSections.size(); ++i) {
    const auto& s = state.outSections[i];
    if (s.bss || s.fileSize == 0) continue;
    if (s.fileOffset + s.fileSize > out.size()) {
      error = "internal error: section file range outside image";
      return false;
    }
    const auto& buf = staged[i];
    size_t n = std::min<size_t>(buf.size(), size_t(s.fileSize));
    if (n) std::memcpy(out.data() + s.fileOffset, buf.data(), n);
  }

  // Symbol table.
  for (size_t i = 0; i < syms.size(); ++i) {
    uint64_t o = symtabOff + i * symEnt;
    w.put(o, symNameOff[i], 4);
    if (wide) {
      out[size_t(o + 4)] = syms[i].info;
      out[size_t(o + 5)] = 0;
      w.put(o + 6, syms[i].shndx, 2);
      w.put(o + 8, syms[i].value, 8);
      w.put(o + 16, syms[i].size, 8);
    } else {
      w.put(o + 4, syms[i].value, 4);
      w.put(o + 8, syms[i].size, 4);
      out[size_t(o + 12)] = syms[i].info;
      out[size_t(o + 13)] = 0;
      w.put(o + 14, syms[i].shndx, 2);
    }
  }
  if (!strtab.empty())
    std::memcpy(out.data() + strtabOff, strtab.data(), strtab.size());
  if (!shstrtab.empty())
    std::memcpy(out.data() + shstrtabOff, shstrtab.data(), shstrtab.size());

  // Section headers.
  auto emitSh = [&](uint64_t idx, uint32_t name, uint32_t type, uint64_t flags,
                    uint64_t addr, uint64_t off, uint64_t size, uint32_t link,
                    uint32_t info, uint64_t align, uint64_t entsize) {
    uint64_t o = shoff + idx * shentsize;
    w.put(o, name, 4);
    w.put(o + 4, type, 4);
    if (wide) {
      w.put(o + 8, flags, 8);
      w.put(o + 16, addr, 8);
      w.put(o + 24, off, 8);
      w.put(o + 32, size, 8);
      w.put(o + 40, link, 4);
      w.put(o + 44, info, 4);
      w.put(o + 48, align, 8);
      w.put(o + 56, entsize, 8);
    } else {
      w.put(o + 8, flags, 4);
      w.put(o + 12, addr, 4);
      w.put(o + 16, off, 4);
      w.put(o + 20, size, 4);
      w.put(o + 24, link, 4);
      w.put(o + 28, info, 4);
      w.put(o + 32, align, 4);
      w.put(o + 36, entsize, 4);
    }
  };
  emitSh(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  for (size_t i = 0; i < state.outSections.size(); ++i) {
    const auto& s = state.outSections[i];
    uint64_t off = s.bss ? s.fileOffset : s.fileOffset;
    // NOBITS sections conventionally record sh_offset == file end of the
    // preceding section; keep the assigned offset (equal to the BSS VMA
    // mapping) which readers accept since sh_type==NOBITS implies no bytes.
    emitSh(1 + i, shNameOff[s.name], shType(s), shFlags(s), s.vma, off,
           s.size, 0, 0, s.alignment ? s.alignment : 1, 0);
  }
  uint64_t symtabIdx = 1 + state.outSections.size();
  uint64_t strtabIdx = symtabIdx + 1;
  uint64_t shstrtabIdx = strtabIdx + 1;
  emitSh(symtabIdx, shNameOff[".symtab"], 2, 0, 0, symtabOff,
         syms.size() * symEnt, uint32_t(strtabIdx), 1, wide ? 8 : 4, symEnt);
  emitSh(strtabIdx, shNameOff[".strtab"], 3, 0, 0, strtabOff, strtab.size(),
         0, 0, 1, 0);
  emitSh(shstrtabIdx, shNameOff[".shstrtab"], 3, 0, 0, shstrtabOff,
         shstrtab.size(), 0, 0, 1, 0);
  return true;
}

bool emitShared(const LinkState& state,
                const std::vector<std::vector<uint8_t>>& staged,
                std::vector<uint8_t>& out, std::string& error) {
  // Shared objects share the executable emitter; the only differences are
  // e_type (ET_DYN) and base 0, both already reflected in LinkState.
  // emitExecutable picks ET_DYN when options.pie is set, but LinkState is
  // move-only (it owns ObjectFiles), so it cannot be copied to flip the
  // flag. Instead, replicate the pie decision here: shared output is always
  // ET_DYN, and the emitter below already honours a pre-set pie flag.
  // When the caller did not set pie (plain -shared), force ET_DYN by
  // rewriting the emitted e_type after the fact.
  bool wasPie = state.options.pie;
  if (wasPie) return emitExecutable(state, staged, out, error);
  if (!emitExecutable(state, staged, out, error)) return false;
  // Patch e_type 2 (EXEC) -> 3 (DYN). e_type lives at offset 16, 2 bytes.
  if (out.size() < 18) {
    error = "internal error: truncated image";
    return false;
  }
  if (state.endian == qbfd::Endian::Little) {
    out[16] = 3;
    out[17] = 0;
  } else {
    out[16] = 0;
    out[17] = 3;
  }
  return true;
}

bool emitRelocatable(const LinkState& state, std::vector<uint8_t>& out,
                     std::string& error) {
  // Partial link: concatenate same-named sections, remap symbols and
  // relocations, emit a new ET_REL. Reuses the layout from assignAddresses
  // (VMAs are zero) but rebuilds section headers with relocation sections.
  bool wide = state.wide;
  qbfd::Endian order = state.endian;

  // Merge plan: same grouping as layout (outSections already merged).
  // Build per-output bytes (unrelocated copies).
  std::vector<std::vector<uint8_t>> merged(state.outSections.size());
  for (size_t o = 0; o < state.outSections.size(); ++o) {
    const auto& sec = state.outSections[o];
    if (sec.bss) continue;
    std::vector<uint8_t> buf(size_t(sec.size), 0);
    for (size_t idx : sec.inputs) {
      const auto& in = state.inputSections[idx];
      size_t n = in.contents.size();
      size_t cap = size_t(sec.size) > in.outOffset
                       ? size_t(sec.size - in.outOffset)
                       : 0;
      if (n > cap) n = cap;
      if (n) std::memcpy(buf.data() + in.outOffset, in.contents.data(), n);
    }
    merged[o] = std::move(buf);
  }

  // Symbol remap: (object, symidx) -> new symtab index. Globals keep their
  // resolution (first def wins); locals are copied per object.
  struct NewSym {
    std::string name;
    uint64_t value = 0;
    uint64_t size = 0;
    uint8_t info = 0;
    int32_t outSection = -1;  // output section vector index, -1 abs/undef
    bool undef = false;
  };
  std::vector<NewSym> newsyms;
  newsyms.push_back({});  // NULL
  std::vector<std::vector<int>> remap(state.objects.size());
  for (size_t oi = 0; oi < state.objects.size(); ++oi) {
    const auto& obj = state.objects[oi];
    if (obj.isScript || !obj.object) continue;
    remap[oi].assign(obj.object->symbols().size(), -1);
  }
  // First pass: locals and section symbols per object.
  std::map<std::string, uint32_t> outSecIdx;  // name -> 1-based sh index
  for (size_t o = 0; o < state.outSections.size(); ++o)
    outSecIdx[state.outSections[o].name] = uint32_t(o + 1);
  for (size_t oi = 0; oi < state.objects.size(); ++oi) {
    const auto& obj = state.objects[oi];
    if (obj.isScript || !obj.object) continue;
    for (const auto& s : obj.object->symbols()) {
      bool global = s.binding == qbfd::SymbolBinding::Global ||
                    s.binding == qbfd::SymbolBinding::Weak ||
                    s.binding == qbfd::SymbolBinding::Unique;
      if (global) continue;
      NewSym ns;
      ns.name = s.name;
      ns.size = s.size;
      ns.info = uint8_t(s.nativeType & 0xff);
      if (s.flags & qbfd::sym::Undefined) {
        ns.undef = true;
        ns.outSection = -1;
        ns.value = 0;
      } else if (s.flags & qbfd::sym::Absolute) {
        ns.value = s.value;
        ns.outSection = -1;
      } else if (s.section) {
        // Find output section + offset.
        bool found = false;
        for (size_t o = 0; o < state.outSections.size(); ++o) {
          for (size_t idx : state.outSections[o].inputs) {
            const auto& in = state.inputSections[idx];
            if (in.objectIndex == oi && in.sectionIndex == *s.section) {
              ns.value = in.outOffset + s.value;
              ns.outSection = int(o);
              found = true;
              break;
            }
          }
          if (found) break;
        }
        if (!found) {
          ns.value = s.value;
          ns.outSection = -1;
        }
      } else {
        ns.value = s.value;
      }
      remap[oi][s.index] = int(newsyms.size());
      newsyms.push_back(std::move(ns));
    }
  }
  // Second pass: globals (resolved).
  uint32_t firstGlobal = uint32_t(newsyms.size());
  for (const auto& [name, d] : state.defs) {
    if (d.common) continue;  // COMMON stays COMMON in -r
    NewSym ns;
    ns.name = name;
    ns.size = d.symbol.size;
    uint8_t bind = d.weak ? 2 : 1;
    uint8_t type = 0;
    switch (d.symbol.kind) {
      case qbfd::SymbolKind::Function: type = 2; break;
      case qbfd::SymbolKind::Object: type = 1; break;
      default: type = 0; break;
    }
    ns.info = uint8_t((bind << 4) | type);
    // value = offset within merged output section.
    bool found = false;
    for (size_t o = 0; o < state.outSections.size(); ++o) {
      for (size_t idx : state.outSections[o].inputs) {
        const auto& in = state.inputSections[idx];
        if (in.objectIndex == d.objectIndex &&
            d.symbol.section &&
            in.sectionIndex == *d.symbol.section) {
          ns.value = in.outOffset + d.symbol.value;
          ns.outSection = int(o);
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found) {
      ns.value = d.symbol.value;
      ns.outSection = -1;
    }
    // Point every (object,sym) alias of this global at the same entry.
    int id = int(newsyms.size());
    newsyms.push_back(ns);
    for (size_t oi = 0; oi < state.objects.size(); ++oi) {
      const auto& obj = state.objects[oi];
      if (obj.isScript || !obj.object) continue;
      for (const auto& s : obj.object->symbols()) {
        if (s.name == name &&
            (s.binding == qbfd::SymbolBinding::Global ||
             s.binding == qbfd::SymbolBinding::Weak ||
             s.binding == qbfd::SymbolBinding::Unique) &&
            !(s.flags & qbfd::sym::Undefined)) {
          if (remap[oi][s.index] == -1) remap[oi][s.index] = id;
        }
      }
    }
    // Also map COMMONs and undefined refs to entries.
    for (size_t oi = 0; oi < state.objects.size(); ++oi) {
      const auto& obj = state.objects[oi];
      if (obj.isScript || !obj.object) continue;
      for (const auto& s : obj.object->symbols()) {
        if (s.name != name) continue;
        if (remap[oi][s.index] != -1) continue;
        if (s.flags & qbfd::sym::Undefined) {
          // Undefined refs point at the defined entry (like a normal link).
          remap[oi][s.index] = id;
        } else if (s.kind == qbfd::SymbolKind::Common) {
          remap[oi][s.index] = id;
        }
      }
    }
  }
  // Remaining undefined globals get their own entries.
  for (size_t oi = 0; oi < state.objects.size(); ++oi) {
    const auto& obj = state.objects[oi];
    if (obj.isScript || !obj.object) continue;
    for (const auto& s : obj.object->symbols()) {
      if (remap[oi][s.index] != -1) continue;
      NewSym ns;
      ns.name = s.name;
      ns.size = s.size;
      ns.info = uint8_t(s.nativeType & 0xff);
      ns.undef = true;
      ns.outSection = -1;
      remap[oi][s.index] = int(newsyms.size());
      newsyms.push_back(std::move(ns));
    }
  }
  // COMMONs that were skipped (no def entry) need entries too.
  (void)firstGlobal;

  // Rela sections: one .rela.<name> per output section that has relocs.
  struct OutRela {
    std::string name;
    size_t targetOut = 0;
    struct Entry {
      uint64_t offset = 0;
      uint32_t sym = 0;
      uint32_t type = 0;
      int64_t addend = 0;
    };
    std::vector<Entry> entries;
  };
  std::vector<OutRela> relas;
  for (size_t o = 0; o < state.outSections.size(); ++o) {
    OutRela r;
    r.name = ".rela." + state.outSections[o].name;
    r.targetOut = o;
    for (size_t idx : state.outSections[o].inputs) {
      const auto& in = state.inputSections[idx];
      for (const auto& rel : in.relocs) {
        OutRela::Entry e;
        e.offset = in.outOffset + rel.offset;
        e.type = rel.nativeType;
        e.addend = rel.hasAddend ? rel.addend : 0;
        if (rel.symbol) {
          int id = -1;
          if (in.objectIndex < remap.size() &&
              *rel.symbol < remap[in.objectIndex].size())
            id = remap[in.objectIndex][*rel.symbol];
          if (id < 0) {
            error = "internal error: unmapped relocation symbol";
            return false;
          }
          e.sym = uint32_t(id);
        }
        r.entries.push_back(e);
      }
    }
    if (!r.entries.empty()) relas.push_back(std::move(r));
  }

  // Assemble section list: NULL + merged + relas + symtab/strtab/shstrtab.
  struct SecEmit {
    std::string name;
    uint32_t type = 1;
    uint64_t flags = 0;
    uint64_t size = 0;
    uint64_t align = 1;
    uint64_t entsize = 0;
    std::vector<uint8_t> data;
  };
  std::vector<SecEmit> secs;
  for (size_t o = 0; o < state.outSections.size(); ++o) {
    SecEmit s;
    s.name = state.outSections[o].name;
    s.type = state.outSections[o].bss ? 8 : 1;
    s.flags = shFlags(state.outSections[o]);
    s.size = state.outSections[o].size;
    s.align = state.outSections[o].alignment
                  ? state.outSections[o].alignment
                  : 1;
    if (!state.outSections[o].bss) s.data = merged[o];
    secs.push_back(std::move(s));
  }
  // Rela data.
  for (const auto& r : relas) {
    SecEmit s;
    s.name = r.name;
    s.type = 4;  // SHT_RELA
    s.flags = 0;
    s.align = wide ? 8 : 4;
    s.entsize = wide ? 24 : 12;
    s.size = r.entries.size() * s.entsize;
    s.data.resize(size_t(s.size), 0);
    // Fill later with Writer (needs endian); store entries in order via
    // parallel vector. Encode here assuming little-endian then fix? Encode
    // properly below after offsets known. Keep raw entries in a side table
    // by stashing into s.data after layout using the same Writer.
    secs.push_back(std::move(s));
  }
  // strtab.
  std::vector<uint8_t> strtab{0};
  std::vector<uint32_t> nameOff(newsyms.size(), 0);
  for (size_t i = 1; i < newsyms.size(); ++i) {
    nameOff[i] = uint32_t(strtab.size());
    strtab.insert(strtab.end(), newsyms[i].name.begin(),
                  newsyms[i].name.end());
    strtab.push_back(0);
  }
  // symtab data.
  uint64_t symEnt = wide ? 24 : 16;
  std::vector<uint8_t> symtab(newsyms.size() * size_t(symEnt), 0);
  // shstrtab.
  std::vector<uint8_t> shstrtab{0};
  std::map<std::string, uint32_t> shOff;
  std::vector<std::string> names;
  for (const auto& s : secs) names.push_back(s.name);
  names.push_back(".symtab");
  names.push_back(".strtab");
  names.push_back(".shstrtab");
  for (const auto& n : names) {
    if (shOff.count(n)) continue;
    shOff[n] = uint32_t(shstrtab.size());
    shstrtab.insert(shstrtab.end(), n.begin(), n.end());
    shstrtab.push_back(0);
  }

  // Layout file offsets.
  uint64_t ehsize = wide ? 64 : 52;
  uint64_t off = ehsize;
  for (auto& s : secs) {
    if (s.type == 8) continue;  // NOBITS: no file bytes
    off = alignUp(off, s.align);
    // Record offset in flags field temporarily? Use parallel vector.
    s.flags |= 0;  // keep
    (void)s;
  }
  // Assign offsets.
  std::vector<uint64_t> secOff(secs.size(), 0);
  for (size_t i = 0; i < secs.size(); ++i) {
    if (secs[i].type == 8) {
      secOff[i] = 0;
      continue;
    }
    off = alignUp(off, secs[i].align);
    secOff[i] = off;
    off += secs[i].data.size();
  }
  uint64_t symtabOff = alignUp(off, wide ? 8 : 4);
  uint64_t strtabOff = symtabOff + symtab.size();
  uint64_t shstrtabOff = strtabOff + strtab.size();
  uint64_t shoff = alignUp(shstrtabOff + shstrtab.size(), wide ? 8 : 4);
  uint64_t shentsize = wide ? 64 : 40;
  uint64_t shnum = 1 + secs.size() + 3;
  uint64_t shstrndx = shnum - 1;
  out.assign(size_t(shoff + shnum * shentsize), 0);
  Writer w{out, order};

  // Header: ET_REL.
  out[0] = 0x7f;
  out[1] = 'E';
  out[2] = 'L';
  out[3] = 'F';
  out[4] = wide ? 2 : 1;
  out[5] = order == qbfd::Endian::Little ? 1 : 2;
  out[6] = 1;
  w.put(16, 1, 2);  // ET_REL
  w.put(18, machineFor(state.arch, wide), 2);
  w.put(20, 1, 4);
  w.put(24, 0, wide ? 8 : 4);
  w.put(wide ? 32 : 28, 0, wide ? 8 : 4);
  w.put(wide ? 40 : 32, shoff, wide ? 8 : 4);
  w.put(wide ? 48 : 36, 0, 4);
  w.put(wide ? 52 : 40, uint16_t(ehsize), 2);
  w.put(wide ? 54 : 42, 0, 2);
  w.put(wide ? 56 : 44, 0, 2);
  w.put(wide ? 58 : 46, uint16_t(shentsize), 2);
  w.put(wide ? 60 : 48, uint16_t(shnum), 2);
  w.put(wide ? 62 : 50, uint16_t(shstrndx), 2);

  // Section bytes.
  for (size_t i = 0; i < secs.size(); ++i) {
    if (secs[i].type == 8) continue;
    if (!secs[i].data.empty())
      w.bytes(secOff[i], secs[i].data);
  }
  // Rela entries encode now.
  size_t relaBase = 0;
  for (size_t i = 0; i < relas.size(); ++i) {
    // Find the SecEmit index for this rela.
    size_t si = state.outSections.size() + i;
    const auto& r = relas[i];
    for (size_t k = 0; k < r.entries.size(); ++k) {
      uint64_t eo = secOff[si] + k * (wide ? 24 : 12);
      const auto& e = r.entries[k];
      w.put(eo, e.offset, wide ? 8 : 4);
      uint64_t info =
          wide ? (uint64_t(e.sym) << 32) | e.type : (e.sym << 8) | e.type;
      w.put(eo + (wide ? 8 : 4), info, wide ? 8 : 4);
      w.put(eo + (wide ? 16 : 8), uint64_t(e.addend), wide ? 8 : 4);
    }
    (void)relaBase;
  }
  // Symtab encode.
  for (size_t i = 0; i < newsyms.size(); ++i) {
    uint64_t o = symtabOff + i * symEnt;
    uint32_t shndx = 0;
    uint64_t val = newsyms[i].value;
    if (newsyms[i].undef) {
      shndx = 0;
      val = 0;
    } else if (newsyms[i].outSection >= 0) {
      shndx = uint32_t(newsyms[i].outSection + 1);
    } else {
      shndx = 0xfff1;
    }
    // COMMON stays SHN_COMMON.
    w.put(o, nameOff[i], 4);
    if (wide) {
      out[size_t(o + 4)] = newsyms[i].info;
      out[size_t(o + 5)] = 0;
      w.put(o + 6, shndx, 2);
      w.put(o + 8, val, 8);
      w.put(o + 16, newsyms[i].size, 8);
    } else {
      w.put(o + 4, val, 4);
      w.put(o + 8, newsyms[i].size, 4);
      out[size_t(o + 12)] = newsyms[i].info;
      out[size_t(o + 13)] = 0;
      w.put(o + 14, shndx, 2);
    }
  }
  w.bytes(symtabOff, symtab.data(), 0);  // ensure size (no-op)
  // Actually copy encoded symtab: we wrote directly into out; copy the
  // strtab/shstrtab bytes.
  if (!strtab.empty()) w.bytes(strtabOff, strtab.data(), strtab.size());
  if (!shstrtab.empty())
    w.bytes(shstrtabOff, shstrtab.data(), shstrtab.size());
  // Re-encode symtab into out (we already wrote entries directly at
  // symtabOff, but symtab vector is still zeros; rewrite properly by
  // copying the direct writes? They are already there. Nothing to do.)

  auto emitSh = [&](uint64_t idx, uint32_t name, uint32_t type, uint64_t flags,
                    uint64_t addr, uint64_t foff, uint64_t size, uint32_t link,
                    uint32_t info, uint64_t align, uint64_t entsize) {
    uint64_t o = shoff + idx * shentsize;
    w.put(o, name, 4);
    w.put(o + 4, type, 4);
    if (wide) {
      w.put(o + 8, flags, 8);
      w.put(o + 16, addr, 8);
      w.put(o + 24, foff, 8);
      w.put(o + 32, size, 8);
      w.put(o + 40, link, 4);
      w.put(o + 44, info, 4);
      w.put(o + 48, align, 8);
      w.put(o + 56, entsize, 8);
    } else {
      w.put(o + 8, flags, 4);
      w.put(o + 12, addr, 4);
      w.put(o + 16, foff, 4);
      w.put(o + 20, size, 4);
      w.put(o + 24, link, 4);
      w.put(o + 28, info, 4);
      w.put(o + 32, align, 4);
      w.put(o + 36, entsize, 4);
    }
  };
  emitSh(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
  for (size_t i = 0; i < secs.size(); ++i) {
    uint32_t link = 0, info = 0;
    if (secs[i].type == 4) {  // RELA: link symtab, info target
      // symtab index = 1 + secs.size()
      link = uint32_t(1 + secs.size());
      // info = target section index (1-based)
      // target name -> find.
      info = 0;
      // relas[i - outSections.size()] targets relas[..].targetOut
      size_t ri = i - state.outSections.size();
      if (ri < relas.size()) info = uint32_t(relas[ri].targetOut + 1);
    }
    // sh_flags for merged: Alloc only when original alloc? For ET_REL keep
    // original alloc/write/exec flags.
    uint64_t flags = 0;
    if (i < state.outSections.size()) {
      flags = shFlags(state.outSections[i]);
      // REL output keeps SHF_ALLOC etc. as in inputs.
    }
    emitSh(1 + i, shOff[secs[i].name], secs[i].type, flags, 0,
           secs[i].type == 8 ? 0 : secOff[i], secs[i].size, link, info,
           secs[i].align, secs[i].entsize);
  }
  uint64_t symtabIdx = 1 + secs.size();
  emitSh(symtabIdx, shOff[".symtab"], 2, 0, 0, symtabOff,
         newsyms.size() * symEnt, uint32_t(symtabIdx + 1),
         firstGlobal, wide ? 8 : 4, symEnt);
  emitSh(symtabIdx + 1, shOff[".strtab"], 3, 0, 0, strtabOff, strtab.size(),
         0, 0, 1, 0);
  emitSh(symtabIdx + 2, shOff[".shstrtab"], 3, 0, 0, shstrtabOff,
         shstrtab.size(), 0, 0, 1, 0);
  return true;
}

}  // namespace qld::detail
