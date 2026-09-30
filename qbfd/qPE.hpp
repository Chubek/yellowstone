// qPE.hpp - PE / PE32+ backend for qBFD
#pragma once

#include <algorithm>
#include <cstring>

#include "qBFDCore.hpp"
#include "qCOFF.hpp"

namespace qbfd::pe {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
namespace k {
constexpr uint16_t DosMagic = 0x5A4D;     // "MZ"
constexpr uint32_t PeMagic = 0x00004550;  // "PE\0\0"
constexpr uint16_t Opt32 = 0x10B;
constexpr uint16_t Opt64 = 0x20B;

constexpr uint16_t MachI386 = 0x014C;
constexpr uint16_t MachAmd64 = 0x8664;
constexpr uint16_t MachArm = 0x01C0;
constexpr uint16_t MachArmNT = 0x01C4;
constexpr uint16_t MachArm64 = 0xAA64;
constexpr uint16_t MachRiscV32 = 0x5032;
constexpr uint16_t MachRiscV64 = 0x5064;
constexpr uint16_t MachLoongArch64 = 0x6264;

// IMAGE_FILE_HEADER Characteristics
constexpr uint16_t FileExecutable = 0x0002;
constexpr uint16_t FileDll = 0x2000;

// IMAGE_SECTION_HEADER Characteristics
constexpr uint32_t ScnCode = 0x00000020;
constexpr uint32_t ScnInitData = 0x00000040;
constexpr uint32_t ScnUninitData = 0x00000080;
constexpr uint32_t ScnLnkInfo = 0x00000200;
constexpr uint32_t ScnLnkRemove = 0x00000800;
constexpr uint32_t ScnLnkComdat = 0x00001000;
constexpr uint32_t ScnAlignMask = 0x00F00000;
constexpr uint32_t ScnDiscardable = 0x02000000;
constexpr uint32_t ScnExecute = 0x20000000;
constexpr uint32_t ScnRead = 0x40000000;
constexpr uint32_t ScnWrite = 0x80000000;

// Data directory indices
constexpr uint32_t DirExport = 0;
constexpr uint32_t DirImport = 1;
constexpr uint32_t DirBaseReloc = 5;

// COFF symbol storage classes / section numbers
constexpr uint8_t ClassExternal = 2;
constexpr uint8_t ClassStatic = 3;
constexpr uint8_t ClassFile = 103;
constexpr uint8_t ClassWeakExternal = 105;
constexpr int16_t SymUndefined = 0;
constexpr int16_t SymAbsolute = -1;
constexpr int16_t SymDebug = -2;
constexpr uint16_t SymTypeFunction = 0x20;

constexpr size_t FileHeaderSize = 20;
constexpr size_t SectionHeaderSize = 40;
constexpr size_t SymbolSize = 18;
constexpr size_t RelocSize = 10;
constexpr size_t ImportDescSize = 20;
}  // namespace k

// ---------------------------------------------------------------------------
// Little-endian bounds-checked reader
// ---------------------------------------------------------------------------
using Reader = detail::Reader;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
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
      return "pe-i386";
    case k::MachAmd64:
      return "pe-x86-64";
    case k::MachArm:
    case k::MachArmNT:
      return "pe-arm";
    case k::MachArm64:
      return "pe-aarch64";
    case k::MachRiscV32:
      return "pe-riscv32";
    case k::MachRiscV64:
      return "pe-riscv64";
    case k::MachLoongArch64:
      return "pe-loongarch64";
    default:
      return "pe-unknown";
  }
}

inline std::string baseRelocName(uint16_t t) {
  switch (t) {
    case 0:
      return "IMAGE_REL_BASED_ABSOLUTE";
    case 1:
      return "IMAGE_REL_BASED_HIGH";
    case 2:
      return "IMAGE_REL_BASED_LOW";
    case 3:
      return "IMAGE_REL_BASED_HIGHLOW";
    case 4:
      return "IMAGE_REL_BASED_HIGHADJ";
    case 5:
      return "IMAGE_REL_BASED_ARM_MOV32";
    case 7:
      return "IMAGE_REL_BASED_THUMB_MOV32";
    case 10:
      return "IMAGE_REL_BASED_DIR64";
    default:
      return "IMAGE_REL_BASED_" + std::to_string(t);
  }
}

