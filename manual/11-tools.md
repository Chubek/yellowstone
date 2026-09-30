# Chapter 11: Inspection Tools

qobjdump, qobjsize, qobjnm, qobjstr, qobjstrip, qobjcp, and qobjar share common argument and diagnostic helpers.

## ISA-aware qobjdump

`qobjdump` can now use processor descriptions from `infobank/isa`:

```sh
qobjdump -m --isa-dir infobank/isa program.o
qobjdump -d --isa-dir infobank/isa program.o
qobjdump -d --search ret --tui program.o
```

`-m` reports the detected architecture and matching profile fields. `-d` scans executable sections, selects the matching ISA document, uses encoding opcode fields to identify operations, and prints syntax plus semantic annotations when available. `--search` filters disassembly lines by a literal pattern. `--tui` adds terminal highlighting for instruction text.

The decoder is intentionally metadata-driven. Unsupported or ambiguous encodings are emitted as `.byte` or `.word` records instead of being silently discarded.

## Hex and octal views

Section contents can be displayed in hexadecimal or octal:

```sh
qobjdump --hex-dump -j .text program.o
qobjdump --oct-dump -j .text program.o
qobjdump --oct-dump --no-text -j .text program.o
```

Printable text is shown beside each row by default. `--no-text` suppresses that column.

For text files, firmware blobs, and other files that are not object files, use `--raw`:

```sh
qobjdump --raw --hex-dump input.bin
qobjdump --raw --oct-dump --no-text input.bin
```

Raw mode uses the same 16-byte rows and text rendering as section dumps, so generated dumps are easy to inspect and compare.
