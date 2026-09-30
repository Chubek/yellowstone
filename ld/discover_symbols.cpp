// discover_symbols.cpp - global discovery and archive member selection.
//
// Eager objects contribute their symbols immediately. Archives are scanned
// lazily: a member is pulled only when it defines a currently-undefined
// symbol (or, when the archive has no index, when any member defines one).
// The loop repeats until no new member is selected, matching the usual
// left-to-right archive semantics.
#include "internal.hpp"

namespace qld::detail {

namespace {

bool definesUndefined(const qbfd::ObjectFile& obj,
                      const std::vector<std::string>& undefined,
                      const std::map<std::string, Def>& defs) {
  for (const auto& s : obj.symbols()) {
    if (s.name.empty()) continue;
    if (s.flags & qbfd::sym::Undefined) continue;
    if (s.flags & qbfd::sym::Debug) continue;
    if (s.kind == qbfd::SymbolKind::File ||
        s.kind == qbfd::SymbolKind::Section)
      continue;
    if (s.binding != qbfd::SymbolBinding::Global &&
        s.binding != qbfd::SymbolBinding::Weak &&
        s.binding != qbfd::SymbolBinding::Unique)
      continue;
    if (defs.count(s.name)) continue;
    for (const auto& u : undefined)
      if (u == s.name) return true;
  }
  return false;
}

bool openCandidate(LinkState& state, ArchiveCandidate& cand,
                   const std::string& archivePath, std::string& error) {
  std::string label = archivePath + "(" + cand.member.name + ")";
  if (cand.bytes.empty()) {
    error = label + ": empty archive member";
    return false;
  }
  auto opened = qbfd::open(cand.bytes, cand.member.name);
  if (!opened) {
    error = label + ": " + opened.error().message;
    return false;
  }
  if (opened->object->format() != qbfd::Format::ELF) {
    error = label + ": only ELF archive members are supported";
    return false;
  }
  if (opened->object->is64Bit() != state.wide ||
      opened->object->endian() != state.endian) {
    error = label + ": archive member class/order mismatch";
    return false;
  }
  LoadedObject lo;
  lo.label = label;
  lo.archivePath = archivePath;
  lo.bytes = std::move(cand.bytes);
  lo.buffer = std::move(opened->buffer);
  lo.object = std::move(opened->object);
  lo.fromArchive = true;
  state.objects.push_back(std::move(lo));
  return true;
}

}  // namespace

bool discoverSymbols(LinkState& state, std::string& error) {
  std::vector<std::string> errors;
  // Pass 1: eager objects (including command-line members already expanded
  // by the loader as plain objects).
  for (size_t i = 0; i < state.objects.size(); ++i)
    addObjectSymbols(state, i, errors);
  if (!errors.empty()) {
    error = errors.front();
    for (size_t i = 1; i < errors.size(); ++i) error += "\n" + errors[i];
    return false;
  }

  // Pass 2: lazy archive extraction. Repeat until fixpoint: each newly
  // pulled member may reference further members.
  // To honour left-to-right order, scan archives in command-line order and
  // members in archive order, pulling at most one full sweep per iteration.
  bool progress = true;
  // Expand all archives once up front (payloads are small; members are only
  // parsed when selected).
  std::vector<std::vector<ArchiveCandidate>> candidates(state.archives.size());
  std::vector<std::vector<bool>> taken(state.archives.size());
  for (size_t a = 0; a < state.archives.size(); ++a) {
    candidates[a] = expandArchive(state.archives[a], error);
    if (!error.empty()) return false;
    for (auto& c : candidates[a]) c.archiveIndex = a;
    taken[a].assign(candidates[a].size(), false);
  }

  while (progress) {
    progress = false;
    for (size_t a = 0; a < state.archives.size(); ++a) {
      auto& arch = state.archives[a];
      const auto& index = arch.archive->symbolIndex();
      bool hasIndex = !index.empty();
      for (size_t m = 0; m < candidates[a].size(); ++m) {
        if (taken[a][m]) continue;
        bool needed = false;
        if (hasIndex) {
          // Use the armap: pull when the member is the index's recorded
          // definer of a needed symbol.
          for (const auto& e : index) {
            bool want = false;
            for (const auto& u : state.undefined)
              if (u == e.symbol) {
                want = true;
                break;
              }
            if (!want) continue;
            if (state.defs.count(e.symbol)) continue;
            const auto* def = arch.archive->memberAt(e.headerOffset);
            if (def && def->name == candidates[a][m].member.name) {
              needed = true;
              break;
            }
          }
          // Fall back to content sniffing when the index names nothing
          // needed but the member might still define a needed symbol
          // (e.g. weak-only or COMMON-only members some indexers skip).
          if (!needed) {
            auto probe = qbfd::open(candidates[a][m].bytes,
                                    candidates[a][m].member.name);
            if (probe && definesUndefined(*probe->object, state.undefined,
                                          state.defs))
              needed = true;
          }
        } else {
          // No index: scan every member.
          auto probe = qbfd::open(candidates[a][m].bytes,
                                  candidates[a][m].member.name);
          if (!probe) continue;  // unreadable member: skip, error later
          if (definesUndefined(*probe->object, state.undefined, state.defs))
            needed = true;
          else if (state.undefined.empty() && state.defs.empty()) {
            // First object from an index-less archive with no prior
            // symbols: GNU ld pulls the first member only when nothing
            // else references it? No: with no undefined symbols nothing
            // is pulled. Keep the standard behaviour: pull nothing.
          }
        }
        if (!needed) continue;
        if (!openCandidate(state, candidates[a][m], arch.path, error))
          return false;
        taken[a][m] = true;
        progress = true;
        std::vector<std::string> errs;
        addObjectSymbols(state, state.objects.size() - 1, errs);
        if (!errs.empty()) {
          error = errs.front();
          return false;
        }
      }
    }
  }

  if (!resolveCommons(state, error)) return false;

  // Final undefined check (deferred so --allow-undefined can pass).
  if (!state.options.allowUndefined && state.options.mode != Mode::Relocatable) {
    // Shared links may leave dynamic undefined symbols; executables may not.
    std::vector<std::string> missing;
    for (const auto& u : state.undefined) {
      if (state.defs.count(u)) continue;
      missing.push_back(u);
    }
    if (!missing.empty()) {
      error = "undefined reference to `" + missing.front() + "'";
      for (size_t i = 1; i < missing.size(); ++i)
        error += "\nundefined reference to `" + missing[i] + "'";
      return false;
    }
  }
  return true;
}

}  // namespace qld::detail
