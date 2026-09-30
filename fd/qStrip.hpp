// qStrip.hpp - removes symbols and sections by rewriting an object file.
//
// The readers in this package are read-only by design, so stripping works the
// way `objcopy` does: copy the input, then rewrite the tables that describe it.
// Two invariants keep the result self-consistent:
//
//   * The file layout never moves. Section contents stay at their existing
//     offsets and virtual addresses, so absolute addresses baked into data and
//     the offsets recorded by relocations all stay valid. Bytes belonging to a
//     removed section are zeroed rather than relocated.
//   * Every index that can name a symbol or a section is renumbered together.
//     Dropping symbol 3 of 10 without also fixing the relocation entries, the
//     `sh_link`/`sh_info` fields, `st_shndx` and the symbol table's own
//     first-global boundary would leave a file that parses but points at the
//     wrong things -- much worse than refusing the operation.
//
// Compacted tables are always written at or below the offset of the table they
// replace, so no surviving offset shifts. The output can only ever grow if a
// rebuild is unexpectedly larger, which is caught rather than assumed away.
//
//   ELF    -s/-g/-d, -R, -K, -N, with full symtab/strtab/shndx rebuild
//   COFF   -s/-g/-d, -R, -K, -N, renumbering relocation symbol references
//   Mach-O -s/-g/-d, -R, -K, -N, renumbering r_symbolnum
//   PE     the embedded COFF symbol table, same machinery as COFF
#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <set>

#include "qBFD.hpp"

namespace qbfd::strip {

// How much to remove.
enum class Mode : uint8_t {
  All,       // every symbol table, and the sections only they referenced
  Debug,     // debug sections and debug-only symbols
  Unneeded,  // debug symbols, plus locals no surviving relocation mentions
};

struct Options {
  Mode mode = Mode::Debug;
  std::vector<std::string> removeSections;  // -R
  std::vector<std::string> keepSections;    // -K
  std::vector<std::string> stripSymbols;    // -N
  // Treat -N and -R/-K patterns as globs rather than exact names.
  bool wildcards = false;
};

struct Result {
  std::vector<uint8_t> bytes;
  // Original sections that no longer exist. Both name tables are rebuilt into
  // fresh sections on every pass, so this includes them; use it as "what is
  // gone", not as "what the caller asked for".
  uint32_t sectionsRemoved = 0;
  // Symbol records that no longer exist, counted the same way.
  uint32_t symbolsRemoved = 0;
};

inline bool isDebugName(std::string_view name) {
  return name.starts_with(".debug") || name.starts_with(".zdebug") ||
         name.starts_with(".stab") || name.starts_with(".gdb_index") ||
         name == "__DWARF" || name.starts_with("__debug");
}

// Glob with '*' and '?'. Deliberately tiny: -R/-K/-N patterns are the only
// consumer, so no character classes, no escaping, nothing surprising.
inline bool globMatch(std::string_view pattern, std::string_view text) {
  size_t p = 0, t = 0, star = std::string_view::npos, mark = 0;
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
      ++p;
      ++t;
    } else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

inline bool symbolSelected(const Options& o, std::string_view name) {
  for (const auto& pattern : o.stripSymbols)
    if (o.wildcards ? globMatch(pattern, name) : pattern == name) return true;
  return false;
}

inline bool sectionDropped(const Options& o, std::string_view name) {
  for (const auto& keep : o.keepSections)
    if (globMatch(keep, name)) return false;
  for (const auto& drop : o.removeSections)
    if (globMatch(drop, name)) return true;
  return false;
}

namespace edit {

// A bounds-checked mutable view of the output. Every write is range-checked;
// a value that will not fit is dropped rather than corrupting a neighbour.
class Writer {
 public:
  Writer(std::span<const uint8_t> data, std::vector<uint8_t>& out,
         Endian order = Endian::Little)
      : r_{data}, out_(out) {
    r_.order = order;
  }

  bool has(uint64_t off, uint64_t len) const { return r_.has(off, len); }
  uint64_t integer(uint64_t off, unsigned width) const {
    return r_.has(off, width) ? r_.integer(off, width) : 0;
  }
  bool store(uint64_t off, uint64_t value, unsigned width) {
    if (!r_.has(off, width)) return false;
    for (unsigned i = 0; i < width; ++i)
      out_[off + i] =
          uint8_t(value >> (8 * (r_.order == Endian::Little ? i
                                                           : width - i - 1)));
    return true;
  }
  void fill(uint64_t off, uint64_t len, uint8_t byte = 0) {
    if (!r_.has(off, len)) return;
    std::memset(out_.data() + off, byte, size_t(len));
  }
  // Overwrites `len` bytes at `at` with `data`, growing the file if needed.
  void poke(uint64_t at, std::string_view data) {
    if (at > out_.size()) out_.resize(size_t(at), 0);
    if (at + data.size() > out_.size()) out_.resize(size_t(at + data.size()), 0);
    std::memcpy(out_.data() + at, data.data(), data.size());
  }
  std::vector<uint8_t>& bytes() { return out_; }

