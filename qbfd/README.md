# qBFD

C++20, header-only binary inspection for ELF, PE, COFF and Mach-O. Include
`qBFD.hpp`; no implementation macro, platform SDK, libbfd or link library is
required. All four backends are registered once on the first front-end open.
Legacy `QBFD_IMPLEMENTATION` / `QBFD_ENABLE_*` definitions are unnecessary and
have no effect. The public Mach-O filename is `qMachO.hpp`.

## Opening and ownership

```cpp
#include "qBFD.hpp"

auto opened = qbfd::open(std::filesystem::path("program"));
if (!opened) {
  std::cerr << opened.error().message << '\n';
  return 1;
}
const auto& object = *opened->object;
for (const auto& section : object.sections()) {
  auto contents = object.sectionContents(section);
  // contents is a checked, borrowed span; BSS has no file contents.
}
```

`open(vector<uint8_t>, label)` takes ownership of bytes. Front-end objects
retain that storage even if moved out of `Opened`. Spans and references returned
by an object remain borrowed from that object. Backend `ELFFile::open`,
`PEFile::open`, `COFFFile::open`, `MachOFile::open`, and `macho::openSlice`
accept borrowed spans: keep the source bytes alive and immutable while using
those objects.

`openAs(path_or_bytes, targetName)` selects a backend explicitly and still
validates its input. `Registry::instance().targets()` lists available names.
ELF targets use class and byte order (`elf32-little`, `elf32-big`,
`elf64-little`, `elf64-big`); PE/COFF names describe the machine; thin Mach-O
uses `mach-o-32` and `mach-o-64`. Register custom targets before concurrent
opens; concurrent registry mutation is not supported. Target names are owned
and duplicate registrations are ignored.

`detectFormat` is a signature classifier, not a validity check. Structural
failures return `Expected<T>` errors, including truncated fields, invalid
ranges, unterminated strings and bad table references. Allocation failures
follow normal C++ exception behavior. These libraries inspect metadata; they
do not load code or apply relocations.

## Archives

`qArchive.hpp` reads and writes `ar` containers. `ar` has no single standard,
so all three conventions in circulation are handled:

| Flavour | Magic | Names | Index |
| --- | --- | --- | --- |
| SVR4/GNU | `!<arch>\n` | `<= 15` bytes inline, longer names in a `//` table as `/<offset>` | `/` armap, or `/SYM64/` |
| BSD | `!<arch>\n` | `<= 16` bytes inline, otherwise `#1/<len>` with the name at the head of the payload | `__.SYMDEF`, recognised but not decoded |
| thin | `!<thin>\n` | as SVR4 | as SVR4 |

A thin archive keeps its index and long-name table in the file but stores each
object payload separately, so its headers still record a real size while no
payload bytes follow. `readMember` materialises those; `memberData` cannot.
Windows import libraries (two leading `/` members) are detected and their
members read, but no index is produced — see the boundaries below.

```cpp
auto archive = qbfd::openArchive("libfoo.a");
for (const auto& member : archive->archive->members()) {
  if (member.special) continue;            // index or long-name table
  auto object = archive->archive->openMember(member);
}

qbfd::ArchiveOptions options;
options.index = true;                      // rebuilt from the members' symbols
auto bytes = qbfd::buildArchive(entries, options);
```

`buildArchive` needs no fixed-point iteration: the armap's *size* depends only
on the symbol names, never on the offsets it records, so the layout is computed
once. With `deterministic` the output is byte-identical to `ar rcsD` on the
toolchains tested.

## Rewriting

`qStrip.hpp` removes symbols and sections. The readers are read-only by design,
so this works the way `objcopy` does: copy the input, then rewrite the tables
that describe it. The file layout never moves — section contents keep their
offsets and addresses, so absolute values in data and relocation offsets stay
valid — and every index that can name a symbol or a section is renumbered
together. Both string tables are appended as fresh sections rather than
rewritten in place, because a Clang-produced object uses one `.strtab` for
both the symbol names and the section names.

```cpp
qbfd::strip::Options options;
options.mode = qbfd::strip::Mode::Debug;  // All | Debug | Unneeded
options.removeSections = {".eh_frame"};
auto result = qbfd::strip::strip(bytes, options);
```

`Result::symbolsRemoved` and `Result::sectionsRemoved` count what no longer
exists, which includes the name tables this pass rebuilds.

The readers expose the on-disk geometry a rewriter needs, so the layout is
stated once rather than re-derived: `Layout` and `rawSections()` on all four
backends, plus `relocationPointer()`, `relocationCountOffset()` and the
`rawSymbol*` accessors on COFF and PE, and `symbolSlot()` on Mach-O.

## Native C++ DSL

The shared DSL uses the `qdsl` toolkit's CRTP `DSL`, `Pipeline`, `Operators`,
`PipeStage` and `Predicate` facilities. Query results own their selected values.

```cpp
using namespace qbfd;
using namespace qbfd::query;

auto codeNames = object
  | sections()
  | where(flags(sec::Code) & !flags(sec::Bss))
  | transform([](const Section& s) { return s.name; });

auto globalFunctions = object
  | symbols()
  | where(binding(SymbolBinding::Global) & kind(SymbolKind::Function));

// Error-aware opening and inspection; a failed open skips the query.
auto result = std::filesystem::path("program")
  | read()
  | requireFormat(Format::ELF)
  | inspect(sections());

auto forced = std::filesystem::path("program") | as("elf64-little");
```

