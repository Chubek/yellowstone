# Chapter 26: Extension Guide

Add a reader by implementing the `ObjectFile` contract, register a target, add fixtures, then expose queries and documentation. Keep ABI additions source compatible.

## Extending ISA support

ISA support is a horizontal slice shared by tools and libraries:

1. Add or update a processor description under `infobank/isa`.
2. Validate it against `infobank/isa.peg` and the field conventions in `infobank/schema.json`.
3. Load it through `qisa::Database` and inspect the resulting `Document` AST.
4. Add architecture-name mapping in the consumer, such as `dump/main.cpp`.
5. Add a fixture containing representative encodings and verify fallback output for unknown encodings.

A consumer should treat `syntax` and `semantics` as optional metadata. The object reader remains useful when no description is installed, and a decoder must print raw bytes when an encoding cannot be resolved.

## Adding a new architecture

Extend the qbfd `Arch` enum and backend mapping first. Then add the corresponding ISA filename mapping, profile reporting, and disassembly width rules. Keep register names and aliases in the `.isa` file so other consumers can reuse them.

## Keeping the slice reusable

Do not put terminal rendering, file-format parsing, or linker policy into `common/isa.hpp`. The qisa AST/database layer should remain independent; qobjdump, qld, and future compiler or relocation tools can build their own views over it.
