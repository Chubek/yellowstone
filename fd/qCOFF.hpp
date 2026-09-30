// qCOFF.hpp - COFF object (.obj / bigobj) backend for qBFD
#pragma once

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "qBFDCore.hpp"

namespace qbfd::coff {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
namespace k {
constexpr uint16_t MachI386 = 0x014C;
constexpr uint16_t MachAmd64 = 0x8664;
constexpr uint16_t MachArm = 0x01C0;
constexpr uint16_t MachArmNT = 0x01C4;
constexpr uint16_t MachArm64 = 0xAA64;
constexpr uint16_t MachRiscV32 = 0x5032;
constexpr uint16_t MachRiscV64 = 0x5064;
constexpr uint16_t MachLoongArch64 = 0x6264;

// Section Characteristics
constexpr uint32_t ScnCode = 0x00000020;
constexpr uint32_t ScnInitData = 0x00000040;
constexpr uint32_t ScnUninitData = 0x00000080;
constexpr uint32_t ScnLnkInfo = 0x00000200;
constexpr uint32_t ScnLnkRemove = 0x00000800;
constexpr uint32_t ScnLnkComdat = 0x00001000;
constexpr uint32_t ScnAlignMask = 0x00F00000;
constexpr uint32_t ScnNRelocOvfl = 0x01000000;
constexpr uint32_t ScnDiscardable = 0x02000000;
constexpr uint32_t ScnExecute = 0x20000000;
constexpr uint32_t ScnRead = 0x40000000;
constexpr uint32_t ScnWrite = 0x80000000;

// Storage classes
constexpr uint8_t ClassNull = 0;
constexpr uint8_t ClassExternal = 2;
constexpr uint8_t ClassStatic = 3;
constexpr uint8_t ClassLabel = 6;
constexpr uint8_t ClassFunction = 101;
constexpr uint8_t ClassFile = 103;
constexpr uint8_t ClassSection = 104;
constexpr uint8_t ClassWeakExternal = 105;
constexpr uint8_t ClassClrToken = 107;

constexpr int32_t SymUndefined = 0;
constexpr int32_t SymAbsolute = -1;
constexpr int32_t SymDebug = -2;

constexpr size_t FileHeaderSize = 20;
constexpr size_t BigObjHeaderSize = 56;
constexpr size_t SectionHeaderSize = 40;
constexpr size_t SymbolSize = 18;
constexpr size_t BigSymbolSize = 20;
constexpr size_t RelocSize = 10;

// ANON_OBJECT_HEADER_BIGOBJ ClassID
constexpr uint8_t BigObjClassId[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA,
                                       0xA9, 0x4B, 0xAF, 0x20, 0xFA, 0xF6,
                                       0x6A, 0xA4, 0xDC, 0xB8};
}  // namespace k

// ---------------------------------------------------------------------------
// Little-endian bounds-checked reader
// ---------------------------------------------------------------------------
using Reader = detail::Reader;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
inline bool knownMachine(uint16_t m) {
  switch (m) {
    case k::MachI386:
    case k::MachAmd64:
    case k::MachArm:
    case k::MachArmNT:
    case k::MachArm64:
    case k::MachRiscV32:
    case k::MachRiscV64:
    case k::MachLoongArch64:
      return true;
    default:
      return false;
  }
}

inline Arch archFromMachine(uint16_t m) {
  switch (m) {
    case k::MachI386:
      return Arch::X86;
    case k::MachAmd64:
      return Arch::X86_64;
    case k::MachArm:
    case k::MachArmNT:
      return Arch::ARM;
    case k::MachArm64:
      return Arch::AArch64;
    case k::MachRiscV32:
      return Arch::RISCV32;
    case k::MachRiscV64:
      return Arch::RISCV64;
    case k::MachLoongArch64:
      return Arch::LoongArch64;
    default:
      return Arch::Unknown;
  }
}

inline std::string_view targetNameFor(uint16_t m) {
  switch (m) {
    case k::MachI386:
      return "coff-i386";
    case k::MachAmd64:
      return "coff-x86-64";
    case k::MachArm:
    case k::MachArmNT:
      return "coff-arm";
    case k::MachArm64:
      return "coff-aarch64";
    case k::MachRiscV32:
      return "coff-riscv32";
    case k::MachRiscV64:
      return "coff-riscv64";
    case k::MachLoongArch64:
      return "coff-loongarch64";
    default:
      return "coff-unknown";
  }
}

// Name, bit size, pc-relative for each (machine, type)
struct RelocInfo {
  const char* name;
  uint8_t bits;
  bool pcrel;
};

inline RelocInfo relocInfo(uint16_t machine, uint16_t t) {
  switch (machine) {
    case k::MachAmd64: {
      static const RelocInfo tbl[] = {
          {"IMAGE_REL_AMD64_ABSOLUTE", 0, false},
          {"IMAGE_REL_AMD64_ADDR64", 64, false},
          {"IMAGE_REL_AMD64_ADDR32", 32, false},
          {"IMAGE_REL_AMD64_ADDR32NB", 32, false},
          {"IMAGE_REL_AMD64_REL32", 32, true},
          {"IMAGE_REL_AMD64_REL32_1", 32, true},
          {"IMAGE_REL_AMD64_REL32_2", 32, true},
          {"IMAGE_REL_AMD64_REL32_3", 32, true},
          {"IMAGE_REL_AMD64_REL32_4", 32, true},
          {"IMAGE_REL_AMD64_REL32_5", 32, true},
          {"IMAGE_REL_AMD64_SECTION", 16, false},
          {"IMAGE_REL_AMD64_SECREL", 32, false},
          {"IMAGE_REL_AMD64_SECREL7", 8, false},
          {"IMAGE_REL_AMD64_TOKEN", 32, false},
          {"IMAGE_REL_AMD64_SREL32", 32, true},
          {"IMAGE_REL_AMD64_PAIR", 0, false},
          {"IMAGE_REL_AMD64_SSPAN32", 32, false},
      };
      if (t < std::size(tbl)) return tbl[t];
      break;
    }
    case k::MachI386:
      switch (t) {
        case 0:
          return {"IMAGE_REL_I386_ABSOLUTE", 0, false};
        case 1:
          return {"IMAGE_REL_I386_DIR16", 16, false};
        case 2:
          return {"IMAGE_REL_I386_REL16", 16, true};
        case 6:
          return {"IMAGE_REL_I386_DIR32", 32, false};
        case 7:
          return {"IMAGE_REL_I386_DIR32NB", 32, false};
        case 9:
          return {"IMAGE_REL_I386_SEG12", 16, false};
        case 10:
          return {"IMAGE_REL_I386_SECTION", 16, false};
        case 11:
          return {"IMAGE_REL_I386_SECREL", 32, false};
        case 12:
          return {"IMAGE_REL_I386_TOKEN", 32, false};
        case 13:
          return {"IMAGE_REL_I386_SECREL7", 8, false};
        case 20:
          return {"IMAGE_REL_I386_REL32", 32, true};
      }
      break;
    case k::MachArm64: {
      static const RelocInfo tbl[] = {
          {"IMAGE_REL_ARM64_ABSOLUTE", 0, false},
          {"IMAGE_REL_ARM64_ADDR32", 32, false},
          {"IMAGE_REL_ARM64_ADDR32NB", 32, false},
          {"IMAGE_REL_ARM64_BRANCH26", 32, true},
          {"IMAGE_REL_ARM64_PAGEBASE_REL21", 32, true},
          {"IMAGE_REL_ARM64_REL21", 32, true},
          {"IMAGE_REL_ARM64_PAGEOFFSET_12A", 32, false},
          {"IMAGE_REL_ARM64_PAGEOFFSET_12L", 32, false},
          {"IMAGE_REL_ARM64_SECREL", 32, false},
          {"IMAGE_REL_ARM64_SECREL_LOW12A", 32, false},
          {"IMAGE_REL_ARM64_SECREL_HIGH12A", 32, false},
          {"IMAGE_REL_ARM64_SECREL_LOW12L", 32, false},
          {"IMAGE_REL_ARM64_TOKEN", 32, false},
          {"IMAGE_REL_ARM64_SECTION", 16, false},
          {"IMAGE_REL_ARM64_ADDR64", 64, false},
          {"IMAGE_REL_ARM64_BRANCH19", 32, true},
          {"IMAGE_REL_ARM64_BRANCH14", 32, true},
          {"IMAGE_REL_ARM64_REL32", 32, true},
      };
      if (t < std::size(tbl)) return tbl[t];
      break;
    }
    case k::MachArm:
    case k::MachArmNT:
      switch (t) {
        case 0:
          return {"IMAGE_REL_ARM_ABSOLUTE", 0, false};
        case 1:
          return {"IMAGE_REL_ARM_ADDR32", 32, false};
        case 2:
          return {"IMAGE_REL_ARM_ADDR32NB", 32, false};
        case 3:
          return {"IMAGE_REL_ARM_BRANCH24", 32, true};
        case 4:
          return {"IMAGE_REL_ARM_BRANCH11", 16, true};
        case 14:
          return {"IMAGE_REL_ARM_REL32", 32, true};
        case 15:
          return {"IMAGE_REL_ARM_SECTION", 16, false};
        case 16:
          return {"IMAGE_REL_ARM_SECREL", 32, false};
        case 17:
          return {"IMAGE_REL_ARM_MOV32A", 32, false};
        case 19:
          return {"IMAGE_REL_ARM_MOV32T", 32, false};
        case 20:
          return {"IMAGE_REL_ARM_BRANCH20T", 32, true};
        case 22:
          return {"IMAGE_REL_ARM_BRANCH24T", 32, true};
        case 23:
          return {"IMAGE_REL_ARM_BLX23T", 32, true};
        case 24:
          return {"IMAGE_REL_ARM_PAIR", 0, false};
      }
      break;
  }
  return {nullptr, 0, false};
}

// ---------------------------------------------------------------------------
// COFFFile
// ---------------------------------------------------------------------------
class COFFFile final : public ObjectFile {
 public:
  COFFFile(std::span<const uint8_t> data, std::filesystem::path path)
      : ObjectFile(data, std::move(path)), r_{data} {}

