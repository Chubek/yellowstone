# qobjfile

`qobjfile` is a C++20 object-file toolkit: a header-only reader library
(`fd/`) plus a set of command-line tools built on it, including a Unix-style
linker (`ld/` → `qobjld`).

## Tools

| Tool | What it does |
| --- | --- |
| `qobjdump` | identity, headers, sections, symbols, relocations, hex dumps |
| `qobjsize` | text/data/bss/rodata/debug accounting, per section or totalled |
| `qobjnm` | symbol listing with filters, sorting, size sorting and demangling |
| `qobjstr` | printable-string extraction with length, offset and section filters |
| `qobjcp` | byte-for-byte copying and section extraction |
| `qobjar` | `ar` archive creation, listing, extraction, replacement and deletion |
| `qobjstrip` | symbol and section removal for ELF, COFF, Mach-O and PE |
| `qobjld` | static/shared/relocatable link with `ld` scripts (see below) |

Every tool accepts an object **or** an archive: an archive is expanded into its
members, each labelled `archive.a(member.o)`, so `qobjnm -g libfoo.a` works the
way `nm -g libfoo.a` does. `qobjar` and `qobjdump -a` work on the container
itself.

Each tool supports `--help`, and rejects unknown options and missing values
rather than ignoring them.

### Notable options

```sh
qobjdump -x lib.a              # every header, section, symbol and relocation
qobjdump -r --full-reloc a.o   # every decoded relocation field
qobjdump -j .text a.o          # one section, honouring --start/--stop-address
qobjnm -C -g -S a.o            # demangled, external only, sorted by size
qobjnm -A lib.a                # the archive symbol index
qobjsize --sections a.o        # per-section sizes
qobjstr -n 8 -o -S .rodata a.o # strings >= 8 bytes, with offsets, from .rodata
qobjar rcsD lib.a a.o b.o      # create/replace, symbol index, deterministic
qobjar x lib.a out/            # extract every member
qobjstrip -d -o out.o in.o     # remove debug symbols and unreferenced locals
qobjstrip -R .eh_frame in.o    # remove one section
qobjld -o a.out main.o libfoo.a -L. -lfoo -T script.ld -Map map.txt
```

## Linking (`qobjld`)

`qobjld` links ELF relocatables (32/64-bit, either byte order, one
class/order per link) into static executables (`ET_EXEC`, the default),
position-independent executables (`-pie`), shared objects (`--shared`,
`ET_DYN`) and partial links (`-r`, `ET_REL`). Inputs are plain objects,
archives (lazy member selection via the armap, falling back to a full scan
when the archive has no index), `-l` libraries resolved against `-L` paths,
and linker scripts (`-T file`, `--script-text`, `GROUP`/`INPUT`).

```sh
qobjld -o a.out main.o add.o
qobjld -o b.out main.o libu.a
qobjld -r -o part.o main.o add.o && qobjld -o c.out part.o
qobjld --shared -o libt.so add.o
qobjld -T script.ld -o d.out main.o add.o
qobjld --check-script --script-text 'ENTRY(_start) SECTIONS { .text : { *(.text) } }'
```

The script language covers `ENTRY`, `OUTPUT_FORMAT`/`OUTPUT_ARCH`/`OUTPUT`,
`SEARCH_DIR`, `GROUP`/`INPUT`, `MEMORY` (`ORIGIN`/`LENGTH`), `SECTIONS`
(output descriptors with address, `AT`, `>region`, `NOLOAD`, `KEEP`,
`SORT*`, `EXCLUDE_FILE`, `/DISCARD/`), symbol assignments (including
`PROVIDE`/`HIDDEN` and `. = expr`), `ASSERT`, and C-like expressions with
`ALIGN`, `ABSOLUTE`, `ADDR`/`SIZEOF`/`LOADADDR`, `DEFINED`, `CONSTANT`,
`ORIGIN`/`LENGTH`, `MAX`/`MIN` and `K/M/G` suffixes. `--check-script`
validates a script without linking.

### Library API

* `ld/ld.h` + `ld/ld.c` — C API (`qld_link`, `qld_link_bytes`,
  `qld_check_script`, `qld_version`, plus the incremental `qld_*` builder).
  `ld.c` is compiled as C++ so it can use the header-only readers in `fd/`;
  link C consumers with a C++ linker (`g++` or `-lstdc++`).
* `ld/ld.hpp` — C++ wrapper over the C API (`qld::link`, `qld::checkScript`,
  `qld::Linker`).