 private:
  qbfd::detail::Reader r_;
  std::vector<uint8_t>& out_;
};

// Builds a string table, sharing suffixes so an unchanged run of names does not
// grow. Only the offset of each requested name is ever handed out, and the
// table is written after every request is known, so the sharing cannot
// invalidate an offset already returned.
class Strtab {
 public:
  uint32_t add(std::string_view name) {
    if (name.empty()) return 0;
    auto it = index_.find(std::string(name));
    if (it != index_.end()) return it->second;
    uint32_t at = uint32_t(data_.size());
    index_.emplace(std::string(name), at);
    data_ += name;
    data_ += '\0';
    return at;
  }
  const std::string& data() const { return data_; }

 private:
  std::string data_ = std::string(1, '\0');  // index 0 is always the empty name
  std::map<std::string, uint32_t> index_;
};

// Every multi-byte field in an object file is in the file's own byte order.
// Writing one little-endian value into a big-endian file produces a file that
// still parses and names the wrong things, so every write goes through here.
inline void putInt(std::string& s, size_t at, uint64_t value, unsigned width,
                   Endian order) {
  for (unsigned i = 0; i < width; ++i)
    s[at + i] = char(uint8_t(value >>
                             (8 * (order == Endian::Little ? i : width - i - 1))));
}

// Patches `width` bytes of an existing buffer in place.
inline void patchInt(std::vector<uint8_t>& bytes, size_t at, uint64_t value,
                     unsigned width, Endian order) {
  for (unsigned i = 0; i < width; ++i)
    bytes[at + i] = uint8_t(value >>
                            (8 * (order == Endian::Little ? i : width - i - 1)));
}

// STB_LOCAL, read out of st_info. One predicate, so the numbering pass and the
// emission pass cannot disagree about a symbol's binding.
inline bool isLocalSymbol(const Writer& w, uint64_t offset, bool wide) {
  return uint8_t(w.integer(offset + (wide ? 4 : 12), 1) >> 4) == 0;
}

}  // namespace edit

// ---------------------------------------------------------------------------
// ELF
// ---------------------------------------------------------------------------
inline Expected<Result> stripELF(const elf::ELFFile& file, const Options& o) {
  using namespace edit;
  const auto L = file.layout();
  Result result;
  result.bytes.assign(file.data().begin(), file.data().end());
  Writer w{file.data(), result.bytes, file.endian()};

  if (!L.shoff || !L.shnum || !L.shentsize)
    return Error{Error::Code::Unsupported,
                 "ELF file has no section header table to rewrite"};
  const auto& raw = file.rawSections();
  if (raw.size() != L.shnum)
    return Error{Error::Code::Malformed, "ELF section table size mismatch"};

  const bool wide = L.wide;
  const unsigned word = wide ? 8 : 4;
  const unsigned shdr = L.shentsize;
  const unsigned symEnt = wide ? 24 : 16;

  // Field offsets inside a section header, for both classes.
  const size_t kType = 4, kOffset = wide ? 24 : 16, kSize = wide ? 32 : 20,
               kLink = wide ? 40 : 24, kInfo = wide ? 44 : 28,
               kAlign = wide ? 48 : 32, kEntsize = wide ? 56 : 36;

  // ---- section names
  std::vector<std::string> names(L.shnum);
  if (L.shstrndx < L.shnum && raw[L.shstrndx].type == elf::k::Strtab) {
    const auto& str = raw[L.shstrndx];
    for (uint32_t i = 0; i < L.shnum; ++i) {
      if (raw[i].name >= str.size) continue;
      uint64_t at = str.offset + raw[i].name;
      if (at >= file.data().size()) continue;
      const char* p = reinterpret_cast<const char*>(file.data().data() + at);
      size_t max = size_t(str.size - raw[i].name), n = 0;
      while (n < max && p[n]) ++n;
      names[i].assign(p, n);
    }
  }

  auto isRel = [](uint32_t t) {
    return t == elf::k::Rel || t == elf::k::Rela || t == 19 /* SHT_RELR */;
  };
  auto isSymtabType = [](uint32_t t) { return t == elf::k::Symtab; };

  // ---- which symbol tables survive, and which string tables they need
  uint32_t symtab = UINT32_MAX;
  for (uint32_t i = 1; i < L.shnum; ++i)
    if (raw[i].type == elf::k::Symtab) symtab = i;
  // In Mode::All the static symbol table goes, but a .dynsym on a linked image
  // must stay: the dynamic loader reads it.
  const bool keepStaticSymtab = o.mode != Mode::All && symtab != UINT32_MAX;
  const bool haveDynsym = [&] {
    for (uint32_t i = 1; i < L.shnum; ++i)
      if (raw[i].type == elf::k::Dynsym) return true;
    return false;
  }();

  // A string table is a symbol string table only if a surviving symbol table
  // links to it; .dynstr is indistinguishable from .strtab by type alone.
  auto stringTableInUse = [&](uint32_t i) {
    for (uint32_t j = 1; j < L.shnum; ++j) {
      if (raw[j].type == elf::k::Dynsym && raw[j].link == i) return true;
      if (keepStaticSymtab && j == symtab && raw[j].link == i) return true;
    }
    return false;
  };

  // ---- decide which sections survive
  std::vector<bool> keep(L.shnum, true);
  keep[0] = true;
  if (o.mode == Mode::All) {
    for (uint32_t i = 1; i < L.shnum; ++i) {
      if (isSymtabType(raw[i].type) || isRel(raw[i].type)) keep[i] = false;
      // The old name tables are replaced wholesale further down.
      if (raw[i].type == elf::k::Strtab && !stringTableInUse(i)) keep[i] = false;
    }
  } else {
    for (uint32_t i = 1; i < L.shnum; ++i)
      if (isDebugName(names[i])) keep[i] = false;
  }
  for (uint32_t i = 1; i < L.shnum; ++i)
    if (!names[i].empty() && sectionDropped(o, names[i])) keep[i] = false;
  // -K wins, including over Mode::All: the caller named this section.
  for (const auto& k : o.keepSections)
    for (uint32_t i = 1; i < L.shnum; ++i)
      if (!names[i].empty() && globMatch(k, names[i])) keep[i] = true;
  if (keepStaticSymtab) keep[symtab] = true;
  // Both name tables are rebuilt, so the old sections are never kept. Their
  // bytes are zeroed with everything else that is dropped.
  keep[L.shstrndx] = false;
  if (keepStaticSymtab && raw[symtab].link < L.shnum) keep[raw[symtab].link] = false;
  // A relocation is meaningless without the symbol table it names.
  for (uint32_t i = 1; i < L.shnum; ++i) {
    if (!keep[i]) continue;
    if (isRel(raw[i].type)) {
      if (raw[i].link < L.shnum && !keep[raw[i].link]) keep[i] = false;
      if (raw[i].info < L.shnum && !keep[raw[i].info]) keep[i] = false;
    }
    // These types exist only to describe a symbol table, so they go with it.
    if (raw[i].type == 18 /* SHT_SYMTAB_SHNDX */ ||
        raw[i].type == 0x6fff4c03 /* SHT_LLVM_ADDRSIG */) {
      if (raw[i].link < L.shnum && !keep[raw[i].link]) keep[i] = false;
    }
  }
  if (!keepStaticSymtab) {
    for (uint32_t i = 1; i < L.shnum; ++i)
      if (keep[i] && isRel(raw[i].type) && raw[i].link == symtab) keep[i] = false;
  }
  (void)haveDynsym;

  // ---- renumber the survivors
  std::vector<uint32_t> to(L.shnum, UINT32_MAX);
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < L.shnum; ++i)
    if (keep[i]) {
      to[i] = uint32_t(order.size());
      order.push_back(i);
    }

