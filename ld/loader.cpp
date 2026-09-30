// loader.cpp - loading input files via the header-only readers in fd/.
//
// Plain objects are opened with qbfd::open(); archives with
// qbfd::openArchive(). Thin archives resolve relative to their own
// directory inside qArchive.hpp, so no extra handling is needed here.
// Linker scripts (GROUP/INPUT text) are detected by content sniffing and
// deferred to ldscript.cpp; -l names are resolved against -L paths.
#include "internal.hpp"

#include <fstream>
#include <sstream>

namespace qld::detail {

namespace {

bool isScriptText(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) return false;
  // A script is ASCII text mentioning one of the script keywords. Object
  // files start with ELF/MZ/BSD magic and never contain these words at
  // the top level, so a cheap substring test is sufficient and never
  // misclassifies a binary: binaries may contain the words, but they also
  // parse as objects first, and scripts never parse as objects.
  std::string head(reinterpret_cast<const char*>(bytes.data()),
                   std::min<size_t>(bytes.size(), 4096));
  for (char& c : head) {
    if (c == '\0') return false;  // binary
  }
  auto has = [&](const char* w) { return head.find(w) != std::string::npos; };
  return has("ENTRY") || has("SECTIONS") || has("MEMORY") || has("GROUP") ||
         has("INPUT") || has("OUTPUT_FORMAT") || has("SEARCH_DIR") ||
         has("PHDRS") || has("VERSION");
}

std::vector<uint8_t> readAll(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return {};
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (in.bad()) {
    error = "read failed: " + path;
    return {};
  }
  return bytes;
}

bool openOneObject(LinkState& state, std::vector<uint8_t> bytes,
                   const std::string& label, const std::string& archivePath,
                   std::string& error) {
  if (bytes.empty()) {
    error = label + ": empty input";
    return false;
  }
  // Archives are handled by the caller; here we only accept objects.
  if (qbfd::isArchive(bytes)) {
    error = label + ": unexpected archive in object slot";
    return false;
  }
  auto opened = qbfd::open(bytes, label);
  if (!opened) {
    // Not an object: maybe a linker script.
    std::vector<uint8_t> probe = bytes;
    if (isScriptText(probe)) {
      LoadedObject lo;
      lo.label = label;
      lo.bytes = std::move(bytes);
      lo.isScript = true;
      lo.fromArchive = false;
      state.objects.push_back(std::move(lo));
      return true;
    }
    error = label + ": " + opened.error().message;
    return false;
  }
  LoadedObject lo;
  lo.label = label;
  lo.archivePath = archivePath;
  lo.bytes = std::move(bytes);
  lo.buffer = std::move(opened->buffer);
  lo.object = std::move(opened->object);
  lo.fromArchive = !archivePath.empty();
  // Only ELF relocatables/executables participate in the link. Shared
  // inputs are accepted for symbol provision but not relocated here;
  // other formats are reported rather than approximated.
  state.objects.push_back(std::move(lo));
  if (!state.objects.back().object) {
    error = label + ": backend returned a null object";
    state.objects.pop_back();
    return false;
  }
  return true;
}

std::string resolveLib(const LinkState& state, const std::string& name,
                       std::string& error) {
  std::vector<std::string> candidates;
  candidates.push_back("lib" + name + ".a");
  candidates.push_back("lib" + name + ".so");
  std::vector<std::string> dirs = state.options.libPaths;
  dirs.insert(dirs.begin(), std::string("."));
  // Scripts may add SEARCH_DIR entries; those are appended to libPaths by
  // ldscript.cpp before archives are resolved, so reading them here is
  // sufficient.
  for (const auto& dir : dirs) {
    for (const auto& cand : candidates) {
      std::filesystem::path p = std::filesystem::path(dir) / cand;
      std::error_code ec;
      if (std::filesystem::is_regular_file(p, ec)) return p.string();
    }
  }
  // Also try the bare name as a path.
  {
    std::error_code ec;
    if (std::filesystem::is_regular_file(name, ec)) return name;
  }
  error = "-l" + name + ": cannot find library";
  return {};
}

}  // namespace

bool isArchivePath(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  char magic[8] = {};
  in.read(magic, 8);
  if (size_t(in.gcount()) < 8) return false;
  return std::string_view(magic, 8) == "!<arch>\n" ||
         std::string_view(magic, 8) == "!<thin>\n";
}

const LoadedArchive* findArchive(const LinkState& state,
                                 const std::string& path) {
  for (const auto& a : state.archives)
    if (a.path == path) return &a;
  return nullptr;
}

bool loadInputs(LinkState& state, std::string& error) {
  // Inline -T text behaves as an extra GROUP script processed before layout.
  // It is kept in options; loader only handles file inputs here.
  std::vector<std::string> queue = state.options.inputs;
  // -l libraries resolve to files and join the queue in order.
  for (const auto& lib : state.options.libs) {
    std::string resolved = resolveLib(state, lib, error);
    if (resolved.empty()) return false;
    queue.push_back(resolved);
  }

  for (const auto& path : queue) {
    std::vector<uint8_t> bytes = readAll(path, error);
    if (!error.empty() && bytes.empty()) {
      // readAll sets error only on failure; distinguish empty file.
      if (error.find("cannot open") != std::string::npos ||
          error.find("read failed") != std::string::npos)
        return false;
      error.clear();
    }
    if (bytes.empty() && error.empty()) {
      // Empty file: treat as error (an empty archive member is different
      // and handled inside archive expansion).
      std::error_code ec;
      auto sz = std::filesystem::file_size(path, ec);
      if (!ec && sz == 0) {
        error = path + ": empty input";
        return false;
      }
    }
    if (qbfd::isArchive(bytes)) {
      auto opened = qbfd::openArchive(path);
      if (!opened) {
        error = path + ": " + opened.error().message;
        return false;
      }
      LoadedArchive la;
      la.path = path;
      auto buf = qbfd::Buffer::fromFile(path);
      if (!buf) {
        error = path + ": " + buf.error().message;
        return false;
      }
      la.buffer = std::move(*buf);
      la.bytes.assign(la.buffer->span().begin(), la.buffer->span().end());
      la.archive = std::move(opened->archive);
      state.archives.push_back(std::move(la));
      continue;
    }
    if (!openOneObject(state, std::move(bytes), path, {}, error)) return false;
  }

  if (state.objects.empty() && state.archives.empty()) {
    error = "no input files";
    return false;
  }
  // Establish a common ELF geometry from the first real object. Archives
  // contribute later during member selection; their geometry must match.
  for (const auto& o : state.objects) {
    if (o.isScript || !o.object) continue;
    if (o.object->format() != qbfd::Format::ELF) {
      error = o.label + ": only ELF inputs are supported (got " +
              std::string(qbfd::toString(o.object->format())) + ")";
      return false;
    }
    state.wide = o.object->is64Bit();
    state.endian = o.object->endian();
    state.arch = o.object->arch();
    break;
  }
  // Validate the rest eagerly so mixed-class links fail fast.
  for (const auto& o : state.objects) {
    if (o.isScript || !o.object) continue;
    if (o.object->format() != qbfd::Format::ELF) {
      error = o.label + ": only ELF inputs are supported";
      return false;
    }
    if (o.object->is64Bit() != state.wide) {
      error = o.label + ": mixing 32-bit and 64-bit objects is unsupported";
      return false;
    }
    if (o.object->endian() != state.endian) {
      error = o.label + ": mixing byte orders is unsupported";
      return false;
    }
  }
  return true;
}

}  // namespace qld::detail
