// cli.hpp - formatting, file access and archive traversal shared by the tools.
//
// The inspection tools report on the canonical ELF/PE/COFF/Mach-O model that
// qBFD exposes, so nothing here reinterprets a host structure from an input
// file: every field printed comes from qBFD.
#pragma once

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <qBFD.hpp>

namespace qobj {

// The tools report one version. Bumped when behaviour changes, not on every
// edit; kept here so a tool never has to repeat the number.
inline constexpr std::string_view kVersion = "1.0";
inline constexpr std::string_view kBFDVersion = "1.0";

// ---------------------------------------------------------------------------
// Errors
// ---------------------------------------------------------------------------
inline int reportError(const std::string& path, const qbfd::Error& e) {
  std::cerr << path << ": " << e.message << '\n';
  return 1;
}
inline int reportError(const std::string& path, const std::exception& e) {
  std::cerr << path << ": " << e.what() << '\n';
  return 1;
}

// ---------------------------------------------------------------------------
// Number and name formatting
// ---------------------------------------------------------------------------
inline std::string hex(uint64_t value, unsigned width = 0) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  if (width) out << std::setw(width);
  out << value;
  return out.str();
}

inline const char* formatName(qbfd::Format f) {
  switch (f) {
    case qbfd::Format::ELF: return "ELF";
    case qbfd::Format::PE: return "PE";
    case qbfd::Format::COFF: return "COFF";
    case qbfd::Format::MachO: return "Mach-O";
    default: return "unknown";
  }
}

inline const char* yesNo(bool value) { return value ? "yes" : "no"; }

// The single-letter symbol type used by `nm`. Weakness lives in the binding,
// not in a flag, so the undefined and defined cases are decided separately.
inline char symbolType(const qbfd::Symbol& s) {
  using Kind = qbfd::SymbolKind;
  if (s.flags & qbfd::sym::Undefined) {
    if (s.binding == qbfd::SymbolBinding::Weak) return 'w';
    if (s.flags & qbfd::sym::Hidden) return 'v';
    return 'U';
  }
  if (s.binding == qbfd::SymbolBinding::Weak) {
    if (s.kind == Kind::Object) return 'V';
    return 'W';
  }
  if (s.flags & qbfd::sym::Absolute) return 'a';
  if (s.binding == qbfd::SymbolBinding::Unique) return 'u';
  if (s.flags & qbfd::sym::Debug) return 'n';
  switch (s.kind) {
    case Kind::Function: return 'T';
    case Kind::Object: return 'D';
    case Kind::Section: return 'N';
    case Kind::File: return 'F';
    case Kind::Common: return 'C';
    case Kind::Tls: return 'D';
    case Kind::Indirect: return 'i';
    default: return '?';
  }
}

// The full `nm` type string, e.g. "T", "i", "W".
inline std::string symbolTypeName(const qbfd::Symbol& s) {
  return std::string(1, symbolType(s));
}

// Section flags, in the order the conventional listings use. A trailing comma
// makes the column read naturally for any subset.
inline std::string sectionFlags(const qbfd::Section& s) {
  namespace sec = qbfd::sec;
  std::string out;
  auto add = [&](char c) {
    if (!out.empty()) out += ' ';
    out += c;
  };
  if (s.flags & sec::Alloc) add('A');
  if (s.flags & sec::Code) add('T');
  if (s.flags & sec::Writable) add('W');
  if (s.flags & sec::ReadOnly) add('R');
  if (s.flags & sec::Load) add('L');
  if (s.flags & sec::Reloc) add('I');
  if (s.flags & sec::Discard) add('D');
  if (s.flags & sec::Tls) add('T');
  if (s.flags & sec::Note) add('N');
  if (s.flags & sec::Strings) add('S');
  if (s.flags & sec::Debug) add('g');
  if (s.flags & sec::Bss) add('b');
  if (s.flags & sec::Exec) add('x');
  return out;
}