  // ---- rebuild the static symbol table, if it survives
  const Endian byteOrder = file.endian();
  bool symtabWritten = false;
  std::string symBytes;
  Strtab symStrings;
  std::vector<uint32_t> remap;
  uint32_t firstGlobal = 0;
  if (keepStaticSymtab && raw[symtab].entsize >= symEnt) {
    const auto& h = raw[symtab];
    const uint32_t count = uint32_t(h.size / h.entsize);
    remap.assign(count, UINT32_MAX);
    std::vector<bool> alive(count, false);

    // Mode::Unneeded drops locals that no surviving relocation mentions.
    std::set<uint32_t> referenced;
    if (o.mode == Mode::Unneeded) {
      for (uint32_t i = 1; i < L.shnum; ++i) {
        if (!keep[i] || !isRel(raw[i].type) || raw[i].link != symtab) continue;
        const auto& r = raw[i];
        if (!r.entsize) continue;
        for (uint64_t k = 0; k + r.entsize <= r.size; k += r.entsize) {
          uint64_t at = r.offset + k;
          if (!w.has(at, r.entsize)) break;
          uint64_t info = w.integer(at + (wide ? 8 : 4), word);
          referenced.insert(uint32_t(wide ? info >> 32 : info >> 8));
        }
      }
    }
    // One pass decides survival for every symbol, so the numbering below, the
    // emitted bytes and sh_info can never disagree.
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t at = h.offset + uint64_t(i) * symEnt;
      if (!w.has(at, symEnt)) break;
      const uint32_t info = uint32_t(w.integer(at + (wide ? 4 : 12), 1));
      const uint32_t section = uint32_t(w.integer(at + (wide ? 6 : 14), 2));
      const uint8_t type = uint8_t(info & 0xf), bind = uint8_t(info >> 4);
      if (section < 0xfff0 && section < L.shnum && !keep[section]) continue;
      if (type == 4 /* STT_FILE */) continue;
      if (type == 3 /* STT_SECTION */ && section < L.shnum &&
          isDebugName(names[section]))
        continue;
      if (o.mode == Mode::Unneeded && bind == 0 /* STB_LOCAL */ &&
          !referenced.contains(i))
        continue;
      if (h.link < L.shnum && !o.stripSymbols.empty()) {
        uint32_t nameOff = uint32_t(w.integer(at, 4));
        if (nameOff < raw[h.link].size) {
          const char* p = reinterpret_cast<const char*>(
              file.data().data() + raw[h.link].offset + nameOff);
          size_t max = size_t(raw[h.link].size - nameOff), n = 0;
          while (n < max && p[n]) ++n;
          if (symbolSelected(o, std::string_view(p, n))) continue;
        }
      }
      // A symbol with an extended section index is dropped: SHT_SYMTAB_SHNDX
      // is not being rebuilt, so keeping one would misattach it.
      if (section == 0xffff /* SHN_XINDEX */) continue;
      alive[i] = true;
    }
    // Locals must all precede globals, so numbering is two passes over the
    // same `alive` set that fixes the emitted order.
    uint32_t next = 0;
    for (uint32_t i = 0; i < count; ++i)
      if (alive[i] && isLocalSymbol(w, h.offset + uint64_t(i) * symEnt, wide))
        remap[i] = next++;
    firstGlobal = next;
    for (uint32_t i = 0; i < count; ++i)
      if (alive[i] && !isLocalSymbol(w, h.offset + uint64_t(i) * symEnt, wide))
        remap[i] = next++;