  // -- Identity ------------------------------------------------------------
  Format format() const override { return Format::COFF; }
  std::string_view targetName() const override {
    return targetNameFor(machine_);
  }
  Arch arch() const override { return archFromMachine(machine_); }
  Endian endian() const override { return Endian::Little; }
  bool is64Bit() const override {
    return machine_ == k::MachAmd64 || machine_ == k::MachArm64 ||
           machine_ == k::MachRiscV64 || machine_ == k::MachLoongArch64;
  }
  FileType fileType() const override { return FileType::Relocatable; }
  uint64_t entryPoint() const override { return 0; }

  // -- COFF-specific accessors --------------------------------------------
  uint16_t machine() const { return machine_; }
  uint32_t timestamp() const { return timestamp_; }
  uint16_t characteristics() const { return fileChars_; }
  bool isBigObj() const { return bigobj_; }

  // On-disk table geometry, for tools that rewrite the file (qStrip.hpp).
  struct Layout {
    uint64_t sectionTable = 0;
    uint32_t symbolTable = 0, symbolCount = 0, stringTable = 0;
    uint32_t symbolSize = k::SymbolSize;
  };
  Layout layout() const { return layout_; }

  // The relocation table's location, for a rewriter that has to patch
  // `SymbolTableIndex` fields (qStrip.hpp).
  uint32_t relocationPointer(uint32_t index) const {
    return index < raw_.size() ? raw_[index].relocPtr : 0;
  }
  // True when the real count lives in the first record's VirtualAddress
  // (IMAGE_SCN_LNK_NRELOC_OVFL).
  bool relocationOverflow(uint32_t index) const {
    return index < raw_.size() &&
           (raw_[index].characteristics & k::ScnNRelocOvfl) &&
           raw_[index].relocCount == 0xFFFF;
  }
  uint32_t relocationCount(uint32_t index) const {
    return index < raw_.size() ? raw_[index].relocCount : 0;
  }
  // File offsets of a section header and its relocation fields, for a rewriter
  // that changes them (qStrip.hpp). Both formats share the 40-byte layout.
  uint64_t sectionHeaderOffset(uint32_t index) const {
    return layout_.sectionTable + uint64_t(index) * k::SectionHeaderSize;
  }
  uint64_t relocationPointerOffset(uint32_t index) const {
    return sectionHeaderOffset(index) + 24;
  }
  uint64_t relocationCountOffset(uint32_t index) const {
    return sectionHeaderOffset(index) + 32;
  }
  // The storage class / type / section-number fields of a raw symbol record,
  // and where its name lives, are defined just above `private:` below, next to
  // the private helpers they use.