// ELF section type name, for `readelf -S`-style output.
inline std::string elfSectionType(uint32_t type) {
  namespace k = qbfd::elf::k;
  switch (type) {
    case k::Null: return "NULL";
    case k::Progbits: return "PROGBITS";
    case k::Symtab: return "SYMTAB";
    case k::Strtab: return "STRTAB";
    case k::Rela: return "RELA";
    case k::Dynamic: return "DYNAMIC";
    case k::Note: return "NOTE";
    case k::Nobits: return "NOBITS";
    case k::Rel: return "REL";
    case k::Dynsym: return "DYNSYM";
    case 18: return "SYMTAB SECTION INDICIES";
    case 19: return "RELR";
    case 0x6ffffff5: return "GNU_ATTRIBUTES";
    case 0x6ffffff6: return "GNU_HASH";
    case 0x6ffffffd: return "GNU_VERDEF";
    case 0x6ffffffe: return "GNU_VERNEED";
    case 0x6fffffff: return "GNU_VERSYM";
    case 0x70000000: return "X86_64_UNWIND";
    case 0x6fff4c03: return "LLVM_ADDRSIG";
    default: return "0x" + hex(type);
  }
}

inline bool isPrintable(uint8_t c) { return c >= 32 && c < 127; }

// A printable rendering of arbitrary bytes, with anything else shown as '.'.
inline std::string printableText(std::span<const uint8_t> bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (uint8_t c : bytes) out += isPrintable(c) ? char(c) : '.';
  return out;
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
// A number given on the command line, accepting 0x-prefixed hex.
inline bool parseNumber(const std::string& text, uint64_t& out) {
  if (text.empty()) return false;
  try {
    size_t used = 0;
    out = std::stoull(text, &used, 0);
    return used == text.size();
  } catch (const std::exception&) {
    return false;
  }
}

inline std::vector<uint8_t> readFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open " + path.string());
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline bool writeFile(const std::filesystem::path& path,
                      std::span<const uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// Archive-aware traversal
//
// Every inspection tool accepts a plain object *or* an archive. A `Source` is
// one openable unit: the file itself, or one member of an archive. `visit`
// walks them, reporting a per-unit failure without abandoning the rest, which
// is what a caller listing several inputs expects.
// ---------------------------------------------------------------------------
struct Source {
  std::string label;                 // what to print in diagnostics
  std::vector<uint8_t> bytes;        // owned, so the object stays valid
  std::shared_ptr<qbfd::Buffer> buffer;
  std::unique_ptr<qbfd::ObjectFile> object;

  const qbfd::ObjectFile& file() const { return *object; }
};

class Sources {
 public:
  // Opens one path. An archive yields every member; anything else yields
  // itself. Failures are appended to `errors` with their diagnostics already
  // recorded, so a caller can report them and carry on.
  bool add(const std::filesystem::path& path, std::string& firstError) {
    std::vector<uint8_t> bytes;
    try {
      bytes = readFile(path);
    } catch (const std::exception& e) {
      firstError = e.what();
      return false;
    }
    if (!qbfd::isArchive(bytes)) {
      Source s;
      s.label = path.string();
      s.bytes = std::move(bytes);
      auto opened = qbfd::open(s.bytes, path);
      if (!opened) {
        firstError = opened.error().message;
        return false;
      }
      s.object = std::move(opened->object);
      items_.push_back(std::move(s));
      return true;
    }
    // A thin archive stores paths, so it has to be opened from its own
    // directory rather than from a buffer.
    auto opened = qbfd::openArchive(path);
    if (!opened) {
      firstError = opened.error().message;
      return false;
    }
    for (const auto& member : opened->archive->members()) {
      if (member.special) continue;
      auto payload = opened->archive->readMember(member);
      if (!payload) {
        firstError = payload.error().message;
        return false;
      }
      Source s;
      s.label = path.string() + "(" + member.name + ")";
      s.bytes.assign(payload->begin(), payload->end());
      auto member_open = qbfd::open(s.bytes, member.name);
      if (!member_open) {
        firstError = member_open.error().message;
        return false;
      }
      s.object = std::move(member_open->object);
      items_.push_back(std::move(s));
    }
    return true;
  }

  const std::vector<Source>& items() const { return items_; }
  bool empty() const { return items_.empty(); }

 private:
  std::vector<Source> items_;
};

// Runs `fn` over every unit, accumulating the exit status. `fn` returns 0 on
// success. A unit that fails to open is reported and counts as a failure.
template <class Fn>
int visit(const std::vector<std::string>& paths, Fn&& fn) {
  int status = 0;
  for (const auto& path : paths) {
    Sources sources;
    std::string error;
    if (!sources.add(path, error)) {
      std::cerr << path << ": " << error << '\n';
      status = 1;
      continue;
    }
    for (const auto& source : sources.items()) status |= fn(source);
  }
  return status;
}

}  // namespace qobj
