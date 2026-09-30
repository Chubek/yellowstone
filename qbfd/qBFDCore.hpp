// qBFD.hpp - uniform interface over qELF / qPE / qCOFF / qMachO
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "../common/qdsl.hpp"

namespace qbfd {

// ---------------------------------------------------------------------------
// Error handling
// ---------------------------------------------------------------------------
struct Error {
  enum class Code {
    None,
    FileNotFound,
    IoError,
    UnknownFormat,
    Ambiguous,    // more than one target accepted the file
    Malformed,    // header/table inconsistent
    Unsupported,  // valid, but feature not implemented
    OutOfRange,   // section/symbol index bad
  };
  Code code = Code::None;
  std::string message;

  explicit operator bool() const { return code != Code::None; }
};

template <class T>
class Expected {
 public:
  Expected(T v) : v_(std::move(v)) {}
  Expected(Error e) : v_(std::move(e)) {}

  bool ok() const { return std::holds_alternative<T>(v_); }
  explicit operator bool() const { return ok(); }

  T& value() { return std::get<T>(v_); }
  const T& value() const { return std::get<T>(v_); }
  T& operator*() { return value(); }
  const T& operator*() const { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const Error& error() const { return std::get<Error>(v_); }

 private:
  std::variant<T, Error> v_;
};

// ---------------------------------------------------------------------------
// Generic enums (the "canonical form" every backend maps onto)
// ---------------------------------------------------------------------------
enum class Format : uint8_t { Unknown, ELF, PE, COFF, MachO };

enum class Arch : uint16_t {
  Unknown,
  X86,
  X86_64,
  ARM,
  AArch64,
  RISCV32,
  RISCV64,
  PowerPC,
  PowerPC64,
  MIPS,
  MIPS64,
  SPARC,
  SPARC64,
  S390X,
  LoongArch64,
  WebAssembly,
};

enum class Endian : uint8_t { Little, Big };

enum class FileType : uint8_t {
  Unknown,
  Relocatable,   // ET_REL, COFF .obj, MH_OBJECT
  Executable,    // ET_EXEC, PE image, MH_EXECUTE
  SharedObject,  // ET_DYN, PE DLL, MH_DYLIB / MH_BUNDLE
  Core,          // ET_CORE, MH_CORE
};

// ---------------------------------------------------------------------------
// Sections
// ---------------------------------------------------------------------------
namespace sec {
enum Flags : uint32_t {
  None = 0,
  Alloc = 1u << 0,  // occupies memory at run time
  Load = 1u << 1,   // has contents in the file
  Code = 1u << 2,
  Data = 1u << 3,
  ReadOnly = 1u << 4,
  Writable = 1u << 5,
  Exec = 1u << 6,
  Bss = 1u << 7,  // zero-fill, no file contents
  Debug = 1u << 8,
  Reloc = 1u << 9,  // contains relocations
  Strings = 1u << 10,
  Note = 1u << 11,
  Tls = 1u << 12,
  Discard = 1u << 13,  // linker may drop (e.g. .comment, COMDAT)
};
}  // namespace sec

struct Section {
  uint32_t index = 0;  // position in the backend's section table
  std::string name;
  uint64_t vma = 0;            // virtual address
  uint64_t lma = 0;            // load address (== vma for most formats)
  uint64_t size = 0;           // size in memory
  uint64_t fileOffset = 0;     // 0 for BSS
  uint64_t fileSize = 0;       // bytes present in the file
  uint64_t alignment = 1;      // bytes, power of two
  uint32_t flags = sec::None;  // sec::Flags
  uint64_t nativeFlags = 0;    // raw sh_flags / Characteristics / S_* flags
  uint32_t nativeType = 0;     // raw sh_type / Mach-O section type, etc.
};

// ---------------------------------------------------------------------------
// Symbols
// ---------------------------------------------------------------------------
enum class SymbolBinding : uint8_t { Local, Global, Weak, Unique };
enum class SymbolKind : uint8_t {
  None,
  Object,
  Function,
  Section,
  File,
  Common,
  Tls,
  Indirect,  // IFUNC / stub
};

namespace sym {
enum Flags : uint32_t {
  None = 0,
  Undefined = 1u << 0,  // defined elsewhere
  Absolute = 1u << 1,   // not relative to any section
  Hidden = 1u << 2,     // STV_HIDDEN / private extern
  Exported = 1u << 3,   // PE export / dyld exported
  Imported = 1u << 4,   // PE import / undefined dynamic
  Synthetic = 1u << 5,  // created by qBFD (e.g. PLT stub names)
  Debug = 1u << 6,
};
}  // namespace sym

struct Symbol {
  uint32_t index = 0;
  std::string name;
  uint64_t value = 0;  // address or section offset
  uint64_t size = 0;
  std::optional<uint32_t> section;  // Section::index, empty if undef/abs
  SymbolBinding binding = SymbolBinding::Local;
  SymbolKind kind = SymbolKind::None;
  uint32_t flags = sym::None;  // sym::Flags
  uint32_t nativeType = 0;     // raw st_info / storage class / n_type
};

// ---------------------------------------------------------------------------
// Relocations
// ---------------------------------------------------------------------------
struct Relocation {
  uint64_t offset = 0;             // offset inside the owning section
  std::optional<uint32_t> symbol;  // Symbol::index
  int64_t addend = 0;              // explicit (RELA) or decoded from data
  uint32_t nativeType = 0;         // R_*, IMAGE_REL_*, *_RELOC_*
  std::string typeName;            // human-readable, e.g. "R_X86_64_PC32"
  uint8_t bitSize = 0;             // 8/16/32/64; 0 if unknown
  bool pcRelative = false;
  std::optional<uint32_t> targetSection;  // Mach-O local relocation section
  bool scattered = false;
  uint64_t nativeValue = 0;    // Mach-O scattered relocation value
  bool dynamicSymbol = false;  // symbol indexes dynamicSymbols() when true
  bool hasAddend = false;      // false when an implicit encoding is not decoded
};

// ---------------------------------------------------------------------------
// Segments (program headers / PE image sections / LC_SEGMENT)
// ---------------------------------------------------------------------------
namespace seg {
enum Flags : uint32_t {
  None = 0,
  Read = 1u << 0,
  Write = 1u << 1,
  Exec = 1u << 2
};
}

struct Segment {
  std::string name;         // Mach-O only; empty otherwise
  uint32_t nativeType = 0;  // PT_* etc.
  uint64_t vaddr = 0;
  uint64_t paddr = 0;
  uint64_t fileOffset = 0;
  uint64_t fileSize = 0;
  uint64_t memSize = 0;
  uint64_t alignment = 1;
  uint32_t flags = seg::None;      // seg::Flags
  std::vector<uint32_t> sections;  // Section::index contained in segment
};

// ---------------------------------------------------------------------------
// ObjectFile - the abstract "bfd" every backend implements
// ---------------------------------------------------------------------------
class Buffer;

class ObjectFile {
 public:
  virtual ~ObjectFile() = default;
  ObjectFile(const ObjectFile&) = delete;
  ObjectFile& operator=(const ObjectFile&) = delete;
  ObjectFile(ObjectFile&&) = delete;
  ObjectFile& operator=(ObjectFile&&) = delete;
  // Front-end opens retain storage even when the object is moved out of Opened.
  void retainBuffer(std::shared_ptr<const Buffer> owner) {
    owner_ = std::move(owner);
  }