  // COMDAT info attached to a section (from the section symbol's aux record)
  struct Comdat {
    uint8_t selection = 0;    // IMAGE_COMDAT_SELECT_*
    uint32_t associated = 0;  // section index (0-based) if selection == 5
    uint32_t checksum = 0;
    std::string leader;  // name of the COMDAT leader symbol
  };
  const std::unordered_map<uint32_t, Comdat>& comdats() const {
    return comdats_;
  }

  // Weak-external target: symbol index of the default definition
  struct WeakExternal {
    uint32_t tagSymbol = 0;        // Symbol::index of default
    uint32_t characteristics = 0;  // IMAGE_WEAK_EXTERN_SEARCH_*
  };
  const std::unordered_map<uint32_t, WeakExternal>& weakExternals() const {
    return weakExternals_;
  }

  // .drectve linker directives (e.g. "/DEFAULTLIB:LIBCMT")
  std::string directives() const {
    const Section* s = findSection(".drectve");
    if (!s) return {};
    auto c = sectionContents(*s);
    if (!c) return {};
    return std::string(reinterpret_cast<const char*>(c->data()), c->size());
  }

  // -- Sections -------------------------------------------------------------
  const std::vector<Section>& sections() const override { return sections_; }

  Expected<std::span<const uint8_t>> sectionContents(
      const Section& s) const override {
    if (s.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "bad section index"};
    const auto& actual = sections_[s.index];
    if (actual.flags & sec::Bss) return std::span<const uint8_t>{};
    if (!r_.has(actual.fileOffset, actual.fileSize))
      return Error{Error::Code::Malformed,
                   "section " + s.name + " extends past end of file"};
    return data_.subspan(actual.fileOffset, actual.fileSize);
  }

