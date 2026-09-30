// ELF32/ELF64 reader. No dependency on host elf.h or host byte order.
#pragma once
#include <unordered_map>

#include "qBFDCore.hpp"

namespace qbfd::elf {
namespace k {
inline constexpr uint32_t Null = 0, Progbits = 1, Symtab = 2, Strtab = 3,
                          Rela = 4, Dynamic = 6, Note = 7, Nobits = 8, Rel = 9,
                          Dynsym = 11;
}
inline Arch archFromMachine(uint16_t machine, bool wide) {
  switch (machine) {
    case 3:
      return Arch::X86;
    case 62:
      return Arch::X86_64;
    case 40:
      return Arch::ARM;
    case 183:
      return Arch::AArch64;
    case 243:
      return wide ? Arch::RISCV64 : Arch::RISCV32;
    case 20:
      return Arch::PowerPC;
    case 21:
      return Arch::PowerPC64;
    case 8:
      return wide ? Arch::MIPS64 : Arch::MIPS;
    case 2:
      return Arch::SPARC;
    case 43:
      return Arch::SPARC64;
    case 22:
      return Arch::S390X;
    case 258:
      return Arch::LoongArch64;
    default:
      return Arch::Unknown;
  }
}
class ELFFile final : public ObjectFile {
 public:
  ELFFile(std::span<const uint8_t> bytes, std::filesystem::path path)
      : ObjectFile(bytes, std::move(path)), r_{bytes} {}
  Format format() const override { return Format::ELF; }
  std::string_view targetName() const override { return target_; }
  Arch arch() const override { return archFromMachine(machine_, wide_); }
  Endian endian() const override { return r_.order; }
  bool is64Bit() const override { return wide_; }
  FileType fileType() const override {
    switch (type_) {
      case 1:
        return FileType::Relocatable;
      case 2:
        return FileType::Executable;
      case 3:
        return FileType::SharedObject;
      case 4:
        return FileType::Core;
      default:
        return FileType::Unknown;
    }
  }
  uint64_t entryPoint() const override { return entry_; }
  uint64_t imageBase() const override { return base_; }
  uint16_t machine() const { return machine_; }
  uint8_t osABI() const { return r_.u8(7); }
  uint32_t flags() const { return flags_; }
  const std::vector<Section>& sections() const override { return sections_; }
  const std::vector<Symbol>& symbols() const override { return symbols_; }
  const std::vector<Symbol>& dynamicSymbols() const override {
    return dynamic_;
  }
  const std::vector<Segment>& segments() const override { return segments_; }
  std::vector<std::string> neededLibraries() const override { return needed_; }
  Expected<std::span<const uint8_t>> sectionContents(
      const Section& section) const override {
    if (section.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "invalid ELF section"};
    const auto& s = sections_[section.index];
    if (s.flags & sec::Bss) return std::span<const uint8_t>{};
    return data_.subspan(s.fileOffset, s.fileSize);
  }
  Expected<std::vector<Relocation>> relocations(
      const Section& s) const override {
    if (s.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "invalid ELF section"};
    return relocs_[s.index];
  }
  static bool sniff(std::span<const uint8_t> d) {
    return d.size() >= 4 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' &&
           d[3] == 'F';
  }
  // On-disk table geometry. Exposed for tools that rewrite the file in place
  // (qStrip.hpp) so the layout is stated once, here, rather than re-derived.
  struct Layout {
    uint64_t ehsize = 0, phoff = 0, shoff = 0;
    uint16_t phentsize = 0, phnum = 0, shentsize = 0, shnum = 0, shstrndx = 0;
    bool wide = false;
  };
  Layout layout() const { return layout_; }
  uint16_t eType() const { return type_; }

  // A section header exactly as it appears on disk, before any interpretation.
  struct RawSection {
    uint32_t name = 0, type = 0, link = 0, info = 0;
    uint64_t flags = 0, addr = 0, offset = 0, size = 0, align = 0, entsize = 0;
  };
  using Raw = RawSection;