inline std::string coffRelocName(uint16_t machine, uint16_t t) {
  if (machine == k::MachAmd64) {
    static const char* n[] = {
        "ABSOLUTE", "ADDR64",  "ADDR32",  "ADDR32NB", "REL32",   "REL32_1",
        "REL32_2",  "REL32_3", "REL32_4", "REL32_5",  "SECTION", "SECREL",
        "SECREL7",  "TOKEN",   "SREL32",  "PAIR",     "SSPAN32"};
    if (t < std::size(n)) return std::string("IMAGE_REL_AMD64_") + n[t];
  } else if (machine == k::MachI386) {
    switch (t) {
      case 0:
        return "IMAGE_REL_I386_ABSOLUTE";
      case 1:
        return "IMAGE_REL_I386_DIR16";
      case 2:
        return "IMAGE_REL_I386_REL16";
      case 6:
        return "IMAGE_REL_I386_DIR32";
      case 7:
        return "IMAGE_REL_I386_DIR32NB";
      case 9:
        return "IMAGE_REL_I386_SEG12";
      case 10:
        return "IMAGE_REL_I386_SECTION";
      case 11:
        return "IMAGE_REL_I386_SECREL";
      case 12:
        return "IMAGE_REL_I386_TOKEN";
      case 13:
        return "IMAGE_REL_I386_SECREL7";
      case 20:
        return "IMAGE_REL_I386_REL32";
    }
  } else if (machine == k::MachArm64) {
    static const char* n[] = {
        "ABSOLUTE",       "ADDR32",        "ADDR32NB",       "BRANCH26",
        "PAGEBASE_REL21", "REL21",         "PAGEOFFSET_12A", "PAGEOFFSET_12L",
        "SECREL",         "SECREL_LOW12A", "SECREL_HIGH12A", "SECREL_LOW12L",
        "TOKEN",          "SECTION",       "ADDR64",         "BRANCH19",
        "BRANCH14",       "REL32"};
    if (t < std::size(n)) return std::string("IMAGE_REL_ARM64_") + n[t];
  }
  return "IMAGE_REL_" + std::to_string(t);
}

// Native section header, kept for relocation lookup
struct RawSectionHeader {
  uint32_t virtualSize, virtualAddress, rawSize, rawPtr;
  uint32_t relocPtr;
  uint16_t relocCount;
  uint32_t characteristics;
};

struct DataDirectory {
  uint32_t rva = 0, size = 0;
};

// ---------------------------------------------------------------------------
// PEFile
// ---------------------------------------------------------------------------
class PEFile final : public ObjectFile {
 public:
  PEFile(std::span<const uint8_t> data, std::filesystem::path path)
      : ObjectFile(data, std::move(path)), r_{data} {}

  // -- Identity ------------------------------------------------------------
  Format format() const override { return Format::PE; }
  std::string_view targetName() const override {
    return targetNameFor(machine_);
  }
  Arch arch() const override { return archFromMachine(machine_); }
  Endian endian() const override { return Endian::Little; }
  bool is64Bit() const override { return pe32plus_; }
  FileType fileType() const override {
    if (fileChars_ & k::FileDll) return FileType::SharedObject;
    if (fileChars_ & k::FileExecutable) return FileType::Executable;
    return FileType::Relocatable;
  }
  uint64_t entryPoint() const override {
    return entryRva_ ? imageBase_ + entryRva_ : 0;
  }
  uint64_t imageBase() const override { return imageBase_; }

  // -- PE-specific accessors ------------------------------------------------
  uint16_t machine() const { return machine_; }
  uint16_t subsystem() const { return subsystem_; }
  uint16_t dllCharacteristics() const { return dllChars_; }
  uint32_t timestamp() const { return timestamp_; }
  uint64_t sizeOfImage() const { return sizeOfImage_; }
  uint32_t sectionAlignment() const { return sectionAlign_; }
  uint32_t fileAlignment() const { return fileAlign_; }
  const std::vector<DataDirectory>& dataDirectories() const { return dirs_; }
  const std::vector<RawSectionHeader>& rawSections() const { return raw_; }
  std::optional<std::string> exportName() const { return exportDllName_; }