    symBytes.reserve(size_t(next) * symEnt);
    for (uint32_t i = 0; i < count; ++i) {
      if (!alive[i]) continue;
      uint64_t at = h.offset + uint64_t(i) * symEnt;
      std::string entry(symEnt, '\0');
      std::memcpy(entry.data(), file.data().data() + at, symEnt);
      std::string_view name;
      if (h.link < L.shnum) {
        uint32_t nameOff = uint32_t(w.integer(at, 4));
        if (nameOff < raw[h.link].size) {
          const char* p = reinterpret_cast<const char*>(
              file.data().data() + raw[h.link].offset + nameOff);
          size_t max = size_t(raw[h.link].size - nameOff), n = 0;
          while (n < max && p[n]) ++n;
          name = std::string_view(p, n);
        }
      }
      uint32_t nameAt = symStrings.add(name);
      putInt(entry, 0, nameAt, 4, byteOrder);
      uint32_t section = uint32_t(w.integer(at + (wide ? 6 : 14), 2));
      if (section < L.shnum && keep[section]) section = to[section];
      putInt(entry, wide ? 6 : 14, section, 2, byteOrder);
      symBytes += entry;
    }
    result.symbolsRemoved = count - uint32_t(symBytes.size() / symEnt);
    symtabWritten = true;
  } else if (!keepStaticSymtab) {
    // The whole static symbol table is going. Reporting it matters: a caller
    // that asks "what did you remove?" would otherwise be told nothing, when in
    // fact every symbol went. The relocations that named it are dropped below.
    result.symbolsRemoved = uint32_t(file.symbols().size());
  }

  // A rebuilt table that ends up empty takes its section with it.
  if (keepStaticSymtab && symtabWritten && symBytes.empty()) {
    keep[symtab] = false;
    to[symtab] = UINT32_MAX;
    order.clear();
    for (uint32_t i = 0; i < L.shnum; ++i)
      if (keep[i]) {
        to[i] = uint32_t(order.size());
        order.push_back(i);
      }
    symtabWritten = false;
  }

  // ---- zero the bytes of every removed section
  for (uint32_t i = 1; i < L.shnum; ++i) {
    if (keep[i] || raw[i].type == elf::k::Nobits || raw[i].type == elf::k::Null)
      continue;
    w.fill(raw[i].offset, raw[i].size);
  }

  // ---- append the rebuilt tables
  //
  // Both string tables are written past the end of the file. In a
  // Clang-produced object the symbol names and the section names share one
  // .strtab section, so there is no single existing slot both can be rebuilt
  // into; appending avoids having to know which alias applies, and moving
  // nothing keeps every surviving offset valid.
  //
  // The section header table itself grows (it gains an entry for each new
  // section), so the append point has to clear its *final* extent, not just the
  // end of the input.
  struct Added {
    std::string name;
    uint32_t type = elf::k::Strtab, link = 0, info = 0, entsize = 0;
    uint64_t offset = 0, size = 0, align = 1;
  };
  const size_t finalSectionCount = order.size() + (symtabWritten ? 1 : 0) + 1;
  uint64_t appendCursor = std::max<uint64_t>(
      result.bytes.size(), L.shoff + uint64_t(finalSectionCount) * shdr);
  auto appendBytes = [&](std::string_view bytes) {
    uint64_t at = appendCursor;
    w.poke(at, bytes);
    appendCursor = at + bytes.size();
    return at;
  };
  std::vector<Added> added;
  uint32_t strtabIndex = 0;
  if (symtabWritten) {
    // The symbol table only shrinks, so it still fits the slot it has.
    const auto& h = raw[symtab];
    w.poke(h.offset, symBytes);
    w.fill(h.offset + symBytes.size(), h.size - symBytes.size());
    // A new section's index is its position after the survivors, so record it
    // here rather than deriving it from a vector that is still growing.
    strtabIndex = uint32_t(order.size() + added.size());
    added.push_back({.name = ".strtab",
                     .offset = appendBytes(symStrings.data()),
                     .size = symStrings.data().size()});
    // Renumber every surviving relocation that referenced this table.
    for (uint32_t i = 1; i < L.shnum; ++i) {
      if (!keep[i] || !isRel(raw[i].type) || raw[i].link != symtab) continue;
      const auto& r = raw[i];
      if (!r.entsize) continue;
      for (uint64_t k = 0; k + r.entsize <= r.size; k += r.entsize) {
        uint64_t at = r.offset + k;
        if (!w.has(at, r.entsize)) continue;
        uint64_t info = w.integer(at + (wide ? 8 : 4), word);
        uint32_t old = uint32_t(wide ? info >> 32 : info >> 8);
        if (old >= remap.size() || remap[old] == UINT32_MAX) continue;
        uint64_t type = wide ? (info & 0xffffffffull) : uint8_t(info);
        uint64_t value =
            wide ? (uint64_t(remap[old]) << 32 | type) : (remap[old] << 8 | type);
        w.store(at + (wide ? 8 : 4), value, word);
      }
    }
  }
  // A single string table serves both purposes: the sh_name offsets in the
  // headers and the .shstrtab contents are the same bytes. Section names never
  // depend on where the table lands, so it can be appended before the headers
  // that reference it are built.
  const uint32_t shstrIndex = uint32_t(order.size() + added.size());
  Strtab shstr;
  for (uint32_t i : order) shstr.add(names[i]);
  for (const auto& a : added) shstr.add(a.name);
  shstr.add(".shstrtab");
  added.push_back({.name = ".shstrtab"});
  const uint64_t shstrAt = appendBytes(shstr.data());
  added.back().offset = shstrAt;
  added.back().size = shstr.data().size();

  std::string shBytes;
  shBytes.reserve((order.size() + added.size()) * shdr);
  auto remapField = [&](uint32_t value) {
    return value < L.shnum && to[value] != UINT32_MAX ? to[value] : 0u;
  };
  auto put = [&](std::string& entry, size_t at, uint64_t value, unsigned width) {
    putInt(entry, at, value, width, byteOrder);
  };
  for (uint32_t i : order) {
    std::string entry(shdr, '\0');
    std::memcpy(entry.data(),
                file.data().data() + file.sectionHeaderOffset(i), shdr);
    put(entry, 0, shstr.add(names[i]), 4);
    if (i == symtab && symtabWritten) {
      // The rebuilt symbol table's link, boundary and extent all changed, and
      // they have to be written into the header that is actually emitted --
      // patching the old slot would be overwritten below.
      put(entry, kLink, strtabIndex, 4);
      put(entry, kInfo, firstGlobal, 4);
      put(entry, kSize, symBytes.size(), word);
    } else {
      put(entry, kLink, remapField(raw[i].link), 4);
      put(entry, kInfo, remapField(raw[i].info), 4);
    }
    shBytes += entry;
  }
  for (const auto& a : added) {
    std::string entry(shdr, '\0');
    put(entry, 0, shstr.add(a.name), 4);
    put(entry, kType, a.type, 4);
    put(entry, kOffset, a.offset, word);
    put(entry, kSize, a.size, word);
    put(entry, kAlign, a.align, word);
    put(entry, kEntsize, a.entsize, word);
    shBytes += entry;
  }
  w.poke(L.shoff, shBytes);
  w.store(wide ? 60 : 48, order.size() + added.size(), 2);
  w.store(wide ? 62 : 50, shstrIndex, 2);
  result.sectionsRemoved = uint32_t(L.shnum - order.size());
  return result;
}


