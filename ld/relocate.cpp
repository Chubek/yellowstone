// relocate.cpp - relocation application.
//
// Supports the x86/x64 static relocations the fd readers decode:
//   R_X86_64_64 / R_X86_64_32 / R_X86_64_32S (absolute)
//   R_X86_64_PC32 / R_X86_64_PLT32 (PC-relative; PLT32 relaxes to PC32 in a
//     static link since no PLT is emitted)
//   R_386_32 / R_386_PC32
//   R_X86_64_RELATIVE is never present in ET_REL inputs; it is rejected.
//
// Other types are reported as errors when they occur in an allocated
// section, and ignored in debug sections (which are dropped from the
// executable image when --strip-debug, or copied verbatim otherwise).
#include "internal.hpp"

namespace qld::detail {

namespace {

void writeLE(std::vector<uint8_t>& buf, uint64_t off, uint64_t value,
             unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    buf[off + i] = uint8_t(value >> (8 * i));
}

void writeBE(std::vector<uint8_t>& buf, uint64_t off, uint64_t value,
             unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    buf[off + i] = uint8_t(value >> (8 * (width - i - 1)));
}

void writeVal(std::vector<uint8_t>& buf, uint64_t off, uint64_t value,
              unsigned width, qbfd::Endian order) {
  if (order == qbfd::Endian::Little)
    writeLE(buf, off, value, width);
  else
    writeBE(buf, off, value, width);
}

uint64_t readVal(const std::vector<uint8_t>& buf, uint64_t off, unsigned width,
                 qbfd::Endian order) {
  uint64_t v = 0;
  if (order == qbfd::Endian::Little) {
    for (unsigned i = 0; i < width; ++i) v |= uint64_t(buf[off + i]) << (8 * i);
  } else {
    for (unsigned i = 0; i < width; ++i) v = (v << 8) | buf[off + i];
  }
  return v;
}

}  // namespace

bool applyRelocations(LinkState& state,
                      std::vector<std::vector<uint8_t>>& staged,
                      std::string& error) {
  // staged[i] parallels state.outSections[i]: mutable section bytes.
  if (staged.size() != state.outSections.size()) {
    error = "internal error: staged image size mismatch";
    return false;
  }
  bool isX86_64 =
      state.arch == qbfd::Arch::X86_64 || state.wide;
  bool isX86 = state.arch == qbfd::Arch::X86;
  bool relocatable = state.options.mode == Mode::Relocatable;

  for (const auto& in : state.inputSections) {
    if (!in.kept) continue;
    if (in.relocs.empty()) continue;
    // Output section + base for this input.
    size_t outIdx = SIZE_MAX;
    for (size_t o = 0; o < state.outSections.size(); ++o) {
      for (size_t idx : state.outSections[o].inputs) {
        if (state.inputSections[idx].objectIndex == in.objectIndex &&
            state.inputSections[idx].sectionIndex == in.sectionIndex) {
          outIdx = o;
          break;
        }
      }
      if (outIdx != SIZE_MAX) break;
    }
    if (outIdx == SIZE_MAX) continue;  // dropped section
    const OutputSection& o = state.outSections[outIdx];
    const auto& obj = state.objects[in.objectIndex];
    uint64_t place = o.vma + in.outOffset;  // P for PC-relative
    std::vector<uint8_t>& img = staged[outIdx];
    uint64_t baseOff = 0;
    // intra-output offset of this input's bytes:
    for (size_t idx : o.inputs) {
      if (idx == (size_t)(&in - state.inputSections.data())) break;
      (void)idx;
    }
    // Recompute input base offset within staged image.
    {
      uint64_t off = 0;
      for (size_t idx : o.inputs) {
        const auto& other = state.inputSections[idx];
        if (idx == (size_t)(&in - state.inputSections.data())) {
          baseOff = off;
          break;
        }
        uint64_t a = other.section.alignment ? other.section.alignment : 1;
        off = ((off + a - 1) / a) * a + std::max<uint64_t>(
            other.section.size, other.contents.size());
      }
    }

    for (const auto& r : in.relocs) {
      // Resolve the symbol value S.
      uint64_t S = 0;
      std::string symName;
      bool haveSym = false;
      if (r.symbol) {
        const auto& syms = obj.object->symbols();
        const qbfd::Symbol* sym = nullptr;
        if (*r.symbol < syms.size()) sym = &syms[*r.symbol];
        if (!sym) {
          error = obj.label + ": relocation names bad symbol";
          return false;
        }
        symName = sym->name;
        if (sym->flags & qbfd::sym::Undefined) {
          auto it = state.symAddr.find(sym->name);
          if (it == state.symAddr.end()) {
            if (state.options.allowUndefined ||
                state.options.mode == Mode::Relocatable) {
              S = 0;
            } else {
              error = obj.label + ": undefined reference to `" + sym->name +
                      "'";
              return false;
            }
          } else {
            S = it->second;
          }
          haveSym = true;
        } else if (sym->kind == qbfd::SymbolKind::Section ||
                   sym->kind == qbfd::SymbolKind::File) {
          // Section symbol: S = owning output section base + value.
          if (sym->section) {
            bool found = false;
            for (size_t oi2 = 0; oi2 < state.outSections.size(); ++oi2) {
              for (size_t idx : state.outSections[oi2].inputs) {
                const auto& other = state.inputSections[idx];
                if (other.objectIndex == in.objectIndex &&
                    other.sectionIndex == *sym->section) {
                  S = state.outSections[oi2].vma + other.outOffset +
                      sym->value;
                  found = true;
                  break;
                }
              }
              if (found) break;
            }
            if (!found) S = sym->value;
          } else {
            S = sym->value;
          }
          haveSym = true;
        } else {
          auto it = state.symAddr.find(sym->name);
          if (it != state.symAddr.end()) {
            S = it->second;
          } else {
            // Local symbol not in the global table: compute directly.
            if (sym->section) {
              bool found = false;
              for (size_t oi2 = 0; oi2 < state.outSections.size(); ++oi2) {
                for (size_t idx : state.outSections[oi2].inputs) {
                  const auto& other = state.inputSections[idx];
                  if (other.objectIndex == in.objectIndex &&
                      other.sectionIndex == *sym->section) {
                    S = state.outSections[oi2].vma + other.outOffset +
                        sym->value;
                    found = true;
                    break;
                  }
                }
                if (found) break;
              }
              if (!found) S = sym->value;
            } else {
              S = sym->value;
            }
          }
          haveSym = true;
        }
      }
      (void)haveSym;
      int64_t A = r.hasAddend ? r.addend : 0;
      // Implicit addends for REL inputs are already in the section bytes;
      // qELF decodes them into addend when the type is recognised, so we
      // must not double-count: when hasAddend came from decoding (REL),
      // the bytes still hold the original value and the formula below
      // overwrites them, which is correct.
      uint64_t P = place + r.offset;
      uint64_t loc = baseOff + r.offset;
      uint32_t type = r.nativeType;

      auto needBytes = [&](unsigned w) -> bool {
        if (loc + w > img.size()) {
          error = obj.label + ": relocation offset outside section " +
                  in.section.name;
          return false;
        }
        return true;
      };

      if (relocatable) {
        // Partial link: leave relocations for later; nothing to apply.
        continue;
      }

      if (isX86_64 || (!isX86 && state.wide)) {
        switch (type) {
          case 1: {  // R_X86_64_64
            if (!needBytes(8)) return false;
            uint64_t v = S + (uint64_t)(int64_t)A;
            // For REL (no explicit addend decoded), A already includes the
            // in-place value, but we overwrote? qELF sets hasAddend for
            // RELA always and for REL only when decoded. When not decoded
            // (unknown), we read the in-place value here.
            if (!r.hasAddend) {
              uint64_t inplace = readVal(img, loc, 8, state.endian);
              v = S + inplace;
            }
            writeVal(img, loc, v, 8, state.endian);
            break;
          }
          case 10: {  // R_X86_64_32
            if (!needBytes(4)) return false;
            uint64_t v = S + (uint64_t)(int64_t)A;
            if (!r.hasAddend) v = S + readVal(img, loc, 4, state.endian);
            if (v >> 32) {
              error = obj.label + ": relocation truncated to fit: " +
                      "R_X86_64_32 against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, v & 0xffffffffu, 4, state.endian);
            break;
          }
          case 11: {  // R_X86_64_32S
            if (!needBytes(4)) return false;
            int64_t v = (int64_t)S + A;
            if (!r.hasAddend)
              v = (int64_t)S + (int64_t)(int32_t)readVal(img, loc, 4,
                                                         state.endian);
            if (v != (int64_t)(int32_t)v) {
              error = obj.label + ": relocation truncated to fit: " +
                      "R_X86_64_32S against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, (uint64_t)(uint32_t)v, 4, state.endian);
            break;
          }
          case 2:   // R_X86_64_PC32
          case 4: {  // R_X86_64_PLT32 (relaxes to PC32 statically)
            if (!needBytes(4)) return false;
            int64_t v = (int64_t)S + A - (int64_t)P;
            if (!r.hasAddend) {
              int32_t inplace =
                  (int32_t)readVal(img, loc, 4, state.endian);
              v = (int64_t)S + inplace - (int64_t)P;
            }
            if (v != (int64_t)(int32_t)v) {
              error = obj.label + ": relocation truncated to fit: " +
                      std::string(type == 2 ? "R_X86_64_PC32" : "R_X86_64_PLT32") +
                      " against `" + symName + "'";
              return false;
            }
            writeVal(img, loc, (uint64_t)(uint32_t)v, 4, state.endian);
            break;
          }
          case 8: {  // R_X86_64_RELATIVE in ET_REL: unexpected
            error = obj.label + ": unsupported R_X86_64_RELATIVE in " +
                    "relocatable input";
            return false;
          }
          default: {
            // Unknown type in an alloc section is fatal; in debug, skip.
            if (o.alloc) {
              error = obj.label + ": unsupported relocation type " +
                      std::to_string(type) + " in " + in.section.name +
                      " against `" + symName + "'";
              return false;
            }
            break;
          }
        }
      } else {
        // 32-bit x86.
        switch (type) {
          case 1: {  // R_386_32
            if (!needBytes(4)) return false;
            uint64_t v = S + (uint64_t)(int64_t)A;
            if (!r.hasAddend) v = S + readVal(img, loc, 4, state.endian);
            writeVal(img, loc, v & 0xffffffffu, 4, state.endian);
            break;
          }
          case 2: {  // R_386_PC32
            if (!needBytes(4)) return false;
            int64_t v = (int64_t)S + A - (int64_t)P;
            if (!r.hasAddend) {
              int32_t inplace =
                  (int32_t)readVal(img, loc, 4, state.endian);
              v = (int64_t)S + inplace - (int64_t)P;
            }
            writeVal(img, loc, (uint64_t)(uint32_t)v, 4, state.endian);
            break;
          }
          default: {
            if (o.alloc) {
              error = obj.label + ": unsupported relocation type " +
                      std::to_string(type) + " in " + in.section.name;
              return false;
            }
            break;
          }
        }
      }
    }
  }
  return true;
}

}  // namespace qld::detail
