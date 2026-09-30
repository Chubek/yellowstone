// static.cpp - static executable link driver.
//
// Orchestrates layout, relocation and emission for ET_EXEC (and PIE, which
// shares the emitter with a zero base).
#include "internal.hpp"

namespace qld::detail {

bool linkStatic(LinkState& state, std::vector<uint8_t>& out,
                std::string& error) {
  if (!assignAddresses(state, error)) return false;
  // Stage section bytes then relocate in place.
  std::vector<std::vector<uint8_t>> staged(state.outSections.size());
  for (size_t o = 0; o < state.outSections.size(); ++o) {
    const auto& sec = state.outSections[o];
    if (sec.bss) continue;
    std::vector<uint8_t> buf(size_t(sec.size), 0);
    for (size_t idx : sec.inputs) {
      const auto& in = state.inputSections[idx];
      size_t n = in.contents.size();
      size_t cap = size_t(sec.size) > in.outOffset
                       ? size_t(sec.size - in.outOffset)
                       : 0;
      if (n > cap) n = cap;
      if (n) std::copy(in.contents.begin(), in.contents.begin() + n,
                       buf.begin() + ptrdiff_t(in.outOffset));
    }
    staged[o] = std::move(buf);
  }
  if (!applyRelocations(state, staged, error)) return false;
  if (!emitExecutable(state, staged, out, error)) return false;
  return true;
}

}  // namespace qld::detail
