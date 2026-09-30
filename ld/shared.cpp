// shared.cpp - shared-object / PIE / relocatable drivers.
//
// -shared and -pie share the executable emitter with a zero base and ET_DYN;
// -r delegates to the relocatable emitter in binarizer.cpp.
#include "internal.hpp"

namespace qld::detail {

bool linkShared(LinkState& state, std::vector<uint8_t>& out,
                std::string& error) {
  // Shared links default to base 0 and allow undefined symbols (resolved
  // at load time).
  if (!state.options.hasBase) {
    state.options.base = 0;
    state.options.hasBase = true;
  }
  bool savedAllow = state.options.allowUndefined;
  state.options.allowUndefined = true;
  if (!assignAddresses(state, error)) return false;
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
  // Undefined symbols are tolerated for shared; applyRelocations leaves
  // them as 0 with allowUndefined set. Restore afterwards for reporting.
  std::string relErr;
  bool ok = applyRelocations(state, staged, relErr);
  state.options.allowUndefined = savedAllow;
  if (!ok) {
    error = relErr;
    return false;
  }
  if (!emitShared(state, staged, out, error)) return false;
  return true;
}

bool linkRelocatable(LinkState& state, std::vector<uint8_t>& out,
                     std::string& error) {
  if (!assignAddresses(state, error)) return false;
  if (!emitRelocatable(state, out, error)) return false;
  return true;
}

}  // namespace qld::detail