  // Identity
  virtual Format format() const = 0;
  virtual std::string_view targetName()
      const = 0;  // "elf64-x86-64", "pe-x86-64"
  virtual Arch arch() const = 0;
  virtual Endian endian() const = 0;
  virtual bool is64Bit() const = 0;
  virtual FileType fileType() const = 0;
  virtual uint64_t entryPoint() const = 0;
  virtual uint64_t imageBase() const { return 0; }

  // Raw bytes of the whole file
  std::span<const uint8_t> data() const { return data_; }
  const std::filesystem::path& path() const { return path_; }

  // Sections
  virtual const std::vector<Section>& sections() const = 0;
  virtual Expected<std::span<const uint8_t>> sectionContents(
      const Section& s) const = 0;
  const Section* findSection(std::string_view name) const {
    for (const auto& s : sections())
      if (s.name == name) return &s;
    return nullptr;
  }
  const Section* sectionForAddress(uint64_t vma) const {
    for (const auto& s : sections())
      if ((s.flags & sec::Alloc) && vma >= s.vma && vma - s.vma < s.size)
        return &s;
    return nullptr;
  }

  // Symbols: static table plus (if present) dynamic/export table
  virtual const std::vector<Symbol>& symbols() const = 0;
  virtual const std::vector<Symbol>& dynamicSymbols() const {
    static const std::vector<Symbol> empty;
    return empty;
  }
  const Symbol* findSymbol(std::string_view name) const {
    for (const auto& s : symbols())
      if (s.name == name) return &s;
    return nullptr;
  }