  // On-disk table geometry, for a rewriter that patches the COFF symbol table
  // (qStrip.hpp). `fileHeader` is the offset of IMAGE_FILE_HEADER, whose
  // PointerToSymbolTable and NumberOfSymbols sit 8 and 12 bytes in.
  struct Layout {
    uint64_t fileHeader = 0, sectionTable = 0;
    uint32_t symbolTable = 0, symbolCount = 0, stringTable = 0;
  };
  Layout layout() const { return layout_; }

  uint32_t relocationPointer(uint32_t index) const {
    return index < raw_.size() ? raw_[index].relocPtr : 0;
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

  struct Import {
    std::string dll;
    std::string name;      // empty if by ordinal
    uint16_t ordinal = 0;  // valid if name.empty()
    uint16_t hint = 0;
    uint64_t iatAddress = 0;  // VA of the IAT slot
  };
  const std::vector<Import>& imports() const { return imports_; }

  struct Export {
    std::string name;  // may be empty (ordinal-only)
    uint32_t ordinal = 0;
    uint64_t address = 0;   // VA; 0 if forwarder
    std::string forwarder;  // "DLL.Func" if forwarded
  };
  const std::vector<Export>& exports() const { return exports_; }

  // RVA -> file offset using section table
  std::optional<uint64_t> rvaToOffset(uint32_t rva) const {    for (const auto& h : raw_) {
      uint32_t span = std::max(h.virtualSize, h.rawSize);
      if (rva >= h.virtualAddress && uint64_t(rva) - h.virtualAddress < span) {
        uint32_t delta = rva - h.virtualAddress;
        if (delta >= h.rawSize) return std::nullopt;  // in BSS tail
        return uint64_t(h.rawPtr) + delta;
      }
    }
    // Headers are mapped at RVA 0
    if (rva < sizeOfHeaders_) return rva;
    return std::nullopt;
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
  const std::vector<Symbol>& dynamicSymbols() const override {
    return dynSymbols_;
  }

  // -- Relocations ----------------------------------------------------------
  Expected<std::vector<Relocation>> relocations(
      const Section& requested) const override {
    if (requested.index >= sections_.size())
      return Error{Error::Code::OutOfRange, "bad section index"};
    const auto& target = sections_[requested.index];
    std::vector<Relocation> out;
    if (target.index >= raw_.size())
      return Error{Error::Code::OutOfRange, "bad section index"};
    const auto& h = raw_[target.index];

    // COFF relocations (objects, or images linked with /DEBUG:FULL rarely)
    if (h.relocCount && h.relocPtr) {
      for (uint32_t i = 0; i < h.relocCount; ++i) {
        uint64_t o = uint64_t(h.relocPtr) + i * k::RelocSize;
        if (!r_.has(o, k::RelocSize)) break;
        Relocation rl;
        uint32_t va = r_.u32(o);
        rl.offset = va >= h.virtualAddress ? va - h.virtualAddress : va;
        uint32_t symIdx = r_.u32(o + 4);
        auto it = symIndexMap_.find(symIdx);
        if (it == symIndexMap_.end())
          return Error{Error::Code::Malformed, "invalid PE relocation symbol"};
        rl.symbol = it->second;
        rl.nativeType = r_.u16(o + 8);
        rl.typeName = coffRelocName(machine_, uint16_t(rl.nativeType));
        fillCoffRelocInfo(rl);
        out.push_back(std::move(rl));
      }
    }

    // Base relocations that land inside this section
    for (const auto& br : baseRelocs_) {
      if (br.rva >= h.virtualAddress &&
          br.rva < h.virtualAddress + target.size) {
        Relocation rl;
        rl.offset = br.rva - h.virtualAddress;
        rl.nativeType = br.type;
        rl.typeName = baseRelocName(br.type);
        rl.bitSize = br.type == 10 ? 64 : br.type == 3 ? 32 : 16;
        rl.pcRelative = false;
        out.push_back(std::move(rl));
      }
    }
    return out;
  }

  // -- Segments (synthesised: PE has no program headers) -------------------
  const std::vector<Segment>& segments() const override { return segments_; }

  // -- Dependencies -------------------------------------------------------
  std::vector<std::string> neededLibraries() const override {
    std::vector<std::string> out;
    for (const auto& i : imports_)
      if (std::find(out.begin(), out.end(), i.dll) == out.end())
        out.push_back(i.dll);
    return out;
  }

  // -- Parsing --------------------------------------------------------------
  static bool sniff(std::span<const uint8_t> d, uint16_t wantMachine = 0) {
    Reader r{d};
    if (!r.has(0, 64)) return false;
    if (r.u16(0) != k::DosMagic) return false;
    uint32_t lfanew = r.u32(0x3C);
    if (!r.has(lfanew, 4 + k::FileHeaderSize)) return false;
    if (r.u32(lfanew) != k::PeMagic) return false;
    if (wantMachine && r.u16(uint64_t(lfanew) + 4) != wantMachine) return false;
    return true;
  }

  static Expected<std::unique_ptr<ObjectFile>> open(
      std::span<const uint8_t> d, const std::filesystem::path& p) {
    try {
      auto f = std::make_unique<PEFile>(d, p);
      if (Error e = f->parse()) return e;
      return std::unique_ptr<ObjectFile>(std::move(f));
    } catch (const detail::ParseFailure& e) {
      return e.error;
    }
  }

 private:
  struct BaseReloc {
    uint32_t rva;
    uint16_t type;
  };

  Error parse() {
    // DOS header + PE signature
    if (r_.u16(0) != k::DosMagic)
      return {Error::Code::Malformed, "missing MZ signature"};
    uint32_t lfanew = r_.u32(0x3C);
    if (!r_.has(lfanew, 4 + k::FileHeaderSize) || r_.u32(lfanew) != k::PeMagic)
      return {Error::Code::Malformed, "missing PE signature"};

    // IMAGE_FILE_HEADER
    uint64_t fh = uint64_t(lfanew) + 4;
    machine_ = r_.u16(fh + 0);
    if (!coff::knownMachine(machine_))
      return {Error::Code::Unsupported, "unknown PE machine"};
    uint16_t nSections = r_.u16(fh + 2);
    timestamp_ = r_.u32(fh + 4);
    uint32_t symPtr = r_.u32(fh + 8);
    uint32_t nSyms = r_.u32(fh + 12);
    uint16_t optSize = r_.u16(fh + 16);
    fileChars_ = r_.u16(fh + 18);
    layout_ = {fh, 0, symPtr, nSyms, 0};

    // IMAGE_OPTIONAL_HEADER
    uint64_t oh = fh + k::FileHeaderSize;
    if (optSize < 2 || !r_.has(oh, optSize))
      return {Error::Code::Malformed, "optional header truncated"};
    uint16_t magic = r_.u16(oh);
    if (magic == k::Opt64)
      pe32plus_ = true;
    else if (magic != k::Opt32)
      return {Error::Code::Unsupported, "unknown optional header magic"};

    if (optSize < (pe32plus_ ? 112 : 96))
      detail::malformed("optional header too small");
    entryRva_ = r_.u32(oh + 16);
    if (pe32plus_) {
      imageBase_ = r_.u64(oh + 24);
    } else {
      imageBase_ = r_.u32(oh + 28);
    }
    detail::checkedAdd(imageBase_, entryRva_);
    sectionAlign_ = r_.u32(oh + 32);
    fileAlign_ = r_.u32(oh + 36);
    sizeOfImage_ = r_.u32(oh + 56);
    sizeOfHeaders_ = r_.u32(oh + 60);
    subsystem_ = r_.u16(oh + 68);
    dllChars_ = r_.u16(oh + 70);

    uint64_t nDirsOff = pe32plus_ ? oh + 108 : oh + 92;
    uint64_t dirsOff = pe32plus_ ? oh + 112 : oh + 96;
    uint32_t nDirs = std::min<uint32_t>(r_.u32(nDirsOff), 16);
    if (uint64_t(nDirs) * 8 > oh + optSize - dirsOff)
      detail::malformed("data directory table truncated");
    for (uint32_t i = 0; i < nDirs; ++i)
      dirs_.push_back({r_.u32(dirsOff + i * 8), r_.u32(dirsOff + i * 8 + 4)});

    // Section table
    uint64_t sh = oh + optSize;
    layout_.sectionTable = sh;
    if (!r_.has(sh, uint64_t(nSections) * k::SectionHeaderSize))
      return {Error::Code::Malformed, "section table truncated"};

    // String table (follows symbol table) for long section names
    uint64_t strTab =
        symPtr ? uint64_t(symPtr) + uint64_t(nSyms) * k::SymbolSize : 0;
    layout_.stringTable = uint32_t(strTab);

    for (uint16_t i = 0; i < nSections; ++i) {
      uint64_t o = sh + i * k::SectionHeaderSize;
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
      s.vma = detail::checkedAdd(imageBase_, h.virtualAddress);
      s.lma = s.vma;
      s.size = h.virtualSize ? h.virtualSize : h.rawSize;
      s.fileOffset = h.rawPtr;
      s.fileSize = std::min<uint64_t>(h.rawSize, s.size ? s.size : h.rawSize);
      if (h.rawSize && h.virtualSize == 0) s.fileSize = h.rawSize;
      s.nativeFlags = h.characteristics;
      s.flags = mapSectionFlags(h.characteristics, h.rawPtr != 0);
      uint32_t alignBits = (h.characteristics & k::ScnAlignMask) >> 20;
      s.alignment = alignBits ? (1u << (alignBits - 1)) : sectionAlign_;
      r_.require(h.rawPtr, h.rawSize);
      r_.require(h.relocPtr, uint64_t(h.relocCount) * k::RelocSize);
      sections_.push_back(std::move(s));

      Segment g;
      g.name = sections_.back().name;
      g.vaddr = g.paddr = sections_.back().vma;
      g.fileOffset = h.rawPtr;
      g.fileSize = sections_.back().fileSize;
      g.memSize = sections_.back().size;
      g.alignment = sectionAlign_;
      if (h.characteristics & k::ScnRead) g.flags |= seg::Read;
      if (h.characteristics & k::ScnWrite) g.flags |= seg::Write;
      if (h.characteristics & k::ScnExecute) g.flags |= seg::Exec;
      g.sections.push_back(i);
      segments_.push_back(std::move(g));
    }

    if (nSyms && !symPtr) detail::malformed("missing COFF symbol table");
    if (symPtr) {
      r_.require(symPtr, uint64_t(nSyms) * k::SymbolSize);
      if (r_.u32(strTab) < 4) detail::malformed("invalid string table size");
      r_.require(strTab, r_.u32(strTab));
    }
    parseCoffSymbols(symPtr, nSyms, strTab);
    parseExports();
    parseImports();
    parseBaseRelocs();
    for (const auto& section : sections_) {
      auto result = relocations(section);
      if (!result) return result.error();
    }
    return {};
  }

  std::string sectionName(uint64_t hdrOff, uint64_t strTab) const {
    char raw[9] = {};
    for (int j = 0; j < 8; ++j) raw[j] = char(r_.u8(hdrOff + j));
    std::string n(raw, std::char_traits<char>::length(raw));
    // "/123" => offset into string table
    if (n.size() > 1 && n[0] == '/' && strTab) {
      uint64_t off = 0;
      for (size_t j = 1; j < n.size(); ++j) {
        if (n[j] < '0' || n[j] > '9') return n;
        off = off * 10 + (n[j] - '0');
      }
      return r_.stringAt(strTab, r_.u32(strTab), off);
    }
    return n;
  }

  static uint32_t mapSectionFlags(uint32_t c, bool hasRaw) {
    uint32_t f = sec::Alloc;
    if (c & k::ScnCode) f |= sec::Code;
    if (c & k::ScnInitData) f |= sec::Data;
    if (c & k::ScnUninitData || !hasRaw)
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

  void parseCoffSymbols(uint32_t symPtr, uint32_t nSyms, uint64_t strTab) {
    if (!symPtr || !nSyms) return;
    if (!r_.has(symPtr, uint64_t(nSyms) * k::SymbolSize)) return;

    for (uint32_t i = 0; i < nSyms; ++i) {
      uint64_t o = uint64_t(symPtr) + i * k::SymbolSize;
      uint8_t nAux = r_.u8(o + 17);
      if (nAux >= nSyms - i)
        detail::malformed("COFF auxiliary records exceed table");

      Symbol s;
      s.index = uint32_t(symbols_.size());
      symIndexMap_[i] = s.index;

      if (r_.u32(o) == 0) {
        s.name = r_.stringAt(strTab, r_.u32(strTab), r_.u32(o + 4));
      } else {
        char raw[9] = {};
        for (int j = 0; j < 8; ++j) raw[j] = char(r_.u8(o + j));
        s.name.assign(raw, std::char_traits<char>::length(raw));
      }
      uint32_t value = r_.u32(o + 8);
      int16_t scn = int16_t(r_.u16(o + 12));
      uint16_t type = r_.u16(o + 14);
      uint8_t cls = r_.u8(o + 16);
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
      if (cls == k::ClassFile) {
        s.kind = SymbolKind::File;
        s.flags |= sym::Debug;
      } else if ((type >> 4) == 2) {  // DTYPE_FUNCTION
        s.kind = SymbolKind::Function;
      } else if (cls == k::ClassStatic && value == 0 && scn > 0 &&
                 s.name.size() && s.name[0] == '.') {
        s.kind = SymbolKind::Section;
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
        s.value = 0;
      } else if (scn == k::SymAbsolute) {
        s.flags |= sym::Absolute;
        s.value = value;
      } else if (scn == k::SymDebug) {
        s.flags |= sym::Debug;
        s.value = value;
      } else if (scn > 0 && uint32_t(scn - 1) < sections_.size()) {
        s.section = uint32_t(scn - 1);
        // In images COFF symbol values are RVAs; in objects, section offsets.
        s.value = detail::checkedAdd(sections_[scn - 1].vma, value);
      } else {
        s.value = value;
      }
      symbols_.push_back(std::move(s));
      i += nAux;
    }
  }

  uint64_t mapped(uint32_t rva, uint64_t size) const {
    auto off = rvaToOffset(rva);
    if (!off) detail::malformed("unmapped PE RVA");
    uint64_t available = rva < sizeOfHeaders_ ? sizeOfHeaders_ - rva : 0;
    for (const auto& h : raw_)
      if (rva >= h.virtualAddress &&
          uint64_t(rva) - h.virtualAddress < h.rawSize)
        available = h.rawSize - (uint64_t(rva) - h.virtualAddress);
    if (size > available) detail::malformed("PE range crosses mapped raw data");
    r_.require(*off, size);
    return *off;
  }
  std::string rvaString(uint32_t rva) const {
    uint64_t off = mapped(rva, 1), available = 0;
    if (rva < sizeOfHeaders_) available = sizeOfHeaders_ - rva;
    for (const auto& h : raw_)
      if (rva >= h.virtualAddress &&
          uint64_t(rva) - h.virtualAddress < h.rawSize)
        available = h.rawSize - (uint64_t(rva) - h.virtualAddress);
    return r_.cstr(off, available);
  }
  static uint32_t addRva(uint32_t rva, uint64_t delta) {
    if (delta > UINT32_MAX - rva) detail::malformed("PE RVA overflow");
    return rva + uint32_t(delta);
  }
  void parseImports() {
    if (dirs_.size() <= k::DirImport || !dirs_[k::DirImport].rva) return;
    auto dir = dirs_[k::DirImport];
    uint64_t base = mapped(dir.rva, dir.size);
    bool terminated = false;
    for (uint64_t i = 0; i + 20 <= dir.size; i += 20) {
      auto o = base + i;
      uint32_t lookup = r_.u32(o), name = r_.u32(o + 12), iat = r_.u32(o + 16);
      if (!lookup && !name && !iat && !r_.u32(o + 4) && !r_.u32(o + 8)) {
        terminated = true;
        break;
      }
      if (!name || !iat) detail::malformed("invalid import descriptor");
      auto dll = rvaString(name);
      if (!lookup) lookup = iat;
      unsigned width = pe32plus_ ? 8 : 4;
      for (uint64_t j = 0;; j += width) {
        auto thunk = r_.integer(mapped(addRva(lookup, j), width), width);
        if (!thunk) break;
        Import im;
        im.dll = dll;
        im.iatAddress = detail::checkedAdd(imageBase_, addRva(iat, j));
        mapped(addRva(iat, j), width);
        uint64_t ordinalBit = uint64_t(1) << (width * 8 - 1);
        if (thunk & ordinalBit)
          im.ordinal = uint16_t(thunk);
        else {
          if (thunk > UINT32_MAX) detail::malformed("import name RVA overflow");
          im.hint = r_.u16(mapped(uint32_t(thunk), 2));
          im.name = rvaString(addRva(uint32_t(thunk), 2));
        }
        Symbol s;
        s.index = uint32_t(dynSymbols_.size());
        s.name =
            im.name.empty() ? dll + "!#" + std::to_string(im.ordinal) : im.name;
        s.value = im.iatAddress;
        s.flags = sym::Imported | sym::Undefined;
        s.binding = SymbolBinding::Global;
        dynSymbols_.push_back(std::move(s));
        imports_.push_back(std::move(im));
      }
    }
    if (!terminated) detail::malformed("unterminated import descriptors");
  }
  void parseExports() {
    if (dirs_.size() <= k::DirExport || !dirs_[k::DirExport].rva) return;
    auto dir = dirs_[k::DirExport];
    if (dir.size < 40) detail::malformed("short export directory");
    uint64_t o = mapped(dir.rva, dir.size);
    if (auto name = r_.u32(o + 12)) exportDllName_ = rvaString(name);
    uint32_t ordinalBase = r_.u32(o + 16), count = r_.u32(o + 20),
             names = r_.u32(o + 24);
    uint64_t addresses =
        count ? mapped(r_.u32(o + 28), uint64_t(count) * 4) : 0;
    uint64_t nameTable =
        names ? mapped(r_.u32(o + 32), uint64_t(names) * 4) : 0;
    uint64_t ordinals = names ? mapped(r_.u32(o + 36), uint64_t(names) * 2) : 0;
    std::unordered_multimap<uint32_t, std::string> byIndex;
    for (uint32_t i = 0; i < names; ++i) {
      uint16_t idx = r_.u16(ordinals + uint64_t(i) * 2);
      if (idx >= count)
        detail::malformed("export ordinal outside address table");
      byIndex.emplace(idx, rvaString(r_.u32(nameTable + uint64_t(i) * 4)));
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t rva = r_.u32(addresses + uint64_t(i) * 4);
      if (!rva) continue;
      Export ex;
      ex.ordinal = addRva(ordinalBase, i);
      if (rva >= dir.rva && uint64_t(rva) - dir.rva < dir.size)
        ex.forwarder =
            r_.cstr(mapped(rva, 1), dir.size - (uint64_t(rva) - dir.rva));
      else
        ex.address = detail::checkedAdd(imageBase_, rva);
      auto emit = [&](std::string name) {
        ex.name = std::move(name);
        exports_.push_back(ex);
        Symbol s;
        s.index = uint32_t(dynSymbols_.size());
        s.name = ex.name.empty() ? "#" + std::to_string(ex.ordinal) : ex.name;
        s.value = ex.address;
        s.flags = sym::Exported;
        s.binding = SymbolBinding::Global;
        if (!ex.forwarder.empty()) s.kind = SymbolKind::Indirect;
        dynSymbols_.push_back(std::move(s));
      };
      auto [first, last] = byIndex.equal_range(i);
      if (first == last)
        emit({});
      else
        for (; first != last; ++first) emit(first->second);
    }
  }
  void parseBaseRelocs() {
    if (dirs_.size() <= k::DirBaseReloc || !dirs_[k::DirBaseReloc].rva) return;
    auto dir = dirs_[k::DirBaseReloc];
    uint64_t base = mapped(dir.rva, dir.size);
    for (uint64_t i = 0; i < dir.size;) {
      if (dir.size - i < 8) detail::malformed("short base relocation block");
      uint32_t page = r_.u32(base + i), size = r_.u32(base + i + 4);
      if (size < 8 || size % 2 || size > dir.size - i)
        detail::malformed("invalid base relocation block size");
      for (uint64_t j = 8; j < size; j += 2) {
        uint16_t entry = r_.u16(base + i + j), type = entry >> 12;
        if (!type) continue;
        baseRelocs_.push_back({addRva(page, entry & 0xfff), type});
        if (type == 4) {
          if (j + 4 > size) detail::malformed("missing HIGHADJ pair");
          j += 2;
        }
      }
      i += size;
    }
  }
  void fillCoffRelocInfo(Relocation& r) const {
    auto info = coff::relocInfo(machine_, uint16_t(r.nativeType));
    r.bitSize = info.bits;
    r.pcRelative = info.pcrel;
  }
  uint64_t recordOffset(uint32_t record) const {
    return uint64_t(layout_.symbolTable) + uint64_t(record) * k::SymbolSize;
  }

 public:
  // Defined here rather than at the declarations above: a member function
  // cannot be both declared and defined inside the same class, and a body may
  // only use members already declared.
  std::string rawSymbolName(uint32_t record) const {
    uint64_t o = recordOffset(record);
    if (!r_.has(o, 8)) return {};
    if (r_.u32(o)) return r_.fixed(o, 8);
    uint32_t at = r_.u32(o + 4);
    if (!layout_.stringTable) return {};
    try {
      return r_.stringAt(layout_.stringTable, r_.u32(layout_.stringTable), at);
    } catch (const detail::ParseFailure&) {
      return {};
    }
  }
  uint8_t rawSymbolClass(uint32_t record) const {
    uint64_t o = recordOffset(record);
    return r_.has(o, 17) ? r_.u8(o + 16) : 0;
  }
  int32_t rawSymbolSection(uint32_t record) const {
    uint64_t o = recordOffset(record);
    return r_.has(o, 14) ? int32_t(int16_t(r_.u16(o + 12))) : 0;
  }
  uint32_t rawSymbolAuxCount(uint32_t record) const {
    uint64_t o = recordOffset(record);
    return r_.has(o, 18) ? uint32_t(r_.u8(o + 17)) : 0;
  }

 private:
  Reader r_;
  uint16_t machine_ = 0, subsystem_ = 0, dllChars_ = 0, fileChars_ = 0;
  uint32_t timestamp_ = 0, entryRva_ = 0, sizeOfImage_ = 0, sizeOfHeaders_ = 0;
  uint32_t sectionAlign_ = 0, fileAlign_ = 0;
  uint64_t imageBase_ = 0;
  bool pe32plus_ = false;
  std::vector<RawSectionHeader> raw_;
  std::vector<DataDirectory> dirs_;
  std::vector<Section> sections_;
  std::vector<Symbol> symbols_, dynSymbols_;
  std::vector<Segment> segments_;
  std::vector<Import> imports_;
  std::vector<Export> exports_;
  std::vector<BaseReloc> baseRelocs_;
  std::optional<std::string> exportDllName_;
  std::unordered_map<uint32_t, uint32_t> symIndexMap_;
  Layout layout_;
};
inline void registerTargets(Registry& registry) {
  for (auto machine : {k::MachI386, k::MachAmd64, k::MachArm, k::MachArm64,
                       k::MachRiscV32, k::MachRiscV64, k::MachLoongArch64})
    registry.add({std::string(targetNameFor(machine)), Format::PE,
                  [machine](auto bytes) {
                    return PEFile::sniff(bytes, machine) ||
                           (machine == k::MachArm &&
                            PEFile::sniff(bytes, k::MachArmNT));
                  },
                  PEFile::open});
}
namespace query {
using namespace qbfd::query;
inline auto imports() {
  return ::dsl::pipe([](const PEFile& f) { return f.imports(); });
}
inline auto exports() {
  return ::dsl::pipe([](const PEFile& f) { return f.exports(); });
}
}  // namespace query
}  // namespace qbfd::pe