  // -- Symbols --------------------------------------------------------------
  const std::vector<Symbol>& symbols() const override { return symbols_; }

  // -- Relocations ----------------------------------------------------------
  Expected<std::vector<Relocation>> relocations(
      const Section& requested) const override {
    if (requested.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "bad section index"};
    const auto& target = sections_[requested.index];
    if (target.index >= raw_.size())
      return Error{Error::Code::OutOfRange, "bad section index"};
    const auto& h = raw_[target.index];
    std::vector<Relocation> out;
    if (!h.relocPtr || !h.relocCount) return out;

    uint64_t o = h.relocPtr;
    uint64_t count = h.relocCount;
    // Overflow: real count is stored in the VirtualAddress of reloc #0
    if ((h.characteristics & k::ScnNRelocOvfl) && h.relocCount == 0xFFFF) {
      if (!r_.has(o, k::RelocSize))
        return Error{Error::Code::Malformed, "reloc overflow entry truncated"};
      if (!r_.u32(o))
        return Error{Error::Code::Malformed, "zero relocation overflow count"};
      count = r_.u32(o) - 1;
      o += k::RelocSize;
    }
    if (!r_.has(o, count * k::RelocSize))
      return Error{Error::Code::Malformed,
                   "relocations for " + target.name + " truncated"};

    out.reserve(count);
    for (uint64_t i = 0; i < count; ++i, o += k::RelocSize) {
      Relocation rl;
      uint32_t va = r_.u32(o);
      rl.offset = va >= h.virtualAddress ? va - h.virtualAddress : va;
      uint32_t symIdx = r_.u32(o + 4);
      if (auto it = symIndexMap_.find(symIdx); it != symIndexMap_.end())
        rl.symbol = it->second;
      if (!rl.symbol)
        return Error{Error::Code::Malformed, "invalid COFF relocation symbol"};
      rl.nativeType = r_.u16(o + 8);
      RelocInfo ri = relocInfo(machine_, uint16_t(rl.nativeType));
      rl.typeName =
          ri.name ? ri.name : "IMAGE_REL_" + std::to_string(rl.nativeType);
      rl.bitSize = ri.bits;
      rl.pcRelative = ri.pcrel;
      // COFF has no explicit addend; it lives in the section bytes.
      if (machine_ == k::MachI386 || machine_ == k::MachAmd64) {
        rl.addend = readImplicitAddend(target, rl);
        rl.hasAddend = rl.bitSize && rl.bitSize % 8 == 0 &&
                       rl.offset <= target.fileSize &&
                       rl.bitSize / 8 <= target.fileSize - rl.offset;
      }
      out.push_back(std::move(rl));
    }
    return out;
  }

