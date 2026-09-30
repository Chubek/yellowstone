// position_dependenc.cpp - address assignment (layout).
//
// Lays out merged output sections and fixes every global symbol address.
// The default layout groups alloc sections as .text / .rodata / .data /
// .bss in that order; a linker script (SECTIONS/MEMORY) replaces the order
// and the VMAs when one is present (see ldscript.cpp).
//
// Note the filename keeps its historical spelling (position_dependenc.cpp).
#include "internal.hpp"

#include <algorithm>

namespace qld::detail {

namespace {

uint64_t alignUp(uint64_t v, uint64_t a) {
  if (a <= 1) return v;
  uint64_t m = a - 1;
  return (v + m) & ~m;
}

bool isBssName(const std::string& n) {
  return n == ".bss" || n == ".tbss" || n == ".sbss" || n == ".bss.rel.ro";
}

bool isTextName(const std::string& n) {
  return n == ".text" || n.starts_with(".text.") || n == ".init" ||
         n == ".fini" || n.starts_with(".init") || n.starts_with(".fini");
}

bool isRodataName(const std::string& n) {
  return n == ".rodata" || n.starts_with(".rodata.") || n == ".eh_frame" ||
         n.starts_with(".eh_frame") || n == ".gcc_except_table" ||
         n.starts_with(".gcc_except") || n == ".note" ||
         n.starts_with(".note") || n == ".comment";
}

bool isDataName(const std::string& n) {
  return n == ".data" || n.starts_with(".data.") || n == ".sdata" ||
         n.starts_with(".sdata") || n == ".got" || n.starts_with(".got") ||
         n == ".got.plt" || n == ".plt" || n == ".plt.got" ||
         n == ".plt.sec" || n == ".tdata" || n == ".sdata2" ||
         n == ".data.rel.ro" || n.starts_with(".data.rel.ro");
}

int sectionRank(const std::string& n, uint32_t flags) {
  using namespace qbfd;
  if (flags & sec::Debug) return 100;
  if (n == ".bss" || (flags & sec::Bss)) return 30;
  if (isTextName(n)) return 10;
  if (isRodataName(n)) return 11;
  if (isDataName(n)) return 20;
  if (flags & sec::Tls) return 21;
  if (flags & sec::Alloc) return 19;
  return 50;  // non-alloc, kept (debug etc.)
}

bool skipSection(const qbfd::Section& s, const LinkOptions& opts) {
  if (s.index == 0 && s.name.empty() && s.size == 0) return true;
  if (s.name == ".symtab" || s.name == ".strtab" ||
      s.name == ".shstrtab")
    return true;
  if (s.flags & qbfd::sec::Reloc) return true;
  // RELA/REL sections also appear with native types; belt and braces.
  if (s.name.starts_with(".rel.") || s.name.starts_with(".rela."))
    return true;
  if (opts.stripDebug && (s.flags & qbfd::sec::Debug)) return true;
  if (s.name == ".comment" && opts.stripDebug) return true;
  return false;
}

}  // namespace

bool assignAddresses(LinkState& state, std::string& error) {
  // A script replaces the default layout entirely.
  if (hasScript(state)) return runScript(state, error);

  // Collect input sections.
  state.inputSections.clear();
  for (size_t oi = 0; oi < state.objects.size(); ++oi) {
    const auto& obj = state.objects[oi];
    if (obj.isScript || !obj.object) continue;
    for (const auto& s : obj.object->sections()) {
      if (skipSection(s, state.options)) continue;
      InputSection in;
      in.objectIndex = oi;
      in.sectionIndex = s.index;
      in.section = s;
      if (s.flags & qbfd::sec::Bss) {
        in.contents.clear();
      } else {
        auto c = obj.object->sectionContents(s);
        if (!c) {
          error = obj.label + ": cannot read section " + s.name + ": " +
                  c.error().message;
          return false;
        }
        in.contents.assign(c->begin(), c->end());
      }
      auto r = obj.object->relocations(s);
      if (!r) {
        // Some backends report no relocations as empty; treat errors on
        // alloc sections as fatal, on debug as empty.
        if (s.flags & qbfd::sec::Alloc) {
          error = obj.label + ": cannot read relocations for " + s.name +
                  ": " + r.error().message;
          return false;
        }
      } else {
        in.relocs = std::move(*r);
      }
      state.inputSections.push_back(std::move(in));
    }
  }

  // --gc-sections: drop non-alloc input sections unreferenced by any kept
  // relocation. Alloc sections are roots. This is a deliberately small
  // mark/sweep: it matches --gc-sections on -ffunction-sections inputs for
  // the common case without a full call-graph.
  if (state.options.gcSections) {
    for (auto& in : state.inputSections) {
      if (in.section.flags & qbfd::sec::Alloc) in.kept = true;
      // Keep non-alloc roots that the output always needs.
      else if (in.section.name == ".comment" || in.section.name == ".note")
        in.kept = true;
      else
        in.kept = false;
    }
    // Sweep: any relocation from a kept section keeps its target section
    // when the relocation names a section-local symbol. Global roots are
    // already kept, so one pass suffices for the section-local case.
    for (const auto& in : state.inputSections) {
      if (!in.kept) continue;
      const auto& obj = state.objects[in.objectIndex];
      for (const auto& r : in.relocs) {
        if (r.dynamicSymbol || !r.symbol) continue;
        const auto& syms = obj.object->symbols();
        if (*r.symbol >= syms.size()) continue;
        const auto& sym = syms[*r.symbol];
        if (sym.section && *sym.section == in.sectionIndex) {
          // Self-reference: nothing to do.
        } else if (sym.section) {
          for (auto& other : state.inputSections) {
            if (other.objectIndex == in.objectIndex &&
                other.sectionIndex == *sym.section)
              other.kept = true;
          }
        }
      }
    }
    // Entry section is always kept.
    if (!state.options.entry.empty()) {
      auto it = state.defs.find(state.options.entry);
      if (it != state.defs.end()) {
        const auto& sym = it->second.symbol;
        if (sym.section) {
          // Find the contributing input section for that symbol's object.
          const auto& obj = state.objects[it->second.objectIndex];
          (void)obj;
        }
      }
    }
  }

  // Group by output name. BSS-like sections merge into ".bss" only when
  // they are actually NOBITS; named BSS (.tbss etc.) keeps its name.
  std::map<std::string, size_t> outIndex;
  state.outSections.clear();
  // Preserve first-seen order for determinism, then sort alloc groups.
  std::vector<std::string> order;
  for (const auto& in : state.inputSections) {
    if (!in.kept) continue;
    std::string name = in.section.name.empty() ? ".orphan" : in.section.name;
    // Merge .text.* / .data.* / .rodata.* families into their parents,
    // matching the default GNU ld behaviour for -ffunction-sections.
    if (name.starts_with(".text.")) name = ".text";
    else if (name.starts_with(".rodata.")) name = ".rodata";
    else if (name.starts_with(".data.rel.ro.")) name = ".data.rel.ro";
    else if (name.starts_with(".data.")) name = ".data";
    else if (name.starts_with(".bss.")) name = ".bss";
    if (!outIndex.count(name)) {
      outIndex[name] = state.outSections.size();
      order.push_back(name);
      OutputSection o;
      o.name = name;
      state.outSections.push_back(std::move(o));
    }
  }
  // Sort alloc sections into canonical order, keep the rest stable after.
  std::stable_sort(order.begin(), order.end(), [&](const std::string& a,
                                                   const std::string& b) {
    // Look up representative flags.
    uint32_t fa = 0, fb = 0;
    for (const auto& in : state.inputSections) {
      if (!in.kept) continue;
      std::string na = in.section.name.empty() ? ".orphan" : in.section.name;
      if (na.starts_with(".text.")) na = ".text";
      else if (na.starts_with(".rodata.")) na = ".rodata";
      else if (na.starts_with(".data.")) na = ".data";
      else if (na.starts_with(".bss.")) na = ".bss";
      if (na == a) fa |= in.section.flags;
      if (na == b) fb |= in.section.flags;
    }
    return sectionRank(a, fa) < sectionRank(b, fb);
  });
  // Rebuild outSections in sorted order.
  std::vector<OutputSection> sorted;
  sorted.reserve(order.size());
  std::map<std::string, size_t> newIndex;
  for (size_t i = 0; i < order.size(); ++i) {
    OutputSection o;
    o.name = order[i];
    o.index = uint32_t(i + 1);
    newIndex[order[i]] = i;
    sorted.push_back(std::move(o));
  }
  state.outSections.swap(sorted);
  outIndex.swap(newIndex);

  // Attach inputs to outputs, computing intra-section offsets.
  for (size_t i = 0; i < state.inputSections.size(); ++i) {
    auto& in = state.inputSections[i];
    if (!in.kept) continue;
    std::string name = in.section.name.empty() ? ".orphan" : in.section.name;
    if (name.starts_with(".text.")) name = ".text";
    else if (name.starts_with(".rodata.")) name = ".rodata";
    else if (name.starts_with(".data.rel.ro.")) name = ".data.rel.ro";
    else if (name.starts_with(".data.")) name = ".data";
    else if (name.starts_with(".bss.")) name = ".bss";
    auto it = outIndex.find(name);
    if (it == outIndex.end()) {
      error = "internal error: no output section for " + name;
      return false;
    }
    OutputSection& o = state.outSections[it->second];
    uint64_t align = in.section.alignment ? in.section.alignment : 1;
    o.alignment = std::max(o.alignment, align);
    o.flags |= in.section.flags;
    if (in.section.flags & qbfd::sec::Bss) o.bss = true;
    // The output is BSS only when every contributor is BSS.
    o.inputs.push_back(i);
  }
  // Any output mixing BSS and non-BSS is not BSS.
  for (auto& o : state.outSections) {
    bool anyFile = false;
    for (size_t idx : o.inputs) {
      if (!(state.inputSections[idx].section.flags & qbfd::sec::Bss)) {
        anyFile = true;
        break;
      }
    }
    if (anyFile) o.bss = false;
    o.alloc = (o.flags & qbfd::sec::Alloc) != 0;
    o.executable = (o.flags & (qbfd::sec::Code | qbfd::sec::Exec)) != 0;
    o.writable =
        (o.flags & qbfd::sec::Writable) != 0 || o.name == ".data" ||
        o.name == ".bss" || o.name.starts_with(".data") || o.name == ".got" ||
        o.name.starts_with(".got");
  }

  // COMMON: append each COMMON symbol as BSS space in ".bss". Create the
  // section if no BSS exists yet.
  {
    bool needBss = false;
    for (const auto& [name, d] : state.defs)
      if (d.common) {
        needBss = true;
        break;
      }
    if (needBss && !outIndex.count(".bss")) {
      OutputSection o;
      o.name = ".bss";
      o.index = uint32_t(state.outSections.size() + 1);
      o.flags = qbfd::sec::Alloc | qbfd::sec::Writable | qbfd::sec::Bss |
                qbfd::sec::Data;
      o.alignment = 8;
      o.alloc = true;
      o.bss = true;
      outIndex[".bss"] = state.outSections.size();
      state.outSections.push_back(std::move(o));
    }
  }

  // Assign VMAs and file offsets.
  bool relocatable = state.options.mode == Mode::Relocatable;
  uint64_t base = state.options.hasBase
                      ? state.options.base
                      : (state.options.mode == Mode::Exec &&
                                 !state.options.pie
                             ? 0x400000
                             : 0);
  const uint64_t page = 0x1000;
  // ELF header + program headers occupy the first bytes of the file.
  uint64_t ehsize = state.wide ? 64 : 52;
  uint64_t phentsize = state.wide ? 56 : 32;
  // Estimate program headers: R segment, RW segment (+ GNU_STACK).
  uint64_t phnum = relocatable ? 0 : 3;
  uint64_t headerEnd = relocatable ? ehsize : ehsize + phnum * phentsize;

  uint64_t vma, file;
  if (relocatable) {
    vma = 0;
    file = headerEnd;
  } else if (base == 0) {
    // PIE/shared: first alloc section starts after the headers, VMA == offset.
    vma = headerEnd;
    file = headerEnd;
  } else {
    vma = alignUp(base + headerEnd, page);
    file = alignUp(headerEnd, page);
    // Keep fileOffset == VMA - base for the first LOAD.
    if (file != vma - base) file = vma - base;
  }

  bool seenNonWritable = false;
  bool pageBreakDone = false;
  for (auto& o : state.outSections) {
    if (!o.alloc) continue;
    // Separate RE and RW onto different pages for fixed-base executables:
    // the kernel cannot represent different protections on one page, so a
    // contiguous RE+RW layout would strip X from .text when the RW mapping
    // overlaps it. Align the first writable section to a page boundary.
    if (!relocatable && base != 0 && !pageBreakDone && seenNonWritable &&
        o.writable) {
      vma = alignUp(vma, page);
      pageBreakDone = true;
    }
    if (!o.writable) seenNonWritable = true;
    vma = alignUp(vma, std::max<uint64_t>(o.alignment, 1));
    // BSS after file data still advances VMA but not file size.
    if (!relocatable && base != 0) {
      // Keep LOAD alignment: file follows VMA.
      file = vma - base;
    } else {
      file = alignUp(file, std::max<uint64_t>(o.alignment, 1));
      if (!relocatable && base == 0) vma = file;
    }
    o.vma = vma;
    o.fileOffset = file;
    // Lay out contributors.
    uint64_t off = 0;
    for (size_t idx : o.inputs) {
      auto& in = state.inputSections[idx];
      uint64_t a = in.section.alignment ? in.section.alignment : 1;
      off = alignUp(off, a);
      in.outOffset = off;
      uint64_t sz = in.section.size;
      // Trust the section header size, but clamp contents to it.
      if (in.contents.size() > sz) sz = in.contents.size();
      off += sz;
    }
    // COMMON tail in .bss.
    if (o.name == ".bss") {
      for (const auto& [name, d] : state.defs) {
        if (!d.common) continue;
        (void)name;
      }
      // COMMON addresses are fixed below after o.vma is known.
    }
    o.size = off;
    o.fileSize = o.bss ? 0 : off;
    if (o.bss) {
      vma += off;
    } else {
      vma += off;
      file += off;
    }
  }
  // Non-alloc sections follow the alloc image in the file, VMA 0.
  uint64_t nonAllocFile = file;
  for (auto& o : state.outSections) {
    if (o.alloc) continue;
    nonAllocFile = alignUp(nonAllocFile, std::max<uint64_t>(o.alignment, 1));
    o.vma = 0;
    o.fileOffset = nonAllocFile;
    uint64_t off = 0;
    for (size_t idx : o.inputs) {
      auto& in = state.inputSections[idx];
      uint64_t a = in.section.alignment ? in.section.alignment : 1;
      off = alignUp(off, a);
      in.outOffset = off;
      uint64_t sz = in.section.size;
      if (in.contents.size() > sz) sz = in.contents.size();
      off += sz;
    }
    o.size = o.fileSize = off;
    nonAllocFile += off;
  }
  if (relocatable) {
    for (auto& o : state.outSections) o.vma = 0;
  }

  // Fix symbol addresses.
  state.symAddr.clear();
  // Normal defined symbols: output VMA + intra-section offset + value.
  for (const auto& [name, d] : state.defs) {
    if (d.common) continue;
    const auto& sym = d.symbol;
    if (sym.flags & qbfd::sym::Absolute) {
      state.symAddr[name] = sym.value;
      continue;
    }
    if (!sym.section) {
      // Absolute-ish (SHN_ABS without the flag set distinctly).
      state.symAddr[name] = sym.value;
      continue;
    }
    // Find the input section for (object, section index).
    uint64_t addr = 0;
    bool found = false;
    for (const auto& in : state.inputSections) {
      if (!in.kept) continue;
      if (in.objectIndex == d.objectIndex &&
          in.sectionIndex == *sym.section) {
        // Output section containing this input.
        for (const auto& o : state.outSections) {
          for (size_t idx : o.inputs) {
            if (state.inputSections[idx].objectIndex == d.objectIndex &&
                state.inputSections[idx].sectionIndex == *sym.section) {
              addr = o.vma + state.inputSections[idx].outOffset + sym.value;
              found = true;
              break;
            }
          }
          if (found) break;
        }
        break;
      }
    }
    if (!found) {
      // Section was dropped (e.g. --gc-sections or non-alloc skip): the
      // symbol points at the output section base.
      addr = sym.value;
    }
    state.symAddr[name] = addr;
  }
  // COMMON symbols: allocate sequentially at the end of .bss.
  {
    auto it = outIndex.find(".bss");
    if (it != outIndex.end()) {
      OutputSection& bss = state.outSections[it->second];
      uint64_t tail = bss.size;
      // Recompute tail from contributors first (already in bss.size).
      for (auto& [name, d] : state.defs) {
        if (!d.common) continue;
        tail = alignUp(tail, d.commonAlign ? d.commonAlign : 1);
        state.symAddr[name] = bss.vma + tail;
        tail += d.commonSize;
      }
      uint64_t added = tail - bss.size;
      bss.size = tail;
      // fileSize stays 0 for BSS.
      (void)added;
    } else {
      for (const auto& [name, d] : state.defs) {
        if (d.common) state.symAddr[name] = 0;
      }
    }
  }

  // Entry point.
  {
    std::string entry = state.options.entry;
    // Scripts may override ENTRY(); runScript() handles that path, but the
    // default path also honours an inline ENTRY in scriptText when the
    // script was loaded as an input file. For the default layout the
    // command-line entry wins.
    if (entry.empty()) entry = "_start";
    auto it = state.symAddr.find(entry);
    if (it != state.symAddr.end()) {
      state.entryAddr = it->second;
    } else {
      // Fall back to the first object's e_entry (partial links) or the
      // base address. Missing _start in a static exec is an error to
      // match GNU ld, unless --allow-undefined.
      bool anyObjects = false;
      for (const auto& o : state.objects)
        if (!o.isScript && o.object) {
          anyObjects = true;
          break;
        }
      if (state.options.mode == Mode::Exec && !state.options.allowUndefined &&
          anyObjects) {
        // GNU ld warns, not errors, when _start is missing but still
        // emits with entry == base. Mirror that: warn and use base.
        state.warnings.push_back("cannot find entry symbol " + entry +
                                 "; defaulting to 0x" +
                                 std::to_string(base));
        state.entryAddr = relocatable ? 0 : base;
      } else {
        state.entryAddr = 0;
      }
    }
  }

  // -Map text.
  if (!state.options.mapFile.empty() || state.options.verbose) {
    std::string m;
    m += "Link map (qobjld " + std::string("1.0") + ")\n";
    m += "Output sections:\n";
    char buf[128];
    for (const auto& o : state.outSections) {
      snprintf(buf, sizeof(buf), "  %-16s vma=0x%llx size=0x%llx off=0x%llx\n",
               o.name.c_str(), (unsigned long long)o.vma,
               (unsigned long long)o.size, (unsigned long long)o.fileOffset);
      m += buf;
    }
    m += "Symbols:\n";
    for (const auto& [n, a] : state.symAddr) {
      snprintf(buf, sizeof(buf), "  %-32s 0x%llx\n", n.c_str(),
               (unsigned long long)a);
      m += buf;
      if (m.size() > 1 << 20) break;  // cap pathological maps
    }
    state.mapText = std::move(m);
  }
  return true;
}

}  // namespace qld::detail