  // Relocations that apply to a given section
  virtual Expected<std::vector<Relocation>> relocations(
      const Section& target) const = 0;

  // Segments / program headers (may be empty for relocatable objects)
  virtual const std::vector<Segment>& segments() const = 0;

  // Shared-library dependencies (DT_NEEDED, PE imports, LC_LOAD_DYLIB)
  virtual std::vector<std::string> neededLibraries() const { return {}; }

  // Address <-> file offset helpers built on sections()
  std::optional<uint64_t> vmaToFileOffset(uint64_t vma) const {
    const Section* s = sectionForAddress(vma);
    if (!s || (s->flags & sec::Bss) || vma - s->vma >= s->fileSize ||
        s->fileOffset > data_.size() ||
        vma - s->vma >= data_.size() - s->fileOffset)
      return std::nullopt;
    return s->fileOffset + (vma - s->vma);
  }

 protected:
  ObjectFile(std::span<const uint8_t> data, std::filesystem::path path)
      : data_(data), path_(std::move(path)) {}

  std::span<const uint8_t> data_;
  std::filesystem::path path_;
  std::shared_ptr<const Buffer> owner_;
};

// ---------------------------------------------------------------------------
// Archives (.a / .lib) - a container of ObjectFiles
//
// `ar` has no single standard. Three layouts are in circulation and all three
// are read here:
//   SVR4/GNU  `!<arch>\n` + 60-byte headers, names <= 15 bytes terminated by
//             '/', longer names in a `//` long-name table referenced as `/N`.
//             Optional `/` (armap) and `/SYM64/` symbol indexes first.
//   BSD       `!<arch>\n` + 60-byte headers, names <= 16 bytes right-padded
//             with spaces, longer/awkward names as `#1/<len>` with the name
//             stored at the head of the member data. Optional `__.SYMDEF`
//             (or `__.SYMDEF_64`, `__.SYMDEF SORTED`) symbol index first.
//   thin      `!<thin>\n`, same headers as SVR4/GNU, but the member payload
//             lives in a separate file and `size` is always 0.
//
// A thin archive borrows bytes it does not own, so `memberData` for a thin
// member may return an empty span; `readMember` materialises it instead.
// ---------------------------------------------------------------------------

// Which `ar` convention a container follows. Detection is from the container
// magic and the leading member headers, never from the member payloads.
enum class ArchiveFlavor : uint8_t {
  Unknown,
  Svr4,    // `!<arch>\n`, '/' and '//' conventions
  Bsd,     // `!<arch>\n`, '#1/<len>' extended names, `__.SYMDEF` index
  Thin,    // `!<thin>\n`, payloads live in separate files
  Windows, // `!<arch>\n` with two leading '/' members: a COFF import library
};

class Archive {
 public:
  struct Member {
    std::string name;
    uint64_t headerOffset = 0;  // offset of this member's 60-byte header
    uint64_t offset = 0;        // offset of member data in the archive
    uint64_t size = 0;          // payload length, excluding any pad byte
    uint32_t mode = 0;          // st_mode from the header
    uint64_t mtime = 0;
    uint32_t uid = 0;
    uint32_t gid = 0;
    bool special = false;  // armap, symbol table or long-name table entry
  };

  // One archive-wide symbol -> member mapping, as stored in an armap.
  struct IndexEntry {
    std::string symbol;
    uint64_t headerOffset = 0;  // header of the defining member
  };

  virtual ~Archive() = default;
  virtual const std::vector<Member>& members() const = 0;
  virtual std::span<const uint8_t> memberData(const Member& m) const = 0;

