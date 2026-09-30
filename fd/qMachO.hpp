// Thin Mach-O reader and explicit universal-binary slice selection.
#pragma once
#include "qBFDCore.hpp"

namespace qbfd::macho {
inline Arch archFromCpu(uint32_t cpu) {
  switch (cpu) {
    case 7:
      return Arch::X86;
    case 0x1000007:
      return Arch::X86_64;
    case 12:
      return Arch::ARM;
    case 0x100000c:
      return Arch::AArch64;
    case 18:
      return Arch::PowerPC;
    case 0x1000012:
      return Arch::PowerPC64;
    default:
      return Arch::Unknown;
  }
}
struct FatSlice {
  uint32_t cpuType = 0, cpuSubtype = 0;
  uint64_t offset = 0, size = 0;
  uint32_t alignmentPower = 0;
};
inline bool isFat(std::span<const uint8_t> d) {
  if (d.size() < 4) return false;
  auto m = detail::Reader{d}.u32(0);
  return m == 0xcafebabe || m == 0xbebafeca || m == 0xcafebabf ||
         m == 0xbfbafeca;
}
inline Expected<std::vector<FatSlice>> slices(std::span<const uint8_t> bytes) {
  try {
    if (!isFat(bytes))
      return Error{Error::Code::UnknownFormat, "not a universal Mach-O"};
    detail::Reader r{bytes};
    uint32_t magic = r.u32(0);
    bool wide = magic == 0xcafebabf || magic == 0xbfbafeca;
    r.order = (magic == 0xbebafeca || magic == 0xbfbafeca) ? Endian::Big
                                                           : Endian::Little;
    uint64_t count = r.u32(4), stride = wide ? 32 : 20;
    r.require(8, count * stride);
    std::vector<FatSlice> result;
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t o = 8 + i * stride;
      FatSlice s;
      s.cpuType = r.u32(o);
      s.cpuSubtype = r.u32(o + 4);
      s.offset = wide ? r.u64(o + 8) : r.u32(o + 8);
      s.size = wide ? r.u64(o + 16) : r.u32(o + 12);
      s.alignmentPower = r.u32(o + (wide ? 24 : 16));
      if (s.alignmentPower > 63 ||
          s.offset % (uint64_t(1) << s.alignmentPower) ||
          s.offset < 8 + count * stride || !s.size)
        detail::malformed("invalid universal slice");
      r.require(s.offset, s.size);
      for (const auto& previous : result)
        if (s.offset < previous.offset + previous.size &&
            previous.offset < s.offset + s.size)
          detail::malformed("overlapping universal slices");
      result.push_back(s);
    }
    return result;
  } catch (const detail::ParseFailure& e) {
    return e.error;
  }
}
class MachOFile final : public ObjectFile {
 public:
  MachOFile(std::span<const uint8_t> d, std::filesystem::path path)
      : ObjectFile(d, std::move(path)), r_{d} {}
  Format format() const override { return Format::MachO; }
  std::string_view targetName() const override {
    return wide_ ? "mach-o-64" : "mach-o-32";
  }
  Arch arch() const override { return archFromCpu(cpu_); }
  Endian endian() const override { return r_.order; }
  bool is64Bit() const override { return wide_; }
  FileType fileType() const override {
    switch (type_) {
      case 1:
        return FileType::Relocatable;
      case 2:
        return FileType::Executable;
      case 4:
        return FileType::Core;
      case 6:
      case 8:
      case 9:
        return FileType::SharedObject;
      default:
        return FileType::Unknown;
    }
  }
  uint64_t entryPoint() const override { return entry_; }
  uint64_t imageBase() const override { return base_; }
  uint32_t cpuType() const { return cpu_; }
  uint32_t cpuSubtype() const { return subtype_; }
  uint32_t flags() const { return flags_; }