  // The parsed section headers, in `Section::index` order. Exposed so a
  // rewriter (qStrip.hpp) does not have to re-derive the on-disk layout.
  const std::vector<RawSection>& rawSections() const { return raw_; }
  // File offset of a section header, for patching it in place.
  uint64_t sectionHeaderOffset(uint32_t index) const {
    return layout_.shoff + uint64_t(index) * layout_.shentsize;
  }
  static Expected<std::unique_ptr<ObjectFile>> open(
      std::span<const uint8_t> d, const std::filesystem::path& path) {
    return detail::parse<ELFFile>(d, path);
  }
  Error parse() {
    if (!sniff(data_)) return {Error::Code::Malformed, "missing ELF magic"};
    r_.require(0, 16);
    if ((r_.u8(4) != 1 && r_.u8(4) != 2) || (r_.u8(5) != 1 && r_.u8(5) != 2))
      return {Error::Code::Unsupported, "unsupported ELF class or byte order"};
    wide_ = r_.u8(4) == 2;
    r_.order = r_.u8(5) == 1 ? Endian::Little : Endian::Big;
    r_.require(0, wide_ ? 64 : 52);
    if (r_.u8(6) != 1 || r_.u32(20) != 1)
      detail::malformed("invalid ELF version");
    type_ = r_.u16(16);
    machine_ = r_.u16(18);
    target_ = wide_ ? "elf64" : "elf32";
    target_ += r_.order == Endian::Little ? "-little" : "-big";
    entry_ = word(24);
    uint64_t phoff = word(wide_ ? 32 : 28), shoff = word(wide_ ? 40 : 32);
    flags_ = r_.u32(wide_ ? 48 : 36);
    uint64_t tail = wide_ ? 52 : 40;
    if (r_.u16(tail) < (wide_ ? 64 : 52))
      detail::malformed("invalid ELF header size");
    uint16_t phent = r_.u16(tail + 2), shent = r_.u16(tail + 6);
    uint64_t phnum = r_.u16(tail + 4), shnum = r_.u16(tail + 8),
             names = r_.u16(tail + 10);
    layout_.ehsize = wide_ ? 64 : 52;
    layout_.phoff = phoff;
    layout_.shoff = shoff;
    layout_.phentsize = phent;
    layout_.phnum = uint16_t(phnum);
    layout_.shentsize = shent;
    layout_.shnum = uint16_t(shnum);
    layout_.shstrndx = uint16_t(names);
    layout_.wide = wide_;
    if (shoff) {
      if (shent < (wide_ ? 64 : 40))
        detail::malformed("short ELF section entry");
      r_.require(shoff, shent);
      if (!shnum) shnum = word(shoff + (wide_ ? 32 : 20));
      if (names == 0xffff) names = r_.u32(shoff + (wide_ ? 40 : 24));
      if (phnum == 0xffff) phnum = r_.u32(shoff + (wide_ ? 44 : 28));
    } else if (shnum || names || phnum == 0xffff)
      detail::malformed("missing ELF section table");
    if (shnum > UINT32_MAX ||
        (shnum && (!shent || shnum > (data_.size() - shoff) / shent)))
      detail::malformed("truncated ELF section table");
    for (uint64_t i = 0; i < shnum; ++i) {
      uint64_t o = shoff + i * shent;
      Raw h;
      h.name = r_.u32(o);
      h.type = r_.u32(o + 4);
      h.flags = word(o + 8);
      h.addr = word(o + (wide_ ? 16 : 12));
      h.offset = word(o + (wide_ ? 24 : 16));
      h.size = word(o + (wide_ ? 32 : 20));
      h.link = r_.u32(o + (wide_ ? 40 : 24));
      h.info = r_.u32(o + (wide_ ? 44 : 28));
      h.align = word(o + (wide_ ? 48 : 32));
      h.entsize = word(o + (wide_ ? 56 : 36));
      if (h.align && (h.align & (h.align - 1)))
        detail::malformed("invalid ELF alignment");
      if (h.type != k::Nobits && h.type != k::Null)
        r_.require(h.offset, h.size);
      raw_.push_back(h);
    }
    if (names && (names >= raw_.size() || raw_[names].type != k::Strtab))
      detail::malformed("invalid ELF section name table");
    for (uint32_t i = 0; i < raw_.size(); ++i) {
      const auto& h = raw_[i];
      Section s;
      s.index = i;
      if (names) s.name = string(raw_[names], h.name);
      s.nativeType = h.type;
      s.nativeFlags = h.flags;
      s.vma = s.lma = h.addr;
      s.size = h.type == k::Null ? 0 : h.size;
      s.fileOffset = h.type == k::Null ? 0 : h.offset;
      s.fileSize = (h.type == k::Nobits || h.type == k::Null) ? 0 : h.size;
      s.alignment = h.align ? h.align : 1;
      if (h.flags & 2) s.flags |= sec::Alloc;
      if (h.flags & 1)
        s.flags |= sec::Writable;
      else
        s.flags |= sec::ReadOnly;
      if (h.flags & 4)
        s.flags |= sec::Code | sec::Exec;
      else if (h.flags & 2)
        s.flags |= sec::Data;
      if (h.type == k::Nobits)
        s.flags |= sec::Bss;
      else if (h.type != k::Null)
        s.flags |= sec::Load;
      if (h.flags & 0x400) s.flags |= sec::Tls;
      if (h.flags & 0x20 || h.type == k::Strtab) s.flags |= sec::Strings;
      if (h.type == k::Rel || h.type == k::Rela) s.flags |= sec::Reloc;
      if (h.type == k::Note) s.flags |= sec::Note;
      if (s.name.starts_with(".debug") || s.name.starts_with(".zdebug"))
        s.flags |= sec::Debug;
      sections_.push_back(std::move(s));
    }
    if (phnum) {
      if (!phoff || phent < (wide_ ? 56 : 32))
        detail::malformed("invalid ELF program table");
      r_.require(phoff, phnum * phent);
    }
    bool baseSet = false;
    for (uint64_t i = 0; i < phnum; ++i) {
      uint64_t o = phoff + i * phent;
      Segment g;
      g.nativeType = r_.u32(o);
      uint32_t f = r_.u32(o + (wide_ ? 4 : 24));
      g.fileOffset = word(o + (wide_ ? 8 : 4));
      g.vaddr = word(o + (wide_ ? 16 : 8));
      g.paddr = word(o + (wide_ ? 24 : 12));
      g.fileSize = word(o + (wide_ ? 32 : 16));
      g.memSize = word(o + (wide_ ? 40 : 20));
      g.alignment = word(o + (wide_ ? 48 : 28));
      r_.require(g.fileOffset, g.fileSize);
      if (g.nativeType == 1 && g.fileSize > g.memSize)
        detail::malformed("ELF load segment exceeds memory size");
      if (f & 4) g.flags |= seg::Read;
      if (f & 2) g.flags |= seg::Write;
      if (f & 1) g.flags |= seg::Exec;
      for (const auto& s : sections_)
        if ((s.flags & sec::Alloc) && s.vma >= g.vaddr &&
            s.vma - g.vaddr <= g.memSize &&
            s.size <= g.memSize - (s.vma - g.vaddr))
          g.sections.push_back(s.index);
      if (g.nativeType == 1 && (!baseSet || g.vaddr < base_)) {
        base_ = g.vaddr;
        baseSet = true;
      }
      segments_.push_back(std::move(g));
    }
    for (uint32_t i = 0; i < raw_.size(); ++i)
      if (raw_[i].type == k::Symtab || raw_[i].type == k::Dynsym)
        parseSymbols(i);
    relocs_.resize(sections_.size());
    for (const auto& h : raw_)
      if (h.type == k::Rel || h.type == k::Rela) parseRelocations(h);
    bool dynamicSection = false;
    for (const auto& h : raw_)
      if (h.type == k::Dynamic) {
        parseDynamic(h);
        dynamicSection = true;
      }
    if (!dynamicSection) parseSegmentDependencies();
    return {};
  }

