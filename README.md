# qobjfile

`qobjfile` is a C++20 object-file toolkit: a header-only reader library
(`qobjbfd/`) plus a set of command-line tools built on it.

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
cmake -S qobjfile/qobjbfd -B /tmp/qbfd-build
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

## Boundaries

These are genuinely not implemented, and are reported as errors rather than
approximated:

* Writing Mach-O/PE images, or any linker. The library inspects; it does not
  produce loadable output.
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
