# Yellowstone enhancement pass

This source tree contains an enhancement pass focused on qBFD, qobjld, and
qobjdump, with smaller utility fixes.

## qBFD

- Added generic `ObjectFile` helpers:
  - `symbolAtAddress()`
  - `sectionsWithFlags()`
  - `allRelocations()`
  - `fileOffsetForAddress()`
  - `bytesAtAddress()`
- `findSymbol()` now searches both static and dynamic symbol tables.
- Added `toString(Endian)`.
- Added PE raw section-header access for richer inspection.
- Expanded ELF relocation naming/metadata for AArch64, RISC-V, ARM, and
  PowerPC/PowerPC64 relocations.

## qobjld

- Fixed architecture selection so later input objects cannot silently change
  the link architecture.
- Mixed-architecture inputs are now diagnosed explicitly.
- Archive members are checked for architecture as well as class and endian.
- Added basic static relocation support beyond x86/x86-64:
  - AArch64 ABS64/ABS32/PREL64/PREL32
  - RISC-V 32/64, 32 PC-relative
  - ARM ABS32/REL32
  - PowerPC/PowerPC64 ADDR32/REL32/ADDR64/REL64
- Unsupported instruction-field relocations remain explicit errors rather
  than being silently written incorrectly.

## qobjdump

- Fixed address-range arithmetic to avoid unsigned overflow.
- Added raw PE section geometry to `--private-headers`.
- `--search` is applied to disassembly output.
- Address-range handling now respects file-backed section bytes and BSS.

## qobjnm

- Default symbol ordering is now by displayed symbol name, matching the
  conventional nm listing behavior; size sorting remains available with `-S`.

## Validation

The modified qBFD interface was compiled with a standalone C++20 smoke test.
Syntax-only compilation also passed for the modified qobjdump, qobjld
relocation/loader/archive-discovery, and qobjnm translation units.

A complete repository build was attempted, but the repository's broad build
timed out in the environment before the final aggregate link step.