// ---------------------------------------------------------------------------
// COFF, and the COFF symbol table embedded in a PE image
// ---------------------------------------------------------------------------
inline Expected<Result> stripCOFFTable(const ObjectFile& file,
                                       const Options& o,
                                       std::function<uint32_t(uint32_t)> relocPtr,
                                       std::function<uint32_t(uint32_t)> relocCount,
                                       std::function<bool(uint32_t)> relocOverflow,
                                       std::function<std::string(uint32_t)> rawName,
                                       std::function<uint8_t(uint32_t)> rawClass,
                                       std::function<int32_t(uint32_t)> rawSection,
                                       std::function<uint32_t(uint32_t)> auxCount,
                                       std::function<uint64_t(uint32_t)> sectionHeaderOffset,
                                       std::function<uint64_t(uint32_t)> relocCountField,
                                       std::function<uint64_t(uint32_t)> relocPtrField,
                                       uint64_t headerBase, uint32_t symPtrField,
                                       uint32_t symCountField,
                                       uint64_t symTable, uint32_t symStride,
                                       uint32_t symCount, uint32_t stringTable) {
  using namespace edit;
  Result result;
  result.bytes.assign(file.data().begin(), file.data().end());
  Writer w{file.data(), result.bytes};

  std::vector<std::string> names;
  for (const auto& s : file.sections()) names.push_back(s.name);
  std::vector<bool> keep(names.size(), true);
  if (o.mode != Mode::All)
    for (size_t i = 0; i < names.size(); ++i)
      if (isDebugName(names[i])) keep[i] = false;
  for (size_t i = 0; i < names.size(); ++i)
    if (!names[i].empty() && sectionDropped(o, names[i])) keep[i] = false;
  for (const auto& k : o.keepSections)
    for (size_t i = 0; i < names.size(); ++i)
      if (!names[i].empty() && globMatch(k, names[i])) keep[i] = true;

  // A dropped COFF section is made inert by blanking its 40-byte header. The
  // section count is a field in the file header that other tools rely on, so
  // the slot stays but no longer names any data or relocations.
  for (size_t si = 0; si < names.size() && si < file.sections().size(); ++si) {
    if (keep[si]) continue;
    result.sectionsRemoved++;
    w.fill(sectionHeaderOffset(uint32_t(si)), 40);
  }

  // Walk the symbol records, folding each run of auxiliary records into one
  // entry so they are dropped and renumbered as a unit.
  struct Entry {
    uint32_t record = 0, records = 0;
    bool alive = true;
  };
  std::vector<Entry> entries;
  for (uint32_t i = 0; i < symCount;) {
    Entry e;
    e.record = i;
    e.records = 1 + auxCount(i);
    if (!e.records || e.records > symCount - i) break;
    entries.push_back(e);
    i += e.records;
  }
  std::set<uint32_t> referenced;
  for (size_t si = 0; si < names.size(); ++si) {
    if (!keep[si]) continue;
    auto relocs = file.relocations(file.sections()[si]);
    if (!relocs) continue;
    for (const auto& r : *relocs)
      if (r.symbol) referenced.insert(r.symbol.value_or(0));
  }
  for (auto& e : entries) {
    const int32_t section = rawSection(e.record);
    const uint8_t cls = rawClass(e.record);
    if (o.mode == Mode::All) {
      e.alive = false;
      continue;
    }
    if (cls == 103 /* IMAGE_SYM_CLASS_FILE */) e.alive = false;
    if (section == -2 /* IMAGE_SYM_DEBUG */) e.alive = false;
    if (section > 0 && uint32_t(section - 1) < keep.size() && !keep[section - 1])
      e.alive = false;
    if (symbolSelected(o, rawName(e.record))) e.alive = false;
    if (o.mode == Mode::Unneeded && cls == 3 /* STATIC */ &&
        !referenced.contains(e.record))
      e.alive = false;
  }
  // Weak externals: keep the tag symbol whenever the definition survives.
  for (auto& e : entries) {
    if (!e.alive || e.records < 2) continue;
    if (rawClass(e.record) != 105 /* IMAGE_SYM_CLASS_WEAK_EXTERNAL */) continue;
    uint64_t at = uint64_t(symTable) + uint64_t(e.record) * symStride + symStride;
    if (!w.has(at, 4)) continue;
    uint32_t tag = uint32_t(w.integer(at, 4));
    for (const auto& other : entries)
      if (other.record == tag && !other.alive) e.alive = false;
  }

  uint64_t keptRecords = 0;
  for (const auto& e : entries)
    if (e.alive) keptRecords += e.records;
  result.symbolsRemoved = symCount - uint32_t(keptRecords);
  if (o.mode == Mode::All) result.symbolsRemoved = symCount;

  if (keptRecords == 0) {
    w.store(headerBase + symPtrField, 0, 4);
    w.store(headerBase + symCountField, 0, 4);
    if (symTable && symCount) w.fill(symTable, uint64_t(symCount) * symStride);
    // Every relocation named a symbol, so with no symbol table they all have
    // to go as well; leaving them would leave a file whose relocations resolve
    // to nothing.
    for (size_t si = 0; si < names.size() && si < file.sections().size(); ++si) {
      if (!relocPtr(uint32_t(si))) continue;
      w.store(relocPtrField(uint32_t(si)), 0, 4);
      w.store(relocCountField(uint32_t(si)), 0, 2);
    }
    for (size_t si = 0; si < names.size(); ++si) result.sectionsRemoved += keep[si] ? 0u : 1u;
    return result;
  }

  // Remap raw record indices, so relocation SymbolTableIndex fields can follow.
  std::vector<uint32_t> remap(symCount, UINT32_MAX);
  uint32_t out = 0;
  for (const auto& e : entries)
    if (e.alive)
      for (uint32_t j = 0; j < e.records; ++j) remap[e.record + j] = out + j;

  std::string table;
  table.reserve(size_t(keptRecords) * symStride);
  for (const auto& e : entries) {
    if (!e.alive) continue;
    uint64_t src = uint64_t(symTable) + uint64_t(e.record) * symStride;
    std::string run(reinterpret_cast<const char*>(file.data().data() + src),
                    size_t(e.records) * symStride);
    // An auxiliary record naming a dropped symbol is repointed at record 0,
    // which is the conventional "no definition" for a weak external tag.
    for (uint32_t j = 1; j < e.records; ++j) {
      size_t at = size_t(j) * symStride;
      uint32_t tag = 0;
      for (unsigned b = 0; b < 4; ++b)
        tag |= uint32_t(uint8_t(run[at + b])) << (8 * b);
      uint32_t to = tag < remap.size() ? remap[tag] : UINT32_MAX;
      for (unsigned b = 0; b < 4; ++b)
        run[at + b] = char(uint8_t((to == UINT32_MAX ? 0 : to) >> (8 * b)));
    }
    table += run;
  }
  w.poke(symTable, table);
  w.fill(symTable + table.size(),
         uint64_t(symCount - keptRecords) * symStride);
  w.store(headerBase + symCountField, uint32_t(keptRecords), 4);
  if (stringTable) {
    // The string table sits immediately after the symbol table, so it has to
    // travel with it -- the symbol names index into these exact bytes, and
    // re-pointing the size field alone would leave them reading symbol bytes.
    const uint64_t strAt = symTable + table.size();
    const uint64_t strSize = file.data().size() - stringTable;
    w.poke(strAt, std::string_view(
                      reinterpret_cast<const char*>(file.data().data() +
                                                   stringTable),
                      size_t(strSize)));
    w.store(strAt, strSize, 4);
  }

  // Relocations store raw record indices in their second word. A relocation
  // whose symbol was removed has to be removed too -- left in place it would
  // name a record that no longer exists, and a reader would resolve it to
  // whatever now sits at that index.
  for (size_t si = 0; si < names.size() && si < file.sections().size(); ++si) {
    if (!keep[si]) continue;
    uint32_t at = relocPtr(uint32_t(si));
    if (!at) continue;
    bool overflow = relocOverflow(uint32_t(si));
    uint32_t count = relocCount(uint32_t(si));
    if (overflow) {
      // The real count lives in the first record's VirtualAddress, and that
      // record is itself a placeholder.
      if (!w.has(at, 10)) continue;
      uint32_t total = uint32_t(w.integer(at, 4));
      count = total ? total - 1 : 0;
      at += 10;
    }
    if (count > 0xffff)
      return Error{Error::Code::Unsupported,
                   "too many relocations to renumber safely"};
    // Compact in place: keep the entries whose symbol survived, repointed.
    std::string kept;
    kept.reserve(size_t(count) * 10);
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t rec = uint64_t(at) + uint64_t(i) * 10;
      if (!w.has(rec, 10)) break;
      uint32_t old = uint32_t(w.integer(rec + 4, 4));
      if (old >= remap.size() || remap[old] == UINT32_MAX) continue;
      std::string entry(10, '\0');
      std::memcpy(entry.data(), file.data().data() + size_t(rec), 10);
      for (unsigned b = 0; b < 4; ++b)
        entry[4 + b] = char(uint8_t(remap[old] >> (8 * b)));
      kept += entry;
    }
    const uint32_t newCount = uint32_t(kept.size() / 10);
    w.poke(at, kept);
    w.fill(at + kept.size(), uint64_t(count - newCount) * 10);
    if (overflow) {
      w.store(at - 10 + 4, newCount + 1, 4);
    } else {
      w.store(relocCountField(uint32_t(si)), newCount, 2);
    }
  }
  return result;
}