  // On-disk load-command geometry, for tools that rewrite the file
  // (qStrip.hpp). `symbolCommand` is the file offset of the LC_SYMTAB
  // command, or 0 when the file has none.
  struct Layout {
    uint64_t commandTable = 0;
    uint32_t commandCount = 0, commandBytes = 0;
    uint64_t symbolCommand = 0;
    bool wide = false;
  };
  Layout layout() const { return layout_; }

  // A section's relocation table, for a rewriter that has to renumber
  // `r_symbolnum` (qStrip.hpp).
  uint32_t relocationPointer(uint32_t index) const {
    return index < rawRelocs_.size() ? rawRelocs_[index].first : 0;
  }
  uint32_t relocationCount(uint32_t index) const {
    return index < rawRelocs_.size() ? rawRelocs_[index].second : 0;
  }
  // The raw nlist slot for a symbol, for rewriting one in place.
  uint64_t symbolSlot(uint32_t index) const {
    const unsigned stride = wide_ ? 16 : 12;
    return uint64_t(symoff_) + uint64_t(index) * stride;
  }
  const std::vector<Section>& sections() const override { return sections_; }
  const std::vector<Segment>& segments() const override { return segments_; }
  const std::vector<Symbol>& symbols() const override { return symbols_; }
  const std::vector<Symbol>& dynamicSymbols() const override {
    return dynamic_;
  }
  std::vector<std::string> neededLibraries() const override { return needed_; }
  const std::vector<std::string>& runpaths() const { return runpaths_; }
  const std::string& installName() const { return installName_; }
  Expected<std::span<const uint8_t>> sectionContents(
      const Section& section) const override {
    if (section.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "invalid Mach-O section"};
    const auto& s = sections_[section.index];
    return data_.subspan(s.fileOffset, s.fileSize);
  }
  Expected<std::vector<Relocation>> relocations(
      const Section& section) const override {
    if (section.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "invalid Mach-O section"};
    return relocs_[section.index];
  }
  static bool sniff(std::span<const uint8_t> d) {
    if (d.size() < 4) return false;
    auto m = detail::Reader{d}.u32(0);
    return m == 0xfeedface || m == 0xfeedfacf || m == 0xcefaedfe ||
           m == 0xcffaedfe;
  }
  static Expected<std::unique_ptr<ObjectFile>> open(
      std::span<const uint8_t> d, const std::filesystem::path& path) {
    if (isFat(d))
      return Error{Error::Code::Unsupported,
                   "select a universal slice with macho::openSlice"};
    return detail::parse<MachOFile>(d, path);
  }
  Error parse() {
    if (!sniff(data_)) return {Error::Code::Malformed, "missing Mach-O magic"};
    uint32_t magic = r_.u32(0);
    wide_ = magic == 0xfeedfacf || magic == 0xcffaedfe;
    r_.order = (magic == 0xcefaedfe || magic == 0xcffaedfe) ? Endian::Big
                                                            : Endian::Little;
    uint64_t header = wide_ ? 32 : 28;
    r_.require(0, header);
    cpu_ = r_.u32(4);
    subtype_ = r_.u32(8);
    type_ = r_.u32(12);
    flags_ = r_.u32(24);
    uint32_t count = r_.u32(16), size = r_.u32(20);
    r_.require(header, size);
    layout_ = {header, count, size, 0, wide_};
    if (count > size / 8) detail::malformed("too many Mach-O load commands");
    uint64_t o = header, end = header + size;
    std::optional<uint64_t> mainOffset;
    bool haveSymbols = false;
    for (uint32_t i = 0; i < count; ++i) {
      if (end - o < 8) detail::malformed("short Mach-O load command");
      uint32_t command = r_.u32(o), length = r_.u32(o + 4);
      if (length < 8 || length > end - o || length % (wide_ ? 8 : 4))
        detail::malformed("invalid Mach-O load command size");
      auto need = [&](uint64_t n) {
        if (length < n) detail::malformed("short Mach-O command payload");
      };
      if (command == 1 || command == 0x19) {
        bool is64 = command == 0x19;
        if (is64 != wide_)
          detail::malformed("segment width differs from Mach-O header");
        need(is64 ? 72 : 56);
        parseSegment(o, length, is64);
      } else if (command == 2) {
        need(24);
        if (haveSymbols) detail::malformed("duplicate Mach-O symbol command");
        haveSymbols = true;
        layout_.symbolCommand = o;
        symoff_ = r_.u32(o + 8);
        nsyms_ = r_.u32(o + 12);
        stroff_ = r_.u32(o + 16);
        strsize_ = r_.u32(o + 20);
      } else if (command == 0x80000028) {
        need(24);
        mainOffset = r_.u64(o + 8);
      } else if (command == 0xc || command == 0xd || command == 0x80000018 ||
                 command == 0x8000001f || command == 0x80000023 ||
                 command == 0x20) {
        need(24);
        uint32_t name = r_.u32(o + 8);
        if (name < 24 || name >= length)
          detail::malformed("invalid dylib name offset");
        auto text = r_.cstr(o + name, length - name);
        if (command == 0xd)
          installName_ = std::move(text);
        else
          needed_.push_back(std::move(text));
      } else if (command == 0x8000001c) {
        need(12);
        uint32_t name = r_.u32(o + 8);
        if (name < 12 || name >= length)
          detail::malformed("invalid rpath offset");
        runpaths_.push_back(r_.cstr(o + name, length - name));
      }
      o += length;
    }
    if (o != end) detail::malformed("Mach-O command count/size mismatch");
    bool baseSet = false;
    for (const auto& g : segments_) {
      if (g.fileSize && (!baseSet || g.vaddr < base_)) {
        base_ = g.vaddr;
        baseSet = true;
      }
      if (mainOffset && g.name == "__TEXT") {
        if (*mainOffset >= g.fileSize)
          detail::malformed("Mach-O entry outside text segment");
        entry_ = detail::checkedAdd(g.vaddr, *mainOffset);
        mainOffset.reset();
      }
    }
    if (mainOffset) detail::malformed("LC_MAIN without text segment");
    if (haveSymbols) parseSymbols();
    parseRelocations();
    return {};
  }