  // -- Segments: none for relocatable objects ------------------------------
  const std::vector<Segment>& segments() const override {
    static const std::vector<Segment> empty;
    return empty;
  }

  // -- Parsing --------------------------------------------------------------
  static bool isBigObjHeader(const Reader& r) {
    if (!r.has(0, k::BigObjHeaderSize)) return false;
    if (r.u16(0) != 0 || r.u16(2) != 0xFFFF || r.u16(4) < 2) return false;
    if (!r.has(12, 16)) return false;
    return std::memcmp(r.d.data() + 12, k::BigObjClassId, 16) == 0;
  }

  static bool sniff(std::span<const uint8_t> d, uint16_t wantMachine = 0) {
    Reader r{d};
    if (isBigObjHeader(r)) {
      auto machine = r.u16(6);
      return wantMachine ? machine == wantMachine : knownMachine(machine);
    }
    if (!r.has(0, 2)) return false;
    auto machine = r.u16(0);
    return wantMachine ? machine == wantMachine : knownMachine(machine);
  }

  static Expected<std::unique_ptr<ObjectFile>> open(
      std::span<const uint8_t> d, const std::filesystem::path& p) {
    try {
      auto f = std::make_unique<COFFFile>(d, p);
      if (Error e = f->parse()) return e;
      return std::unique_ptr<ObjectFile>(std::move(f));
    } catch (const detail::ParseFailure& e) {
      return e.error;
    }
  }

 private:
  struct RawSectionHeader {
    uint32_t virtualSize, virtualAddress, rawSize, rawPtr;
    uint32_t relocPtr;
    uint16_t relocCount;
    uint32_t characteristics;
  };

