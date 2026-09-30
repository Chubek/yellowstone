// driver.cpp - top-level link orchestration.
//
// loadInputs -> loadScriptExtras -> discoverSymbols -> mode driver
// (static/shared/relocatable) -> optional -Map write. All file I/O for the
// one-shot API lives in ld.c; this file works on LinkState and byte vectors.
#include "internal.hpp"

#include <fstream>

namespace qld::detail {

static bool writeFile(const std::string& path, const std::vector<uint8_t>& b,
                      std::string& error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(b.data()),
            std::streamsize(b.size()));
  out.close();
  if (!out) {
    error = "write failed: " + path;
    return false;
  }
  return true;
}

bool linkState(LinkState& state, std::vector<uint8_t>& out,
               std::string& error) {
  if (!loadInputs(state, error)) return false;
  if (hasScript(state)) {
    if (!loadScriptExtras(state, error)) return false;
    // Geometry may have changed with new objects; revalidate class/order.
    for (const auto& o : state.objects) {
      if (o.isScript || !o.object) continue;
      if (o.object->is64Bit() != state.wide ||
          o.object->endian() != state.endian) {
        error = o.label + ": class/order mismatch with script inputs";
        return false;
      }
    }
  }
  if (!discoverSymbols(state, error)) return false;
  bool ok = false;
  switch (state.options.mode) {
    case Mode::Exec: ok = linkStatic(state, out, error); break;
    case Mode::Shared: ok = linkShared(state, out, error); break;
    case Mode::Relocatable: ok = linkRelocatable(state, out, error); break;
  }
  if (!ok) return false;
  if (!state.options.mapFile.empty()) {
    std::string m = state.mapText;
    if (m.empty()) m = "(empty map)\n";
    std::vector<uint8_t> mb(m.begin(), m.end());
    std::string werr;
    if (!writeFile(state.options.mapFile, mb, werr)) {
      state.warnings.push_back(werr);
    }
  }
  return true;
}

}  // namespace qld::detail