  // Materialise a member. Only differs from memberData() for thin archives,
  // where the payload is read from `m.name` relative to the archive's
  // directory.
  virtual Expected<std::span<const uint8_t>> readMember(
      const Member& m) const = 0;

  virtual Expected<std::unique_ptr<ObjectFile>> openMember(
      const Member& m) const = 0;

  // Header offset, for reporting and for index lookups.
  const Member* findMember(std::string_view name) const {
    for (const auto& m : members())
      if (m.name == name) return &m;
    return nullptr;
  }
  const Member* memberAt(uint64_t headerOffset) const {
    for (const auto& m : members())
      if (m.headerOffset == headerOffset) return &m;
    return nullptr;
  }
  // Archive-wide symbol -> defining member table, empty when the container has
  // no index.
  virtual const std::vector<IndexEntry>& symbolIndex() const {
    static const std::vector<IndexEntry> empty;
    return empty;
  }
  // Locates a member carrying a candidate global definition of `symbol`, as
  // a linker would when resolving an archive reference.
  const Member* definingMember(std::string_view symbol) const {
    for (const auto& e : symbolIndex())
      if (e.symbol == symbol)
        if (const Member* m = memberAt(e.headerOffset)) return m;
    return nullptr;
  }
};

// ---------------------------------------------------------------------------
// Target registry - the backend table (like bfd_target_vector)
// ---------------------------------------------------------------------------
struct Target {
  std::string name;  // "elf64-x86-64"
  Format format;

  // Cheap check: does this target want this file? Should only look at magic
  // and basic header fields. Must not allocate.
  std::function<bool(std::span<const uint8_t>)> sniff;

  // Full parse. Called only if sniff() returned true (or openAs() forced it).
  std::function<Expected<std::unique_ptr<ObjectFile>>(
      std::span<const uint8_t>, const std::filesystem::path&)>
      open;
};

class Registry {
 public:
  static Registry& instance() {
    static Registry r;
    return r;
  }

  void add(Target t) {
    if (!find(t.name)) targets_.push_back(std::move(t));
  }
  const std::vector<Target>& targets() const { return targets_; }

  const Target* find(std::string_view name) const {
    for (const auto& t : targets_)
      if (t.name == name) return &t;
    return nullptr;
  }

  std::vector<const Target*> match(std::span<const uint8_t> data) const {
    std::vector<const Target*> out;
    for (const auto& t : targets_)
      if (t.sniff(data)) out.push_back(&t);
    return out;
  }