  Error parse() {
    uint64_t sh;  // section table offset
    uint32_t nSections, symPtr, nSyms;

    if (isBigObjHeader(r_)) {
      bigobj_ = true;
      machine_ = r_.u16(6);
      timestamp_ = r_.u32(8);
      nSections = r_.u32(44);
      symPtr = r_.u32(48);
      nSyms = r_.u32(52);
      sh = k::BigObjHeaderSize;
      symSize_ = k::BigSymbolSize;
    } else {
      if (!r_.has(0, k::FileHeaderSize))
        return {Error::Code::Malformed, "file header truncated"};
      machine_ = r_.u16(0);
      nSections = r_.u16(2);
      timestamp_ = r_.u32(4);
      symPtr = r_.u32(8);
      nSyms = r_.u32(12);
      uint16_t optSize = r_.u16(16);
      fileChars_ = r_.u16(18);
      sh = k::FileHeaderSize + optSize;
      symSize_ = k::SymbolSize;
    }
    if (!knownMachine(machine_))
      return {Error::Code::Unsupported, "unknown COFF machine"};
    layout_ = {sh, symPtr, nSyms, 0, symSize_};
    if (!r_.has(sh, uint64_t(nSections) * k::SectionHeaderSize))
      return {Error::Code::Malformed, "section table truncated"};

    uint64_t strTab = 0;
    if (symPtr) {
      strTab = uint64_t(symPtr) + uint64_t(nSyms) * symSize_;
      layout_.stringTable = uint32_t(strTab);
      if (!r_.has(symPtr, uint64_t(nSyms) * symSize_))
        return {Error::Code::Malformed, "symbol table truncated"};
    }

    // Section table
    sections_.reserve(nSections);
    raw_.reserve(nSections);
    for (uint32_t i = 0; i < nSections; ++i) {
      uint64_t o = sh + uint64_t(i) * k::SectionHeaderSize;
      RawSectionHeader h{};
      h.virtualSize = r_.u32(o + 8);
      h.virtualAddress = r_.u32(o + 12);
      h.rawSize = r_.u32(o + 16);
      h.rawPtr = r_.u32(o + 20);
      h.relocPtr = r_.u32(o + 24);
      h.relocCount = r_.u16(o + 32);
      h.characteristics = r_.u32(o + 36);
      raw_.push_back(h);

      Section s;
      s.index = i;
      s.name = sectionName(o, strTab);
      s.vma = s.lma = h.virtualAddress;  // normally 0 in objects
      // For objects SizeOfRawData is the size; VirtualSize is usually 0.
      s.size = h.rawSize ? h.rawSize : h.virtualSize;
      bool bss = (h.characteristics & k::ScnUninitData) || h.rawPtr == 0;
      s.fileOffset = bss ? 0 : h.rawPtr;
      s.fileSize = bss ? 0 : h.rawSize;
      s.nativeFlags = h.characteristics;
      s.flags = mapSectionFlags(h.characteristics, bss);
      uint32_t alignBits = (h.characteristics & k::ScnAlignMask) >> 20;
      s.alignment = alignBits ? (1u << (alignBits - 1)) : 16;
      if (h.relocCount) s.flags |= sec::Reloc;
      r_.require(s.fileOffset, s.fileSize);
      sections_.push_back(std::move(s));
    }

    if (nSyms && !symPtr) detail::malformed("missing symbol table");
    if (symPtr) {
      r_.require(strTab, 4);
      if (r_.u32(strTab) < 4)
        detail::malformed("invalid COFF string table size");
      r_.require(strTab, r_.u32(strTab));
    }
    for (uint32_t i = 0; i < nSyms;) {
      auto o = uint64_t(symPtr) + uint64_t(i) * symSize_;
      auto aux = r_.u8(o + symSize_ - 1);
      if (aux >= nSyms - i)
        detail::malformed("COFF auxiliary records exceed table");
      i += 1 + aux;
    }
    parseSymbols(symPtr, nSyms, strTab);
    for (const auto& section : sections_) {
      auto rel = relocations(section);
      if (!rel) return rel.error();
    }
    return {};
  }

  std::string sectionName(uint64_t hdrOff, uint64_t strTab) const {
    std::string n = r_.fixed(hdrOff, 8);
    if (n.size() > 1 && n[0] == '/' && strTab) {
      uint64_t off = 0;
      for (size_t j = 1; j < n.size(); ++j) {
        if (n[j] < '0' || n[j] > '9') return n;
        off = off * 10 + uint64_t(n[j] - '0');
      }
      return r_.stringAt(strTab, r_.u32(strTab), off);
    }
    return n;
  }

  static uint32_t mapSectionFlags(uint32_t c, bool bss) {
    uint32_t f = sec::None;
    bool linkOnly = c & (k::ScnLnkInfo | k::ScnLnkRemove);
    if (!linkOnly) f |= sec::Alloc;
    if (c & k::ScnCode) f |= sec::Code;
    if (c & (k::ScnInitData | k::ScnUninitData)) f |= sec::Data;
    if (bss)
      f |= sec::Bss;
    else
      f |= sec::Load;
    if (c & k::ScnExecute) f |= sec::Exec;
    if (c & k::ScnWrite)
      f |= sec::Writable;
    else if (c & k::ScnRead)
      f |= sec::ReadOnly;
    if (c & (k::ScnDiscardable | k::ScnLnkRemove | k::ScnLnkComdat))
      f |= sec::Discard;
    if (c & k::ScnLnkInfo) f |= sec::Note;
    return f;
  }

  int32_t sectionNumberAt(uint64_t o) const {
    return bigobj_ ? int32_t(r_.u32(o + 12)) : int32_t(int16_t(r_.u16(o + 12)));
  }
  uint64_t recordOffset(uint32_t record) const {
    return uint64_t(layout_.symbolTable) + uint64_t(record) * layout_.symbolSize;
  }

