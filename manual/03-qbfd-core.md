# Chapter 3: qbfd Core Model

qbfd normalizes object formats into objects, sections, symbols, relocations, segments, and diagnostics. Readers never reinterpret untrusted input as host structures.

## ISA metadata slice

ISA descriptions are loaded through [`common/isa.hpp`](../common/isa.hpp). The parser uses the combinator primitives from [`common/qdsl.hpp`](../common/qdsl.hpp) and produces a reusable AST:

- `Document` identifies the architecture and stores profile fields.
- `Encoding` records opcode and layout fields.
- `Operation` records syntax, operand descriptions, and semantic S-expressions.
- `RegisterClass` stores named registers, widths, and numbers.

`qisa::Database` loads individual `.isa` files or scans a directory. The default tool directory is `infobank/isa`.

## Architecture mapping

qbfd architecture values map to ISA descriptions by name. For example, `Arch::X86_64` selects `amd64` from `x86.isa`, `Arch::RISCV64` selects `riscv64.isa`, and `Arch::AArch64` selects `aarch64.isa`. Unknown or missing descriptions remain a diagnostic condition; object parsing itself does not fail.

## Source of truth

The accepted description grammar is [`infobank/isa.peg`](../infobank/isa.peg). The bundle requirements and field names are defined by [`infobank/schema.json`](../infobank/schema.json). Additions to the ISA AST should preserve those source formats.
