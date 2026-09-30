// internal.hpp - shared internal model for the qobjld linker.
//
// All linker internals build on the header-only readers in ../fd/ (qBFD.hpp
// and friends). Nothing here reinterprets host structures from input bytes:
// every field comes from qbfd::ObjectFile / Section / Symbol / Relocation.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "qBFD.hpp"

namespace qld::detail {

// ---------------------------------------------------------------------------
// Link modes
// ---------------------------------------------------------------------------
enum class Mode : uint8_t { Exec, Shared, Relocatable };

// ---------------------------------------------------------------------------
// Public-facing options (mirrors ld.hpp Options; internal copy avoids a
// dependency from internals back to the wrapper header).
// ---------------------------------------------------------------------------
struct LinkOptions {
  std::vector<std::string> inputs;      // object files, archives, scripts
  std::vector<std::string> libPaths;    // -L directories
  std::vector<std::string> libs;        // -l names (without lib/.a)
  std::string output;                   // -o
  std::string entry = "_start";         // -e
  std::string script;                   // -T path
  std::string scriptText;               // -T text via --script-text (or GROUP)
  bool scriptIsText = false;
  bool hasBase = false;
  uint64_t base = 0x400000;             // --image-base / -Ttext
  Mode mode = Mode::Exec;               // -shared / -r / default exec
  bool pie = false;                     // -pie
  bool gcSections = false;              // --gc-sections
  bool stripDebug = false;              // -s / --strip-debug
  bool verbose = false;                 // --verbose
  bool allowUndefined = false;          // --allow-undefined / -z undefs
  bool deterministic = true;            // -D not yet; placeholder
  std::string mapFile;                  // -M / -Map
  std::string emulator;                 // -m (accepted, selects arch check)
};

// ---------------------------------------------------------------------------
// Loaded inputs
// ---------------------------------------------------------------------------
struct LoadedObject {
  std::string label;                    // display name, e.g. "a.o" or "lib.a(b.o)"
  std::string archivePath;              // empty for plain objects
  std::vector<uint8_t> bytes;           // owned storage
  std::shared_ptr<qbfd::Buffer> buffer;
  std::unique_ptr<qbfd::ObjectFile> object;
  bool fromArchive = false;
  bool isScript = false;                // INPUT() listed a linker script
};

struct LoadedArchive {
  std::string path;
  std::vector<uint8_t> bytes;
  std::shared_ptr<qbfd::Buffer> buffer;
  std::unique_ptr<qbfd::Archive> archive;
};

// One input section that participates in the link. Relocation sections
// (SHT_REL/RELA, COFF relocs) are not materialised here; they are consumed
// through ObjectFile::relocations().
struct InputSection {
  size_t objectIndex = 0;               // into LinkState::objects
  uint32_t sectionIndex = 0;            // Section::index in that object
  qbfd::Section section;                // canonical copy
  std::vector<uint8_t> contents;        // file bytes (empty for BSS)
  std::vector<qbfd::Relocation> relocs; // decoded relocations
  bool kept = true;                     // false when --gc-sections drops it
  uint64_t outOffset = 0;               // offset inside the output section
};

// A global symbol definition after resolution.
struct Def {
  size_t objectIndex = 0;
  uint32_t symbolIndex = 0;             // index in that object's symbols()
  qbfd::Symbol symbol;                  // canonical copy
  bool weak = false;
  bool common = false;
  uint64_t commonSize = 0;
  uint64_t commonAlign = 1;
};

// Per-output-section accumulator.
struct OutputSection {
  std::string name;
  uint32_t flags = 0;                   // sec::Flags union of contributors
  uint64_t alignment = 1;
  uint64_t vma = 0;
  uint64_t fileOffset = 0;
  uint64_t size = 0;                    // memory size (includes BSS tail)
  uint64_t fileSize = 0;                // bytes in file (excludes BSS tail)
  bool bss = false;                     // true when NOBITS-like
  bool alloc = false;
  bool executable = false;
  bool writable = false;
  std::vector<size_t> inputs;           // indices into LinkState::inputSections
  uint32_t index = 0;                   // output section index (0 == NULL)
};

struct LinkState {
  LinkOptions options;
  std::vector<LoadedObject> objects;    // selected objects in link order
  std::vector<LoadedArchive> archives;  // archives seen (for -M reporting)
  std::vector<InputSection> inputSections;
  std::map<std::string, Def> defs;      // global symbol table
  std::vector<std::string> undefined;   // ordered undefined externals
  std::vector<OutputSection> outSections;
  std::map<std::string, uint64_t> symAddr; // resolved absolute addresses
  std::string mapText;                  // -Map contents
  // ELF geometry decided during layout.
  bool wide = true;                     // 64-bit when true
  qbfd::Endian endian = qbfd::Endian::Little;
  qbfd::Arch arch = qbfd::Arch::X86_64;
  uint64_t entryAddr = 0;
  // Diagnostics collected along the way.
  std::vector<std::string> warnings;
};

struct LinkError {
  std::string message;
};

// Loads every input file (objects, archives, scripts via GROUP/INPUT).
// On success fills state->objects with the eagerly-selected objects and
// state->archives with the archive containers; archive member selection
// itself happens in discover_symbols.cpp.
bool loadInputs(LinkState& state, std::string& error);

// Archive helpers (archive.cpp).
bool isArchivePath(const std::string& path);
const LoadedArchive* findArchive(const LinkState& state,
                                 const std::string& path);
// Expands one archive container into candidate members (not yet selected).
struct ArchiveCandidate {
  size_t archiveIndex = 0;
  qbfd::Archive::Member member;
  std::vector<uint8_t> bytes;
};
std::vector<ArchiveCandidate> expandArchive(LoadedArchive& archive,
                                            std::string& error);

// Symbol handling (symbol.cpp).
void addObjectSymbols(LinkState& state, size_t objectIndex,
                      std::vector<std::string>& errors);
bool resolveCommons(LinkState& state, std::string& error);

// Archive member selection + global discovery (discover_symbols.cpp).
bool discoverSymbols(LinkState& state, std::string& error);

// Address assignment (position_dependenc.cpp). Honours the linker script
// when one is present, otherwise uses the default layout.
bool assignAddresses(LinkState& state, std::string& error);

// Relocation (relocate.cpp). Applies relocations to the staged section
// contents after assignAddresses() has fixed every symbol address.
bool applyRelocations(LinkState& state,
                      std::vector<std::vector<uint8_t>>& staged,
                      std::string& error);

// Linker-script execution (ldscript.cpp). Parses state->options script text
// (or file) and, when present, drives assignAddresses() instead of the
// default layout. Also exposes expression evaluation for PROVIDE/DEFINED.
bool hasScript(const LinkState& state);
bool runScript(LinkState& state, std::string& error);
bool loadScriptExtras(LinkState& state, std::string& error);

// Top-level driver (driver.cpp).
bool linkState(LinkState& state, std::vector<uint8_t>& out,
               std::string& error);

// Output emission (binarizer.cpp).
bool emitExecutable(const LinkState& state,
                    const std::vector<std::vector<uint8_t>>& staged,
                    std::vector<uint8_t>& out, std::string& error);
bool emitShared(const LinkState& state,
                const std::vector<std::vector<uint8_t>>& staged,
                std::vector<uint8_t>& out, std::string& error);
bool emitRelocatable(const LinkState& state, std::vector<uint8_t>& out,
                     std::string& error);

// Mode drivers (static.cpp / shared.cpp).
bool linkStatic(LinkState& state, std::vector<uint8_t>& out,
                std::string& error);
bool linkShared(LinkState& state, std::vector<uint8_t>& out,
                std::string& error);
bool linkRelocatable(LinkState& state, std::vector<uint8_t>& out,
                     std::string& error);

}  // namespace qld::detail