 public:
  // Defined here rather than at the declarations above: a member function
  // cannot be both declared and defined inside the same class, and a body may
  // only use members already declared.
  std::string rawSymbolName(uint32_t record) const {
    uint64_t o = recordOffset(record);
    if (!r_.has(o, 8)) return {};
    if (r_.u32(o)) return r_.fixed(o, 8);
    if (!layout_.stringTable) return {};
    try {
      return r_.stringAt(layout_.stringTable, r_.u32(layout_.stringTable),
                         r_.u32(o + 4));
    } catch (const detail::ParseFailure&) {
      return {};
    }
  }
  uint8_t rawSymbolClass(uint32_t record) const {
    uint64_t o = recordOffset(record);
    return r_.has(o, 1) ? r_.u8(o + (bigobj_ ? 18 : 16)) : 0;
  }
  int32_t rawSymbolSection(uint32_t record) const {
    uint64_t o = recordOffset(record);
    return r_.has(o, 12) ? sectionNumberAt(o) : 0;
  }
  uint32_t rawSymbolAuxCount(uint32_t record) const {
    uint64_t o = recordOffset(record);
    return r_.has(o, 1) ? uint32_t(r_.u8(o + (bigobj_ ? 19 : 17))) : 0;
  }

  void parseSymbols(uint32_t symPtr, uint32_t nSyms, uint64_t strTab) {
    if (!symPtr || !nSyms) return;
    const uint64_t typeOff = bigobj_ ? 16 : 14;
    const uint64_t clsOff = bigobj_ ? 18 : 16;
    const uint64_t auxOff = bigobj_ ? 19 : 17;

    // First pass: map raw index -> Symbol::index, so aux records referring
    // to other symbols (weak externals) can be resolved.
    std::vector<uint32_t> rawIdx;
    for (uint32_t i = 0; i < nSyms;) {
      rawIdx.push_back(i);
      i += 1 + r_.u8(uint64_t(symPtr) + uint64_t(i) * symSize_ + auxOff);
    }
    for (uint32_t j = 0; j < rawIdx.size(); ++j) symIndexMap_[rawIdx[j]] = j;

    symbols_.reserve(rawIdx.size());
    for (uint32_t i : rawIdx) {
      uint64_t o = uint64_t(symPtr) + uint64_t(i) * symSize_;
      uint8_t nAux = r_.u8(o + auxOff);

      Symbol s;
      s.index = uint32_t(symbols_.size());
      s.name = r_.u32(o) == 0
                   ? r_.stringAt(strTab, r_.u32(strTab), r_.u32(o + 4))
                   : r_.fixed(o, 8);
      uint32_t value = r_.u32(o + 8);
      int32_t scn = sectionNumberAt(o);
      uint16_t type = r_.u16(o + typeOff);
      uint8_t cls = r_.u8(o + clsOff);
      s.nativeType = uint32_t(cls) << 16 | type;

      switch (cls) {
        case k::ClassExternal:
          s.binding = SymbolBinding::Global;
          break;
        case k::ClassWeakExternal:
          s.binding = SymbolBinding::Weak;
          break;
        default:
          s.binding = SymbolBinding::Local;
          break;
      }

      bool isFunc = (type >> 4) == 2;
      if (cls == k::ClassFile) {
        s.kind = SymbolKind::File;
        s.flags |= sym::Debug;
        // Aux records hold the file name (18/20 bytes each, concatenated)
        if (nAux) s.name = r_.fixed(o + symSize_, nAux * symSize_);
      } else if (cls == k::ClassSection ||
                 (cls == k::ClassStatic && value == 0 && scn > 0 && nAux == 1 &&
                  !isFunc && uint32_t(scn - 1) < sections_.size() &&
                  s.name == sections_[scn - 1].name)) {
        s.kind = SymbolKind::Section;
        if (nAux == 1) parseSectionAux(o + symSize_, uint32_t(scn - 1), s.name);
      } else if (isFunc || cls == k::ClassFunction) {
        s.kind = SymbolKind::Function;
        if (cls == k::ClassFunction) s.flags |= sym::Debug;  // .bf / .ef / .lf
      } else if (cls == k::ClassLabel) {
        s.kind = SymbolKind::None;
      } else {
        s.kind = SymbolKind::Object;
      }

      if (scn == k::SymUndefined) {
        if (cls == k::ClassExternal && value != 0) {
          s.kind = SymbolKind::Common;
          s.size = value;
        } else {
          s.flags |= sym::Undefined;
        }
        if (cls == k::ClassWeakExternal && nAux >= 1) {
          WeakExternal w;
          uint32_t tagRaw = r_.u32(o + symSize_);
          if (auto it = symIndexMap_.find(tagRaw); it != symIndexMap_.end())
            w.tagSymbol = it->second;
          w.characteristics = r_.u32(o + symSize_ + 4);
          weakExternals_[s.index] = w;
        }
      } else if (scn == k::SymAbsolute) {
        s.flags |= sym::Absolute;
        s.value = value;
      } else if (scn == k::SymDebug) {
        s.flags |= sym::Debug;
        s.value = value;
      } else if (scn > 0 && uint32_t(scn) <= sections_.size()) {
        s.section = uint32_t(scn - 1);
        s.value = sections_[*s.section].vma + value;
      } else
        detail::malformed("invalid COFF symbol section");
      symbols_.push_back(std::move(s));
    }
  }