 private:
  std::vector<Target> targets_;
};

// ---------------------------------------------------------------------------
// Buffer ownership: keep the bytes alive as long as the ObjectFile lives
// ---------------------------------------------------------------------------
class Buffer {
 public:
  static Expected<std::shared_ptr<Buffer>> fromFile(
      const std::filesystem::path& p);
  static std::shared_ptr<Buffer> fromBytes(std::vector<uint8_t> bytes) {
    auto b = std::make_shared<Buffer>();
    b->bytes_ = std::move(bytes);
    return b;
  }
  std::span<const uint8_t> span() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

// ---------------------------------------------------------------------------
// Public entry points (bfd_openr + bfd_check_format in one step)
// ---------------------------------------------------------------------------
struct Opened {
  std::shared_ptr<Buffer> buffer;      // owns the bytes
  std::unique_ptr<ObjectFile> object;  // views into buffer
};

Expected<Opened> open(const std::filesystem::path& path);
Expected<Opened> open(std::vector<uint8_t> bytes,
                      const std::filesystem::path& label = {});
Expected<Opened> openAs(const std::filesystem::path& path,
                        std::string_view targetName);

// ---------------------------------------------------------------------------
// Archive entry points
// ---------------------------------------------------------------------------
struct OpenedArchive {
  std::shared_ptr<Buffer> buffer;  // owns the container bytes
  std::unique_ptr<Archive> archive;  // views into buffer
};

// Parses a container. Thin members are resolved relative to the archive's own
// directory and cached in `OpenedArchive::thin`, so the returned object stays
// valid for as long as the OpenedArchive does.
Expected<OpenedArchive> openArchive(const std::filesystem::path& path);
Expected<OpenedArchive> openArchive(std::vector<uint8_t> bytes,
                                    const std::filesystem::path& label = {});

struct ArchiveEntry {
  std::string name;
  std::vector<uint8_t> data;
  uint32_t mode = 0100644;
  uint64_t mtime = 0;
  uint32_t uid = 0;
  uint32_t gid = 0;
};

// Builds a container. Entries are emitted in the given order.
struct ArchiveOptions {
  ArchiveFlavor flavor = ArchiveFlavor::Svr4;
  // Zeroes uid/gid/mtime and forces mode 0644, matching `ar D` and `rcsD`, so
  // identical inputs produce byte-identical archives.
  bool deterministic = true;
  // Emit a leading symbol index built from the members' own symbol tables.
  bool index = false;
  // Use the 8-byte armap (`/SYM64/`) instead of the 32-bit one.
  bool wideIndex = false;
};
Expected<std::vector<uint8_t>> buildArchive(
    const std::vector<ArchiveEntry>& entries, const ArchiveOptions& options);

// Reads the payload for an entry, honouring its mode and mtime. Used by the
// `r` and `q` operations of `qobjar`.
Expected<ArchiveEntry> archiveEntryFromFile(const std::filesystem::path& path);

// The global symbols an object defines, as a linker would index them.
Expected<std::vector<std::string>> archiveExportedSymbols(
    const ArchiveEntry& entry);

// Format detection without a full parse
Format detectFormat(std::span<const uint8_t> data);
// Classifies a container. Format::Unknown is returned for anything that is
// not an archive; use it to decide whether a path should be handed to
// `openArchive` or to `open`.
bool isArchive(std::span<const uint8_t> data);
// Human-readable archive container flavour, for `qobjar` and diagnostics.
std::string_view toString(ArchiveFlavor flavor);

// Human-readable names
std::string_view toString(Format f);
std::string_view toString(Arch a);
std::string_view toString(FileType t);

// Register all compiled-in backends. Idempotent.
void registerBuiltinTargets();

}  // namespace qbfd

namespace qbfd::detail {
// Integer reads never use unaligned casts. A failed read cannot become a valid
// zero.
struct ParseFailure {
  Error error;
};
[[noreturn]] inline void malformed(std::string message) {
  throw ParseFailure{{Error::Code::Malformed, std::move(message)}};
}
struct Reader {
  std::span<const uint8_t> d;
  Endian order = Endian::Little;
  bool has(uint64_t off, uint64_t len) const {
    return off <= d.size() && len <= d.size() - off;
  }
  void require(uint64_t off, uint64_t len) const {
    if (!has(off, len))
      malformed("truncated binary range at " + std::to_string(off));
  }
  uint64_t integer(uint64_t off, unsigned width) const {
    if (!width || width > 8) malformed("invalid integer width");
    require(off, width);
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i) {
      unsigned shift = order == Endian::Little ? i : width - i - 1;
      value |= uint64_t(d[off + i]) << (8 * shift);
    }
    return value;
  }
  uint8_t u8(uint64_t off) const { return uint8_t(integer(off, 1)); }
  uint16_t u16(uint64_t off) const { return uint16_t(integer(off, 2)); }
  uint32_t u32(uint64_t off) const { return uint32_t(integer(off, 4)); }
  uint64_t u64(uint64_t off) const { return integer(off, 8); }
  std::string fixed(uint64_t off, uint64_t len) const {
    require(off, len);
    uint64_t n = 0;
    while (n < len && d[off + n]) ++n;
    if (!n) return {};
    return {reinterpret_cast<const char*>(d.data() + off), size_t(n)};
  }
  std::string cstr(uint64_t off, uint64_t limit = UINT64_MAX) const {
    require(off, 1);
    uint64_t length = std::min<uint64_t>(limit, d.size() - off);
    auto s = fixed(off, length);
    if (s.size() == length) malformed("unterminated binary string");
    return s;
  }
  std::string stringAt(uint64_t base, uint64_t size, uint64_t index) const {
    require(base, size);
    if (index >= size) malformed("string index outside table");
    return cstr(base + index, size - index);
  }
};
template <class File>
Expected<std::unique_ptr<ObjectFile>> parse(std::span<const uint8_t> bytes,
                                            const std::filesystem::path& path) {
  try {
    auto file = std::make_unique<File>(bytes, path);
    if (auto error = file->parse()) return error;
    return std::unique_ptr<ObjectFile>(std::move(file));
  } catch (const ParseFailure& e) {
    return e.error;
  }
}
inline uint64_t checkedAdd(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a) malformed("address overflow");
  return a + b;
}
}  // namespace qbfd::detail

