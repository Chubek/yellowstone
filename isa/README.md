# Simple ISA sources

These `.isa` files are an alternative to `../compiler/stdspec/*.dsymspec` for
small tools that need register names, assembly spelling, and instruction
encodings. All 23 architecture sources have forms. The four `*.isa` files with
documentation or legacy format names are comment-only markers and are excluded
by `available()`.

Each source covers the instruction set it names. Where a form cannot be
described by this format the file header says so, and the reason is always the
same shape: the form is an assembler pseudo-instruction that expands to more
than one word, or its encoding puts two things in one field. RISC-V omits the
sign and zero extension pseudo-instructions for the first reason; MIPS omits
BAL for the second.

Native CPU encodings and the GCN gfx900 subset emit instruction bytes. Hexagon
encodings describe a single-instruction packet on v60. WebAssembly and SPIR-V
forms emit instruction fragments; their modules must be constructed separately.
PTX, DXIL, and TriCore are text-only: `render()` produces syntax and
`encode()` raises `ISAError`.

The parser and loader are in `../simple_isa.py` and need only Python 3:

```python
from qarch.simple_isa import available, load, load_arch

rv = load_arch("riscv64")  # or load("qarch/simple-isa/riscv64.isa")
assert rv.render("add", rd="a0", rs1="a1", rs2="a2") == "add a0, a1, a2"
assert rv.encode("add", rd="a0", rs1="a1", rs2="a2") == bytes.fromhex("3385c500")
print(rv.register("fp"), rv.operation("add"))

wasm = load_arch("wasm")
assert wasm.encode("i32_const", imm=-2) == bytes.fromhex("417e")
ptx = load_arch("nvptx")
assert ptx.render("ret") == "ret;"  # PTX has no target-independent binary opcode
```

From the repository root, run `python3 qarch/simple_isa.py --list` or
`python3 qarch/simple_isa.py qarch/simple-isa/aarch64.isa` to inspect sources.
Python imports work with `qarch.simple_isa` from the repository root.

## Header-only C++ API

`../simple_isa.hpp` requires C++20 and the sibling `qdsl/qDSL.hpp`. The
lexer uses qDSL's `ParsecInput`, `Parser`, and `run_parser`, while `SourceDSL`
extends its C++ DSL base and constructs the same validated model as `parse()`.
Include both directories when compiling outside CMake:

```sh
c++ -std=c++20 -I qarch -I qdsl app.cpp -o app
```

```cpp
#include "simple_isa.hpp"

namespace si = qarch::simple_isa;

auto rv = si::load_arch("riscv64", "qarch/simple-isa");
auto bytes = rv.encode("add", {{"rd", "a0"}, {"rs1", "a1"}, {"rs2", "a2"}});
auto assembly = rv.render("add", {{"rd", "a0"}, {"rs1", "a1"}, {"rs2", "a2"}});
// bytes == {0x33, 0x85, 0xc5, 0x00}; assembly == "add a0, a1, a2"

auto paths = si::available("qarch/simple-isa");
auto from_file = si::load_file(paths.at("aarch64"));
auto from_text = si::parse("arch demo { word_size = 32; addr_size = 32; "
                           "endian = little; align = 4; }");

auto native = si::SourceDSL{}
    .architecture("demo", 32, 32, "little", 4)
    .reg("r0", "GPR", 32, 0).reg("r1", "GPR", 32, 1)
    .encoding(si::Encoding{"ADD", si::Width::bits32, 0x33,
                           {{"rd", 7, 5, 0}, {"rs1", 15, 5, 0}}, {}})
    .form(si::Operation{"add", "arithmetic", "ADD", "add {rd}, {rs1}",
                        {{"rd", "GPR"}, {"rs1", "GPR"}}})
    .finish();
```

`available()` and `load_arch()` default to the source tree's `simple-isa`
directory. Supply the installed `share/domsymascc/simple-isa` directory when
using installed headers. `domsymas_simple_isa` is an independent CMake
interface target; install qDSL's public header alongside `simple_isa.hpp`.
Parsing and encoding errors throw `si::ISAError`.

## Syntax