  void parseSectionAux(uint64_t o, uint32_t section, const std::string& name) {
    if (section >= sections_.size())
      detail::malformed("invalid COMDAT section");
    uint8_t selection = r_.u8(o + 14);
    if (!selection) return;
    Comdat c;
    c.selection = selection;
    c.checksum = r_.u32(o + 8);
    uint32_t associated = r_.u16(o + 12) | uint32_t(r_.u16(o + 16)) << 16;
    if (selection == 5) {
      if (!associated || associated > sections_.size())
        detail::malformed("invalid associative COMDAT");
      c.associated = associated - 1;
    }
    c.leader = name;
    comdats_[section] = std::move(c);
  }

  int64_t readImplicitAddend(const Section& s, const Relocation& r) const {
    if (!r.bitSize || r.bitSize > 64 || r.bitSize % 8 ||
        r.offset > s.fileSize || r.bitSize / 8 > s.fileSize - r.offset)
      return 0;
    uint64_t value = r_.integer(s.fileOffset + r.offset, r.bitSize / 8);
    if (r.bitSize < 64 && (value & (uint64_t(1) << (r.bitSize - 1))))
      value |= UINT64_MAX << r.bitSize;
    return std::bit_cast<int64_t>(value);
  }

 private:
  Reader r_;
  uint16_t machine_ = 0, fileChars_ = 0;
  uint32_t timestamp_ = 0;
  bool bigobj_ = false;
  uint32_t symSize_ = k::SymbolSize;
  Layout layout_;
  std::vector<RawSectionHeader> raw_;
  std::vector<Section> sections_;
  std::vector<Symbol> symbols_;
  std::unordered_map<uint32_t, uint32_t> symIndexMap_;
  std::unordered_map<uint32_t, Comdat> comdats_;
  std::unordered_map<uint32_t, WeakExternal> weakExternals_;
};
inline void registerTargets(Registry& registry) {
  for (auto machine :
       {k::MachI386, k::MachAmd64, k::MachArm, k::MachArmNT, k::MachArm64,
        k::MachRiscV32, k::MachRiscV64, k::MachLoongArch64}) {
    // ARM and Thumb share one target and are both accepted by it.
    if (machine == k::MachArmNT) continue;
    registry.add({std::string(targetNameFor(machine)), Format::COFF,
                  [machine](auto bytes) {
                    return COFFFile::sniff(bytes, machine) ||
                           (machine == k::MachArm &&
                            COFFFile::sniff(bytes, k::MachArmNT));
                  },
                  COFFFile::open});
  }
}
namespace query {
using namespace qbfd::query;
inline auto comdat() {
  return ObjectDSL::make_pred(
      [](const Section& s) { return (s.nativeFlags & k::ScnLnkComdat) != 0; });
}
inline auto directives() { return named(".drectve"); }
}  // namespace query
}  // namespace qbfd::coff