`sections()`, `symbols()`, `dynamicSymbols()` and `segments()` select canonical
collections; `where`, `transform`, and `count` operate on those collections.
`named`, `flags`, `binding`, `kind`, and `nativeType` are composable predicates.
Format namespaces reuse the shared vocabulary and add:

| Namespace | Additional vocabulary |
| --- | --- |
| `elf::query` | `type(SHT_*)`, `allocated()` |
| `pe::query` | `imports()`, `exports()` on `const PEFile&` |
| `coff::query` | `comdat()`, `directives()` section predicates |
| `macho::query` | `type(S_*)`, `segment(name)` segment predicate |

For example, `peFile | pe::query::imports() | where(named("CreateFileW"))`
selects imports by name. The format headers may be included independently for
their backend APIs; include the umbrella for `open`, `openAs`, and open pipelines.

## Format coverage and boundaries

| Format | Implemented |
| --- | --- |
| ELF | 32/64-bit, both byte orders, extended section counts/indices, section/program headers, static/dynamic symbols, REL/RELA, section-backed dependencies and PT_DYNAMIC dependencies without section headers |
| PE | PE32/PE32+, sections, COFF symbols, named/ordinal imports, named/ordinal/forwarded exports, base relocations, dependencies, RVA mapping |
| COFF | Standard/bigobj headers, long names, auxiliary/file/section symbols, COMDAT associations, weak externals, relocation overflow records, linker directives |
| Mach-O | Thin 32/64-bit, both byte orders, segments/sections, nlist symbols, section relocations, dylib dependencies/install names/rpaths, LC_MAIN entry points, universal 32/64-bit slice tables |
| `ar` | SVR4/GNU, BSD and thin containers, long-name tables, symbol indexes, byte-identical deterministic writing |

`Section::index` is zero-based; ELF retains its null section. ELF/COFF symbol
values in relocatable files are section offsets (plus section VMA where
applicable); image symbols are virtual addresses. Mach-O nlist values retain
their native values. PE exported RVAs and IAT slots are exposed as VAs.

A relocation's `symbol` indexes `symbols()` unless `dynamicSymbol` is true,
in which case it indexes `dynamicSymbols()`. Mach-O local relocations use
`targetSection`; scattered relocations preserve `scattered` and `nativeValue`.
`hasAddend` distinguishes decoded/explicit addends from unavailable ones.
Unknown relocation types retain their numeric type; width and PC relativity
are only supplied for recognized encodings. COFF instruction-field addends
outside x86/x64 are not decoded. ELF RELA addends are preserved for all machines;
implicit ELF addends are decoded only for recognized x86/x64 types.

Universal Mach-O opening requires explicit slice selection:

```cpp
auto table = qbfd::macho::slices(bytes);
if (table && !table->empty()) {
  auto object = qbfd::macho::openSlice(bytes, 0);
}
```

Automatic universal opening returns `Unsupported` rather than choosing an
architecture. Dyld export tries, chained fixups, bind/rebase streams,
`LC_UNIXTHREAD` entry decoding, ELF RELR expansion, dynamic symbol recovery
without section headers, PE delay/bound imports and resource/debug-directory
decoding are outside the current interface. A Windows import library's symbol
index is not decoded: its first `/` member is an import lookup table rather than
a big-endian armap, and guessing that layout would invent entries. BSD's
`__.SYMDEF` is likewise recognised but not decoded, for the same reason. Both
still expose every member. Unknown load commands are skipped after checking
their envelope. Compressed section bytes are returned as stored.

## Build and tests

```sh
cmake -S qobjfile/qobjbfd -B /tmp/qbfd-build
cmake --build /tmp/qbfd-build
ctest --test-dir /tmp/qbfd-build --output-on-failure
```

qBFD is header-only and depends on the `qDSL` toolkit, which is found via
`QOBJFILE_QDSL_DIR`, the sibling `qdsl/` directory, or an installed `qDSL`
package. CMake exposes the interface target `qbfd::qbfd`. `QBFD_SANITIZE=ON`
enables AddressSanitizer and UndefinedBehaviorSanitizer with GCC/Clang. In
ptrace-based environments where LeakSanitizer cannot run, use
`ASAN_OPTIONS=detect_leaks=0`.

Tests cover synthetic fixtures, malformed references, every truncated prefix,
seeded mutations, ownership, archive parsing and round trips, the strip engine,
DSL operators, include order and multiple translation units. `qbfd_tests
path...` also exercises supplied object fixtures, and accepts an archive, whose
members it opens and whose index it cross-checks. `tests/run-fixtures.py`
generates native and cross-target fixtures with Clang, checks their symbols
against LLVM's reader, builds a real archive with `ar`/`ranlib` and reads it
back, and produces reference strip outputs. It exits 77 to skip when those
tools are unavailable.

Implementation references: the System V gABI ELF format, Microsoft's PE/COFF
specification, LLVM's `BinaryFormat/MachO.h`, and the `ar(5)` format
description. No host binary structure is reinterpreted from input bytes.