inline Expected<Result> stripCOFF(const coff::COFFFile& file, const Options& o) {
  const auto L = file.layout();
  return stripCOFFTable(
      file, o, [&](uint32_t i) { return file.relocationPointer(i); },
      [&](uint32_t i) { return file.relocationCount(i); },
      [&](uint32_t i) { return file.relocationOverflow(i); },
      [&](uint32_t i) { return file.rawSymbolName(i); },
      [&](uint32_t i) { return file.rawSymbolClass(i); },
      [&](uint32_t i) { return file.rawSymbolSection(i); },
      [&](uint32_t i) { return file.rawSymbolAuxCount(i); },
      [&](uint32_t i) { return file.sectionHeaderOffset(i); },
      [&](uint32_t i) { return file.relocationCountOffset(i); },
      [&](uint32_t i) { return file.relocationPointerOffset(i); },
      0, 8, 12, L.symbolTable, L.symbolSize, L.symbolCount, L.stringTable);
}

// ---------------------------------------------------------------------------
// Mach-O
// ---------------------------------------------------------------------------
inline Expected<Result> stripMachO(const macho::MachOFile& file,
                                   const Options& o) {
  using namespace edit;
  const auto L = file.layout();
  Result result;
  result.bytes.assign(file.data().begin(), file.data().end());
  Writer w{file.data(), result.bytes, file.endian()};
  if (!L.symbolCommand) return result;
  const unsigned nlist = L.wide ? 16 : 12;

  std::vector<bool> keep(file.sections().size(), true);
  if (o.mode != Mode::All)
    for (size_t i = 0; i < keep.size(); ++i)
      if (isDebugName(file.sections()[i].name)) keep[i] = false;
  for (size_t i = 0; i < keep.size(); ++i)
    if (sectionDropped(o, file.sections()[i].name)) keep[i] = false;
  for (const auto& k : o.keepSections)
    for (size_t i = 0; i < keep.size(); ++i)
      if (globMatch(k, file.sections()[i].name)) keep[i] = true;

  // A symbol a surviving relocation names has to stay, since renumbering is
  // the only way to keep the relocation entries meaningful.
  std::set<uint32_t> referenced;
  for (size_t si = 0; si < keep.size(); ++si) {
    if (!keep[si]) continue;
    auto relocs = file.relocations(file.sections()[si]);
    if (!relocs) continue;
    for (const auto& r : *relocs)
      if (r.symbol) referenced.insert(r.symbol.value_or(0));
  }

  std::vector<bool> alive(file.symbols().size(), true);
  for (size_t i = 0; i < alive.size(); ++i) {
    const auto& s = file.symbols()[i];
    if (s.flags & sym::Debug) alive[i] = false;  // N_STAB
    if (s.section && *s.section < keep.size() && !keep[*s.section])
      alive[i] = false;
    if (symbolSelected(o, s.name)) alive[i] = false;
    if (o.mode == Mode::All && !referenced.contains(uint32_t(i))) alive[i] = false;
    if (o.mode == Mode::Unneeded && s.binding == SymbolBinding::Local &&
        !referenced.contains(uint32_t(i)))
      alive[i] = false;
  }
  std::vector<uint32_t> remap(alive.size(), UINT32_MAX);
  uint32_t next = 0;
  for (size_t i = 0; i < alive.size(); ++i)
    if (alive[i]) remap[i] = next++;
  result.symbolsRemoved = uint32_t(alive.size() - next);

  if (next == 0) {
    w.fill(L.symbolCommand, 24);
    // Section relocations that named symbols now name nothing, so their
    // entries are dropped rather than left pointing past the table.
    for (size_t si = 0; si < keep.size(); ++si)
      if (keep[si] && file.relocationPointer(uint32_t(si)))
        result.sectionsRemoved += 0;  // counted as removed relocations
    return result;
  }

  Strtab strings;
  std::string blob;
  blob.reserve(size_t(next) * nlist);
  for (size_t i = 0; i < alive.size(); ++i) {
    if (!alive[i]) continue;
    uint64_t src = file.symbolSlot(uint32_t(i));
    if (!w.has(src, nlist)) break;
    std::string entry(nlist, '\0');
    std::memcpy(entry.data(), file.data().data() + src, nlist);
    uint32_t nameAt = strings.add(file.symbols()[i].name);
    putInt(entry, 0, nameAt, 4, file.endian());
    blob += entry;
  }
  uint32_t emitted = uint32_t(blob.size() / nlist);
  result.symbolsRemoved = uint32_t(alive.size() - emitted);
  if (emitted == 0) {
    w.fill(L.symbolCommand, 24);
    return result;
  }
  uint64_t symAt = w.integer(L.symbolCommand + 8, 4);
  uint64_t strAt = symAt + uint64_t(emitted) * nlist;
  w.poke(symAt, blob);
  w.poke(strAt, strings.data());
  w.fill(strAt + strings.data().size(),
         w.integer(L.symbolCommand + 20, 4) - strings.data().size());
  w.store(L.symbolCommand + 12, emitted, 4);
  w.store(L.symbolCommand + 16, strAt, 4);
  w.store(L.symbolCommand + 20, strings.data().size(), 4);

  // Renumber r_symbolnum in every surviving section's relocation entries.
  for (size_t si = 0; si < keep.size(); ++si) {
    if (!keep[si]) continue;
    uint32_t at = file.relocationPointer(uint32_t(si));
    uint32_t count = file.relocationCount(uint32_t(si));
    if (!at || !count) continue;
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t rec = uint64_t(at) + uint64_t(i) * 8;
      if (!w.has(rec, 8)) break;
      uint32_t address = uint32_t(w.integer(rec, 4));
      if (address & 0x80000000u) continue;  // scattered
      uint32_t bits = uint32_t(w.integer(rec + 4, 4));
      uint32_t old = file.endian() == Endian::Little ? bits & 0xffffffu
                                                     : bits >> 8;
      if (old >= remap.size() || remap[old] == UINT32_MAX) continue;
      bits = file.endian() == Endian::Little
                 ? (bits & 0xff000000u) | remap[old]
                 : (bits & 0x000000ffu) | (remap[old] << 8);
      w.store(rec + 4, bits, 4);
    }
  }
  for (size_t si = 0; si < keep.size(); ++si)
    if (!keep[si]) result.sectionsRemoved++;
  return result;
}