namespace qbfd::query {
// Queries return values, so a collected result never dangles after an Opened
// dies.
struct ObjectDSL : ::dsl::DSL<ObjectDSL, ::dsl::Pipeline, ::dsl::Operators> {};
inline auto named(std::string name) {
  return ObjectDSL::make_pred(
      [name = std::move(name)](const auto& item) { return item.name == name; });
}
inline auto flags(uint32_t mask) {
  return ObjectDSL::make_pred(
      [mask](const auto& item) { return (item.flags & mask) == mask; });
}
inline auto binding(SymbolBinding value) {
  return ObjectDSL::make_pred(
      [value](const Symbol& s) { return s.binding == value; });
}
inline auto kind(SymbolKind value) {
  return ObjectDSL::make_pred(
      [value](const Symbol& s) { return s.kind == value; });
}
inline auto nativeType(uint32_t value) {
  return ObjectDSL::make_pred(
      [value](const auto& s) { return s.nativeType == value; });
}
inline auto sections() {
  return ::dsl::pipe([](const ObjectFile& file) { return file.sections(); });
}
inline auto symbols() {
  return ::dsl::pipe([](const ObjectFile& file) { return file.symbols(); });
}
inline auto dynamicSymbols() {
  return ::dsl::pipe(
      [](const ObjectFile& file) { return file.dynamicSymbols(); });
}
inline auto segments() {
  return ::dsl::pipe([](const ObjectFile& file) { return file.segments(); });
}
template <class Predicate>
auto where(Predicate predicate) {
  return ::dsl::pipe([predicate = std::move(predicate)](auto items) {
    std::erase_if(items, [&](const auto& item) { return !predicate(item); });
    return items;
  });
}
template <class Function>
auto transform(Function function) {
  return ::dsl::pipe([function = std::move(function)](const auto& items) {
    using Value = std::decay_t<decltype(function(*items.begin()))>;
    std::vector<Value> result;
    result.reserve(items.size());
    for (const auto& item : items) result.push_back(function(item));
    return result;
  });
}
inline auto count() {
  return ::dsl::pipe([](const auto& items) { return items.size(); });
}
}  // namespace qbfd::query

namespace qbfd {
inline Expected<std::shared_ptr<Buffer>> Buffer::fromFile(
    const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return Error{Error::Code::FileNotFound, "cannot open " + p.string()};
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (in.bad())
    return Error{Error::Code::IoError, "read failed: " + p.string()};
  return fromBytes(std::move(bytes));
}

inline std::string_view toString(Format f) {
  switch (f) {
    case Format::ELF:
      return "elf";
    case Format::PE:
      return "pe";
    case Format::COFF:
      return "coff";
    case Format::MachO:
      return "mach-o";
    default:
      return "unknown";
  }
}

inline std::string_view toString(FileType t) {
  switch (t) {
    case FileType::Relocatable:
      return "relocatable";
    case FileType::Executable:
      return "executable";
    case FileType::SharedObject:
      return "shared object";
    case FileType::Core:
      return "core";
    default:
      return "unknown";
  }
}

inline std::string_view toString(Arch a) {
  switch (a) {
    case Arch::X86:
      return "i386";
    case Arch::X86_64:
      return "x86-64";
    case Arch::ARM:
      return "arm";
    case Arch::AArch64:
      return "aarch64";
    case Arch::RISCV32:
      return "riscv32";
    case Arch::RISCV64:
      return "riscv64";
    case Arch::PowerPC:
      return "powerpc";
    case Arch::PowerPC64:
      return "powerpc64";
    case Arch::MIPS:
      return "mips";
    case Arch::MIPS64:
      return "mips64";
    case Arch::SPARC:
      return "sparc";
    case Arch::SPARC64:
      return "sparc64";
    case Arch::S390X:
      return "s390x";
    case Arch::LoongArch64:
      return "loongarch64";
    case Arch::WebAssembly:
      return "wasm";
    default:
      return "unknown";
  }
}

}  // namespace qbfd
