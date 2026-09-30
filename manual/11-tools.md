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

## TUI, pager, and configuration

`--tui` now opens an in-process DomTERM pager. It uses DomTERM's TTY session API, alternate screen, raw key handling, paging, and `/` search (`q` quits, `j/k` move, `g/G` jump). A configured external pager remains available through `--pager-command`.

The default rendering remains plain: semantic S-expressions and decoded register names are off. Enable them explicitly:

```sh
qobjdump -d --semantics --register-names program.o
```

Persistent settings are read from `$XDG_CONFIG_HOME/yellowstone/Objdump.ini`, or `$HOME/.config/yellowstone/Objdump.ini` when `XDG_CONFIG_HOME` is unset:

```ini
[qobjdump]
pager = true
semantics = false
register_names = false
color = true
theme = solarized
pager_command = less -R
isa_dir = /opt/yellowstone/infobank/isa
```

Supported themes are `default`, `solarized`, `dracula`, and `monochrome`. Command-line switches override the file: `--semantics`, `--register-names`, `--no-color`, `--no-pager`, `--theme`, `--pager-command`, and `--isa-dir`.

## Format readers

The build also installs `readelf`, `readexe`, and `readmacho`. They share qbfd's
canonical section and symbol model and accept `--tui`, `--no-color`, and
`--no-text`. The readers select the matching ELF, PE/COFF, or Mach-O backend
from the input magic and render section and symbol tables through the same
DomTERM pager.

## Binscript

`binscript` is the binary manipulation language companion to Termscript. Its
grammar is `binscript/binscript.g`, generated with `scripts/aurocks.pl`; the
public C ABI is in `binscript/binscript.h` and includes a VM and C emission
entry point. The initial runtime accepts scripts and provides the stable ABI
on which ELF, PE/COFF, and Mach-O native modules can be registered.