 private:
  uint64_t word(uint64_t o) const { return wide_ ? r_.u64(o) : r_.u32(o); }
  std::string string(const Raw& h, uint64_t index) const {
    return r_.stringAt(h.offset, h.size, index);
  }
  uint64_t entries(const Raw& h, uint64_t minimum) const {
    if (h.entsize < minimum || h.size % h.entsize)
      detail::malformed("invalid ELF table stride");
    return h.size / h.entsize;
  }
  void parseSymbols(uint32_t table) {
    const auto& h = raw_[table];
    if (h.link >= raw_.size() || raw_[h.link].type != k::Strtab)
      detail::malformed("invalid ELF symbol string table");
    uint64_t count = entries(h, wide_ ? 24 : 16);
    auto& out = h.type == k::Dynsym ? dynamic_ : symbols_;
    if (count > UINT32_MAX - out.size())
      detail::malformed("too many ELF symbols");
    symbolBases_[table] = uint32_t(out.size());
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t o = h.offset + i * h.entsize;
      Symbol s;
      s.index = uint32_t(out.size());
      s.name = string(raw_[h.link], r_.u32(o));
      uint8_t info = r_.u8(o + (wide_ ? 4 : 12)),
              other = r_.u8(o + (wide_ ? 5 : 13));
      uint32_t section = r_.u16(o + (wide_ ? 6 : 14));
      if (section == 0xffff) {
        bool found = false;
        for (const auto& x : raw_)
          if (x.type == 18 && x.link == table) {
            if (entries(x, 4) != count)
              detail::malformed("invalid extended symbol indices");
            section = r_.u32(x.offset + i * x.entsize);
            found = true;
            break;
          }
        if (!found) detail::malformed("missing extended symbol indices");
      }
      s.value = word(o + (wide_ ? 8 : 4));
      s.size = word(o + (wide_ ? 16 : 8));
      s.nativeType = info;
      switch (info >> 4) {
        case 1:
          s.binding = SymbolBinding::Global;
          break;
        case 2:
          s.binding = SymbolBinding::Weak;
          break;
        case 10:
          s.binding = SymbolBinding::Unique;
          break;
      }
      switch (info & 15) {
        case 1:
          s.kind = SymbolKind::Object;
          break;
        case 2:
          s.kind = SymbolKind::Function;
          break;
        case 3:
          s.kind = SymbolKind::Section;
          break;
        case 4:
          s.kind = SymbolKind::File;
          break;
        case 5:
          s.kind = SymbolKind::Common;
          break;
        case 6:
          s.kind = SymbolKind::Tls;
          break;
        case 10:
          s.kind = SymbolKind::Indirect;
          break;
      }
      if (!section)
        s.flags |= sym::Undefined;
      else if (section == 0xfff1)
        s.flags |= sym::Absolute;
      else if (section == 0xfff2)
        s.kind = SymbolKind::Common;
      else if (section < sections_.size())
        s.section = section;
      else
        detail::malformed("invalid ELF symbol section");
      if ((other & 3) == 1 || (other & 3) == 2) s.flags |= sym::Hidden;
      if (h.type == k::Dynsym && s.binding != SymbolBinding::Local)
        s.flags |= section ? sym::Exported : sym::Imported;
      out.push_back(std::move(s));
    }
  }
  void parseRelocations(const Raw& h) {
    bool rela = h.type == k::Rela;
    auto it = symbolBases_.find(h.link);
    if (it == symbolBases_.end() || h.info >= sections_.size())
      detail::malformed("invalid ELF relocation links");
    const auto& table = raw_[h.link];
    uint64_t symbolCount = table.size / table.entsize;
    uint64_t count = entries(h, wide_ ? (rela ? 24 : 16) : (rela ? 12 : 8));
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t o = h.offset + i * h.entsize, address = word(o),
               info = word(o + (wide_ ? 8 : 4));
      // MIPS64EL stores a little-endian symbol word followed by big-endian type
      // bytes.
      if (wide_ && machine_ == 8 && r_.order == Endian::Little) {
        detail::Reader typeReader{data_, Endian::Big};
        info = (uint64_t(r_.u32(o + 8)) << 32) | typeReader.u32(o + 12);
      }
      uint64_t symbol = wide_ ? info >> 32 : info >> 8;
      if (symbol >= symbolCount)
        detail::malformed("invalid ELF relocation symbol");
      const Section* target =
          h.info ? &sections_[h.info] : sectionForAddress(address);
      if (!target)
        continue;  // Dynamic relocation outside any section (e.g. stripped
                   // section table).
      Relocation r;
      r.nativeType = wide_ ? uint32_t(info) : uint8_t(info);
      r.symbol = it->second + uint32_t(symbol);
      r.dynamicSymbol = table.type == k::Dynsym;
      if (type_ != 1 && address < target->vma)
        detail::malformed("ELF relocation precedes section");
      r.offset = type_ == 1 ? address : address - target->vma;
      if (r.offset > target->size)
        detail::malformed("ELF relocation outside section");
      r.typeName =
          "R_" + std::to_string(machine_) + "_" + std::to_string(r.nativeType);
      if (machine_ == 62) {
        switch (r.nativeType) {
          case 1:
            r.typeName = "R_X86_64_64";
            r.bitSize = 64;
            break;
          case 2:
            r.typeName = "R_X86_64_PC32";
            r.bitSize = 32;
            r.pcRelative = true;
            break;
          case 4:
            r.typeName = "R_X86_64_PLT32";
            r.bitSize = 32;
            r.pcRelative = true;
            break;
          case 8:
            r.typeName = "R_X86_64_RELATIVE";
            r.bitSize = 64;
            break;
          case 10:
            r.typeName = "R_X86_64_32";
            r.bitSize = 32;
            break;
          case 11:
            r.typeName = "R_X86_64_32S";
            r.bitSize = 32;
            break;
        }
      } else if (machine_ == 3 && (r.nativeType == 1 || r.nativeType == 2)) {
        r.typeName = r.nativeType == 1 ? "R_386_32" : "R_386_PC32";
        r.bitSize = 32;
        r.pcRelative = r.nativeType == 2;
      } else if (machine_ == 183) {  // AArch64
        switch (r.nativeType) {
          case 257: r.typeName = "R_AARCH64_ABS64"; r.bitSize = 64; break;
          case 258: r.typeName = "R_AARCH64_ABS32"; r.bitSize = 32; break;
          case 259: r.typeName = "R_AARCH64_ABS16"; r.bitSize = 16; break;
          case 260: r.typeName = "R_AARCH64_PREL64"; r.bitSize = 64; r.pcRelative = true; break;
          case 261: r.typeName = "R_AARCH64_PREL32"; r.bitSize = 32; r.pcRelative = true; break;
          case 262: r.typeName = "R_AARCH64_PREL16"; r.bitSize = 16; r.pcRelative = true; break;
          case 275: r.typeName = "R_AARCH64_ADR_PREL_PG_HI21"; break;
          case 277: r.typeName = "R_AARCH64_ADD_ABS_LO12_NC"; break;
          case 282: r.typeName = "R_AARCH64_JUMP26"; r.bitSize = 26; r.pcRelative = true; break;
          case 283: r.typeName = "R_AARCH64_CALL26"; r.bitSize = 26; r.pcRelative = true; break;
        }
      } else if (machine_ == 243) {  // RISC-V
        switch (r.nativeType) {
          case 1: r.typeName = "R_RISCV_32"; r.bitSize = 32; break;
          case 2: r.typeName = "R_RISCV_64"; r.bitSize = 64; break;
          case 3: r.typeName = "R_RISCV_RELATIVE"; r.bitSize = wide_ ? 64 : 32; break;
          case 16: r.typeName = "R_RISCV_JAL"; r.bitSize = 21; r.pcRelative = true; break;
          case 17: r.typeName = "R_RISCV_CALL"; break;
          case 18: r.typeName = "R_RISCV_CALL_PLT"; break;
          case 20: r.typeName = "R_RISCV_HI20"; break;
          case 21: r.typeName = "R_RISCV_LO12_I"; break;
          case 22: r.typeName = "R_RISCV_LO12_S"; break;
          case 23: r.typeName = "R_RISCV_PCREL_HI20"; break;
          case 24: r.typeName = "R_RISCV_PCREL_LO12_I"; break;
          case 25: r.typeName = "R_RISCV_PCREL_LO12_S"; break;
          case 26: r.typeName = "R_RISCV_32_PCREL"; r.bitSize = 32; r.pcRelative = true; break;
          case 51: r.typeName = "R_RISCV_RELAX"; break;
        }
      } else if (machine_ == 40) {  // ARM
        switch (r.nativeType) {
          case 2: r.typeName = "R_ARM_ABS32"; r.bitSize = 32; break;
          case 3: r.typeName = "R_ARM_REL32"; r.bitSize = 32; r.pcRelative = true; break;
          case 10: r.typeName = "R_ARM_THM_CALL"; break;
          case 28: r.typeName = "R_ARM_CALL"; break;
          case 29: r.typeName = "R_ARM_JUMP24"; break;
        }
      } else if (machine_ == 20 || machine_ == 21) {  // PowerPC
        switch (r.nativeType) {
          case 1: r.typeName = "R_PPC_ADDR32"; r.bitSize = 32; break;
          case 26: r.typeName = "R_PPC_REL32"; r.bitSize = 32; r.pcRelative = true; break;
          case 38: r.typeName = "R_PPC64_ADDR64"; r.bitSize = 64; break;
          case 44: r.typeName = "R_PPC64_REL64"; r.bitSize = 64; r.pcRelative = true; break;
        }
      }
      if (rela) {
        r.addend = wide_ ? std::bit_cast<int64_t>(r_.u64(o + 16))
                         : int64_t(std::bit_cast<int32_t>(r_.u32(o + 8)));
        r.hasAddend = true;
      } else if (r.bitSize && r.offset <= target->fileSize &&
                 r.bitSize / 8 <= target->fileSize - r.offset) {
        uint64_t value =
            r_.integer(target->fileOffset + r.offset, r.bitSize / 8);
        if (r.bitSize < 64 && (value & (uint64_t(1) << (r.bitSize - 1))))
          value |= UINT64_MAX << r.bitSize;
        r.addend = std::bit_cast<int64_t>(value);
        r.hasAddend = true;
      }
      relocs_[target->index].push_back(std::move(r));
    }
  }
  void parseDynamic(const Raw& h) {
    if (h.link >= raw_.size() || raw_[h.link].type != k::Strtab)
      detail::malformed("invalid dynamic string table");
    uint64_t count = entries(h, wide_ ? 16 : 8);
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t o = h.offset + i * h.entsize, tag = word(o);
      if (!tag) return;
      if (tag == 1)
        needed_.push_back(string(raw_[h.link], word(o + (wide_ ? 8 : 4))));
    }
    detail::malformed("unterminated ELF dynamic table");
  }
  // Section headers are optional in executables. Resolve DT_STRTAB through
  // PT_LOAD.
  void parseSegmentDependencies() {
    for (const auto& segment : segments_)
      if (segment.nativeType == 2) {
        uint64_t stride = wide_ ? 16 : 8;
        if (segment.fileSize % stride)
          detail::malformed("invalid PT_DYNAMIC size");
        uint64_t strings = 0, size = 0;
        bool terminated = false;
        std::vector<uint64_t> offsets;
        for (uint64_t i = 0; i < segment.fileSize; i += stride) {
          uint64_t o = segment.fileOffset + i, tag = word(o),
                   value = word(o + stride / 2);
          if (!tag) {
            terminated = true;
            break;
          }
          if (tag == 1) offsets.push_back(value);
          if (tag == 5) strings = value;
          if (tag == 10) size = value;
        }
        if (!terminated) detail::malformed("unterminated PT_DYNAMIC");
        if (offsets.empty()) continue;
        std::optional<uint64_t> fileOffset;
        for (const auto& load : segments_)
          if (load.nativeType == 1 && strings >= load.vaddr &&
              strings - load.vaddr <= load.fileSize &&
              size <= load.fileSize - (strings - load.vaddr)) {
            fileOffset = load.fileOffset + (strings - load.vaddr);
            break;
          }
        if (!fileOffset) detail::malformed("unmapped dynamic string table");
        for (auto offset : offsets)
          needed_.push_back(r_.stringAt(*fileOffset, size, offset));
      }
  }
  detail::Reader r_;
  bool wide_ = false;
  uint16_t machine_ = 0, type_ = 0;
  uint32_t flags_ = 0;
  uint64_t entry_ = 0, base_ = 0;
  Layout layout_;
  std::string target_;
  std::vector<RawSection> raw_;
  std::vector<Section> sections_;
  std::vector<Symbol> symbols_, dynamic_;
  std::vector<Segment> segments_;
  std::vector<std::string> needed_;
  std::unordered_map<uint32_t, uint32_t> symbolBases_;
  std::vector<std::vector<Relocation>> relocs_;
};
inline void registerTargets(Registry& registry) {
  for (bool wide : {false, true})
    for (bool big : {false, true}) {
      std::string name = wide ? "elf64" : "elf32";
      name += big ? "-big" : "-little";
      registry.add({name, Format::ELF,
                    [wide, big](auto d) {
                      return ELFFile::sniff(d) &&
                             ((d.size() < 6 && !wide && !big) ||
                              (d.size() >= 6 && d[4] == (wide ? 2 : 1) &&
                               d[5] == (big ? 2 : 1)));
                    },
                    ELFFile::open});
    }
}
namespace query {
using namespace qbfd::query;
inline auto type(uint32_t value) { return nativeType(value); }
inline auto allocated() { return flags(sec::Alloc); }
}  // namespace query
}  // namespace qbfd::elf