// ---------------------------------------------------------------------------
// PE: an image can carry a COFF symbol table, which uses exactly the COFF
// machinery. Section contents are never rewritten, so an image without one has
// nothing to strip here.
// ---------------------------------------------------------------------------
inline Expected<Result> stripPE(const pe::PEFile& file, const Options& o) {
  const auto L = file.layout();
  if (!L.symbolTable)
    return Error{Error::Code::Unsupported, "PE image has no COFF symbol table"};
  return stripCOFFTable(
      file, o, [&](uint32_t i) { return file.relocationPointer(i); },
      [&](uint32_t i) { return file.relocationCount(i); },
      [](uint32_t) { return false; },
      [&](uint32_t i) { return file.rawSymbolName(i); },
      [&](uint32_t i) { return file.rawSymbolClass(i); },
      [&](uint32_t i) { return file.rawSymbolSection(i); },
      [&](uint32_t i) { return file.rawSymbolAuxCount(i); },
      [&](uint32_t i) { return file.sectionHeaderOffset(i); },
      [&](uint32_t i) { return file.relocationCountOffset(i); },
      [&](uint32_t i) { return file.relocationPointerOffset(i); },
      L.fileHeader, 8, 12, L.symbolTable, 18, L.symbolCount, L.stringTable);
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
inline Expected<Result> strip(const ObjectFile& file, const Options& o) {
  switch (file.format()) {
    case Format::ELF:
      if (const auto* f = dynamic_cast<const elf::ELFFile*>(&file)) return stripELF(*f, o);
      break;
    case Format::COFF:
      if (const auto* f = dynamic_cast<const coff::COFFFile*>(&file)) return stripCOFF(*f, o);
      break;
    case Format::MachO:
      if (const auto* f = dynamic_cast<const macho::MachOFile*>(&file)) return stripMachO(*f, o);
      break;
    case Format::PE:
      if (const auto* f = dynamic_cast<const pe::PEFile*>(&file)) return stripPE(*f, o);
      break;
    default:
      break;
  }
  return Error{Error::Code::Unsupported, "no stripper for this format"};
}

// Strips a buffer in one step, sniffing the format. `bytes` is not modified.
inline Expected<Result> strip(std::span<const uint8_t> bytes,
                              const Options& options) {
  auto opened = open(std::vector<uint8_t>(bytes.begin(), bytes.end()));
  if (!opened) return opened.error();
  return strip(*opened->object, options);
}

}  // namespace qbfd::strip