Each file declares one `arch`, zero or more `regclass` blocks and `alias`
declarations, followed by `encoding` and `op` blocks. `#` starts a full-line
comment. An operation's `syntax` is an assembly template with **named**
placeholders (`{rd}`, `{rs1}`, `{imm}`), matching its operand declarations.
Operand types are `GPR`, `FPR`, `imm`, and `rel` (a signed PC-relative byte
displacement), plus `mem` for a memory reference. A `mem` operand is written
Intel-style, `[base+index*scale+disp]`, and the encoder turns it into a ModR/M
mod/rm pair, an SIB byte when an index is present, and a displacement; scale
must be 1, 2, 4 or 8, the index must be a low GPR other than `rsp`, and the
result is available as `ISA.memory(opname)`. BPF branch offsets use `imm`,
measured in BPF instruction units. A register alias keeps the canonical
register's encoding. PowerPC registers are passed as names like `r3` and
printed as the assembler operand `3`.

An `op` may carry directives of its own. `sp = rd, rs1` lists the operands
where register 31 is the stack pointer rather than the zero register, which is
how AArch64 spells the difference between `xzr` and `sp`; without it those
operands take the zero register and a mismatch is an error.

For fixed-size instructions, `base` contains the fixed opcode bits. Fields
such as `rs1 = 15:5` insert the named operand's low five bits at instruction
bit 15. A slice such as `offset[10:5] = 25:6` selects source bits 10 through
5 before inserting them at bit 25; the slice must be exactly as wide as the
field. Fields are disjoint, numbered from bit 0, and emitted in the
architecture's byte order. `signed = 1` bounds an immediate to its signed field
width; `alignment = N` checks immediate and relative operands for N-byte
alignment. Encodings without these directives use unsigned immediates. The
RISC-V `lui` and `auipc` operand is the unshifted upper 20-bit assembly
immediate. AArch64 `ldr`/`str` offsets are byte offsets and must be multiples of
eight, and the AArch64 branch offsets are byte offsets stored as a word count.
RISC-V `mul`/`div`/`rem` need the M extension; scalar `f*.s` and `f*.d`
need F and D respectively. The small loader describes their bytes but does
not select or verify processor extensions.

`width` is 16, 32, 48 or 64 bits, or one of `variable`, `stream` and `text`.
The 48-bit width is the six-byte instruction length s390x uses for its
storage, immediate and relative formats.

AMD64 encodings have `width = variable`. `opcode` contains one or more bytes;
`rex_w = 1` requests 64-bit operands; `modrm = rs1, rd` names the ModR/M
`reg` and `r/m` fields in that order. The first operand may also be `imm`,
representing an opcode-group selector checked to be in 0..7; it does not emit
an immediate byte. The second operand may be a register or `mem` reference.
`opreg = rd` adds the low three register bits to the opcode; `imm8`, `imm16`,
`imm32`, and `imm64` append an operand in little-endian order.
`rel32 = offset` appends a signed 32-bit displacement relative to the end of
the instruction. REX bits follow the register and memory operands. `prefix`
supports scalar SSE forms. Symbols and relaxation are outside this format.
A rendered numeric x86 `call`/`jmp` offset describes a byte displacement; an
external assembler may require a label. Near conditional branches use
`0x0f80` through `0x0f8f` followed by the signed 32-bit displacement.

`width = stream` encodings list `byte(...)` and `word(...)` emission or
signed/unsigned LEB128 operands with `sleb(...)` and `uleb(...)`.
`leb_bits = 32` bounds 32-bit LEB128 operands; `word(...)` emits an endian
aware 32-bit value. `width = text` is an assembly/IR-only form. The loader
does not track live stack state, SPIR-V module ids, register allocation,
processor features, shader capabilities, or relocations.

Malformed sources, unknown registers, incorrectly typed operands, field
overlaps, out-of-range immediates, and unaligned offsets raise `ISAError`.
For larger instruction inventories and SymAS databanks, use the existing
`.dsymspec` compiler documented in `../compiler/stdspec/README.md`.

## How the encodings were derived

Every opcode in the CPU sources is measured rather than transcribed: a probe
line is assembled with `llvm-mc -show-encoding`, the bits its operands drove
are cleared, and what remains is the `base` the encoding block declares. A bit
position written down from memory shows up as a disagreement with the
reference assembler rather than as a plausible wrong encoding.

The sources are cross-checked the same way. For each operation the harness
encodes concrete operand values, renders the operation's own `syntax` string,
and asks `llvm-mc` to assemble the same line; any difference in the bytes is a
mismatch. AArch64 is checked on 311 of its 315 operations, RISC-V, MIPS and BPF
on every form the harness can express, with no mismatches. The forms it cannot
express are the pseudo-instructions named in the file headers.