* Internals per area: `loader.cpp` (input loading), `archive.cpp` +
  `discover_symbols.cpp` + `symbol.cpp` (indexing, lazy extraction,
  resolution), `position_dependenc.cpp` (default layout) + `ldscript.cpp` +
  `ldscript_parser.cpp` + `parser.hpp` (script layout), `relocate.cpp`
  (x86/x64 static relocations), `binarizer.cpp` (ELF emission),
  `static.cpp` / `shared.cpp` / `driver.cpp` (mode drivers), `ld.cpp`
  (shared version string), `main.cpp` (CLI).

```c
#include "ld.h"
const char* in[] = {"main.o", "add.o"};
qld_options_t o = {0};
o.inputs = in; o.n_inputs = 2; o.output = "a.out";
char* err = 0;
if (qld_link(&o, &err)) { fprintf(stderr, "%s\n", err); }
qld_free_string(err);
```

```cpp
#include "ld.hpp"
qld::Options o;
o.inputs = {"main.o", "add.o"};
o.output = "a.out";
if (auto r = qld::link(o); !r) std::cerr << r.error().message << '\n';
```

## Stripping

`qobjstrip` rewrites the file rather than zeroing fields in place, because
dropping a symbol shifts every index after it. Two invariants hold:

* **The file layout never moves.** Section contents keep their existing offsets
  and virtual addresses, so absolute addresses in data and the offsets recorded
  by relocations stay valid. Bytes belonging to a removed section are zeroed,
  not relocated.
* **Every index that can name a symbol or section is renumbered together** —
  relocation entries, `sh_link`/`sh_info`, `st_shndx`, the symbol table's
  first-global boundary, COFF `SymbolTableIndex` and Mach-O `r_symbolnum`.
  A file that parses but points at the wrong symbol is worse than a refusal.

`-g` and `-d` match GNU `strip` symbol for symbol on the formats tested, and
the results still link and behave identically. Coverage is ELF (both classes,
both byte orders), COFF (including bigobj), Mach-O (both classes and byte
orders) and the COFF symbol table inside a PE image.

## Build

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The library can also be configured on its own, which is the supported way to
consume it without the tools:

```sh
cmake -S fd -B /tmp/qbfd-build
cmake --build /tmp/qbfd-build
ctest --test-dir /tmp/qbfd-build --output-on-failure
```

### Dependency

qBFD's query DSL is built on the `qDSL` toolkit in `../qdsl`. qobjfile no longer
carries a private copy of that header — the two would drift. The build finds it
in this order:

1. `-DQOBJFILE_QDSL_DIR=<dir>`
2. the sibling `qdsl/` directory in this repository
3. an installed `qDSL` CMake package

## Tests

* `qbfd_regressions` — synthetic fixtures for all four backends, every
  truncated prefix, seeded mutations, archive parsing, and the strip engine.
  Runs clean under `-fsanitize=address,undefined` (`-DQBFD_SANITIZE=ON`).
* `qbfd_fixtures` — compiles objects for twelve targets, cross-checks their
  symbols against `llvm-readobj`, builds a real archive with `ar`/`ranlib` and
  reads it back, and produces reference strip outputs. Skipped, not failed,
  when those tools are absent.
* `qobjfile_cli` — end-to-end checks of every tool's argument handling, archive
  traversal and reporting, driven by `cmake/run-cli-tests.cmake`.
* `qobjld_cli` — links real fixtures (static exec, archive selection,
  `-r` partial link, `-T` script, `--shared`, undefined diagnostics,
  `--check-script`) and runs the executables, driven by
  `cmake/run-ld-tests.cmake`.

## Boundaries

These are genuinely not implemented, and are reported as errors rather than
approximated:

* Writing Mach-O/PE images. The linker emits ELF only; other formats are
  reported rather than approximated. The inspection library itself only
  reads; `qobjld` is the one place that produces loadable output.
* Dyld export tries, chained fixups and bind/rebase streams.
* `LC_UNIXTHREAD` entry decoding, and ELF `DT_RELR`/RELR section expansion.
* PE delay imports, bound imports, and resource or debug-directory decoding.
* A Windows import library's symbol index. The container is detected and its
  members read, but the index is left empty rather than guessed at.
* The BSD `__.SYMDEF` symbol table is recognised but not decoded; the variants
  disagree about its layout, and a wrong reading would point a resolver at the
  wrong member. BSD archives are written without an index, which is always safe
  because a resolver with no index scans every member.
* Mach-O `-s` drops symbols and their relocations together, matching `strip -S`
  rather than producing a linkable object.