 private:
  void parseSegment(uint64_t o, uint64_t length, bool wide) {
    auto word = [&](uint64_t offset) {
      return wide ? r_.u64(offset) : uint64_t(r_.u32(offset));
    };
    Segment g;
    g.name = r_.fixed(o + 8, 16);
    g.nativeType = wide ? 0x19 : 1;
    g.vaddr = g.paddr = word(o + 24);
    g.memSize = word(o + (wide ? 32 : 28));
    g.fileOffset = word(o + (wide ? 40 : 32));
    g.fileSize = word(o + (wide ? 48 : 36));
    uint32_t prot = r_.u32(o + (wide ? 60 : 44)),
             count = r_.u32(o + (wide ? 64 : 48));
    uint64_t header = wide ? 72 : 56, stride = wide ? 80 : 68;
    if (count > (length - header) / stride)
      detail::malformed("truncated Mach-O section table");
    r_.require(g.fileOffset, g.fileSize);
    if (g.fileSize > g.memSize)
      detail::malformed("Mach-O segment exceeds memory size");
    if (prot & 1) g.flags |= seg::Read;
    if (prot & 2) g.flags |= seg::Write;
    if (prot & 4) g.flags |= seg::Exec;
    for (uint32_t i = 0; i < count; ++i) {
      uint64_t p = o + header + uint64_t(i) * stride;
      Section s;
      s.index = uint32_t(sections_.size());
      s.name = r_.fixed(p, 16);
      s.vma = s.lma = word(p + 32);
      s.size = word(p + (wide ? 40 : 36));
      s.fileOffset = r_.u32(p + (wide ? 48 : 40));
      uint32_t align = r_.u32(p + (wide ? 52 : 44));
      if (align > 63) detail::malformed("invalid Mach-O alignment");
      s.alignment = uint64_t(1) << align;
      uint32_t relocOffset = r_.u32(p + (wide ? 56 : 48)),
               relocCount = r_.u32(p + (wide ? 60 : 52));
      s.nativeFlags = r_.u32(p + (wide ? 64 : 56));
      s.nativeType = uint32_t(s.nativeFlags) & 0xff;
      bool bss =
          s.nativeType == 1 || s.nativeType == 0xc || s.nativeType == 0x12;
      s.fileSize = bss ? 0 : s.size;
      if (bss) s.fileOffset = 0;
      r_.require(s.fileOffset, s.fileSize);
      r_.require(relocOffset, uint64_t(relocCount) * 8);
      s.flags = sec::Alloc | (bss ? sec::Bss : sec::Load);
      if (prot & 2)
        s.flags |= sec::Writable;
      else
        s.flags |= sec::ReadOnly;
      if (s.nativeFlags & 0x80000400)
        s.flags |= sec::Code | sec::Exec;
      else
        s.flags |= sec::Data;
      if (s.nativeFlags & 0x02000000) {
        s.flags |= sec::Debug;
        s.flags &= ~sec::Alloc;
      }
      if (s.nativeType >= 0x11 && s.nativeType <= 0x15) s.flags |= sec::Tls;
      if (s.nativeType == 2) s.flags |= sec::Strings;
      if (relocCount) s.flags |= sec::Reloc;
      rawRelocs_.push_back({relocOffset, relocCount});
      g.sections.push_back(s.index);
      sections_.push_back(std::move(s));
    }
    segments_.push_back(std::move(g));
  }
  void parseSymbols() {
    uint64_t stride = wide_ ? 16 : 12;
    r_.require(symoff_, uint64_t(nsyms_) * stride);
    r_.require(stroff_, strsize_);
    for (uint32_t i = 0; i < nsyms_; ++i) {
      uint64_t o = symoff_ + uint64_t(i) * stride;
      Symbol s;
      s.index = i;
      s.name = r_.stringAt(stroff_, strsize_, r_.u32(o));
      uint8_t type = r_.u8(o + 4), section = r_.u8(o + 5);
      uint16_t desc = r_.u16(o + 6);
      s.value = wide_ ? r_.u64(o + 8) : r_.u32(o + 8);
      s.nativeType = type;
      s.binding = type & 1 ? SymbolBinding::Global : SymbolBinding::Local;
      if (desc & 0xc0) s.binding = SymbolBinding::Weak;
      if (type & 0xe0)
        s.flags |= sym::Debug;
      else {
        if (type & 0x10) s.flags |= sym::Hidden;
        switch (type & 0xe) {
          case 0:
            if (s.value && (type & 1)) {
              s.kind = SymbolKind::Common;
              s.size = s.value;
            } else
              s.flags |=
                  sym::Undefined | ((type & 1) ? sym::Imported : sym::None);
            break;
          case 2:
            s.flags |= sym::Absolute;
            break;
          case 0xe:
            if (!section || section > sections_.size())
              detail::malformed("invalid Mach-O symbol section");
            s.section = section - 1;
            s.kind = sections_[*s.section].flags & sec::Code
                         ? SymbolKind::Function
                         : SymbolKind::Object;
            break;
          case 0xa:
            s.kind = SymbolKind::Indirect;
            break;
        }
        if ((type & 1) && !(s.flags & (sym::Undefined | sym::Hidden)))
          s.flags |= sym::Exported;
      }
      symbols_.push_back(s);
      if (!(s.flags & sym::Debug) &&
          (s.flags & (sym::Imported | sym::Exported))) {
        s.index = uint32_t(dynamic_.size());
        dynamic_.push_back(std::move(s));
      }
    }
  }
  void parseRelocations() {
    relocs_.resize(sections_.size());
    for (const auto& s : sections_) {
      auto [base, count] = rawRelocs_[s.index];
      for (uint32_t i = 0; i < count; ++i) {
        uint64_t o = uint64_t(base) + uint64_t(i) * 8;
        uint32_t address = r_.u32(o), bits = r_.u32(o + 4);
        Relocation r;
        if (!wide_ && (address & 0x80000000)) {
          r.offset = address & 0xffffff;
          r.pcRelative = address & 0x40000000;
          r.bitSize = uint8_t(8u << ((address >> 28) & 3));
          r.nativeType = (address >> 24) & 15;
          r.scattered = true;
          r.nativeValue = bits;
        } else {
          r.offset = address;
          bool little = r_.order == Endian::Little;
          uint32_t symbol = little ? bits & 0xffffff : bits >> 8;
          bool external = little ? bits & 0x8000000 : bits & 0x10;
          r.pcRelative = little ? bits & 0x1000000 : bits & 0x80;
          r.bitSize =
              uint8_t(8u << (little ? ((bits >> 25) & 3) : ((bits >> 5) & 3)));
          r.nativeType = little ? bits >> 28 : bits & 15;
          if (external) {
            if (symbol >= symbols_.size())
              detail::malformed("invalid Mach-O relocation symbol");
            r.symbol = symbol;
          } else if (symbol) {
            // ARM64_RELOC_ADDEND carries a signed immediate instead of a
            // section ordinal.
            if (cpu_ == 0x100000c && r.nativeType == 10) {
              r.addend = std::bit_cast<int32_t>(symbol << 8) / 256;
              r.hasAddend = true;
            } else {
              if (symbol > sections_.size())
                detail::malformed("invalid Mach-O relocation section");
              r.targetSection = symbol - 1;
            }
          }
        }
        r.typeName = "MACHO_RELOC_" + std::to_string(r.nativeType);
        // Paired/scattered records can carry values in the address word.
        // Preserve them verbatim.
        relocs_[s.index].push_back(std::move(r));
      }
    }
  }
  detail::Reader r_;
  bool wide_ = false;
  uint32_t cpu_ = 0, subtype_ = 0, type_ = 0, flags_ = 0;
  uint32_t symoff_ = 0, nsyms_ = 0, stroff_ = 0, strsize_ = 0;
  uint64_t entry_ = 0, base_ = 0;
  Layout layout_;
  std::vector<Section> sections_;
  std::vector<Segment> segments_;
  std::vector<Symbol> symbols_, dynamic_;
  std::vector<std::string> needed_, runpaths_;
  std::string installName_;
  std::vector<std::pair<uint32_t, uint32_t>> rawRelocs_;
  std::vector<std::vector<Relocation>> relocs_;
};
inline Expected<std::unique_ptr<ObjectFile>> openSlice(
    std::span<const uint8_t> bytes, size_t index,
    const std::filesystem::path& path = {}) {
  auto table = slices(bytes);
  if (!table) return table.error();
  if (index >= table->size())
    return Error{Error::Code::OutOfRange, "invalid universal slice index"};
  const auto& s = (*table)[index];
  auto result = MachOFile::open(bytes.subspan(s.offset, s.size), path);
  if (result && static_cast<const MachOFile&>(**result).cpuType() != s.cpuType)
    return Error{Error::Code::Malformed, "universal slice CPU mismatch"};
  return result;
}
inline void registerTargets(Registry& registry) {
  for (bool wide : {false, true})
    registry.add({wide ? "mach-o-64" : "mach-o-32", Format::MachO,
                  [wide](auto d) {
                    if (!MachOFile::sniff(d)) return false;
                    auto m = detail::Reader{d}.u32(0);
                    return wide == (m == 0xfeedfacf || m == 0xcffaedfe);
                  },
                  MachOFile::open});
  registry.add({"mach-o-universal", Format::MachO, isFat, MachOFile::open});
}
namespace query {
using namespace qbfd::query;
inline auto type(uint32_t value) { return nativeType(value); }
inline auto segment(std::string name) { return named(std::move(name)); }
}  // namespace query
}  // namespace qbfd::macho
