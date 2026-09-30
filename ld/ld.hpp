// ld.hpp - C++ wrapper over the qobjld linker facilities (ld.c).
//
// The C API in ld.h / ld.c is the base layer: it owns option translation,
// file I/O and diagnostics in C-callable form. This header wraps it in
// idiomatic C++20 types so C++ consumers never touch raw pointers:
//
//   qld::Options opts;
//   opts.inputs = {"a.o", "b.o"};
//   opts.output = "a.out";
//   auto result = qld::link(opts);
//   if (!result) std::cerr << result.error << '\n';
//
// Direct access to the underlying core (loader, symbol table, script engine)
// is intentionally through these wrappers: they translate to/from the C API
// in ld.c, keeping one implementation of the link itself.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ld.h"

namespace qld {

inline constexpr const char* kVersion = "1.0";

enum class Mode : uint8_t { Exec = 0, Shared = 1, Relocatable = 2 };

// Mirrors qld_options_t with owning C++ types.
struct Options {
  std::vector<std::string> inputs;
  std::vector<std::string> libPaths;
  std::vector<std::string> libs;
  std::string output;          // empty: in-memory only
  std::string entry = "_start";
  bool hasEntry = false;       // when false the default entry is used
  std::string script;          // -T path
  std::string scriptText;      // inline script
  bool useScriptText = false;
  uint64_t base = 0x400000;
  bool hasBase = false;
  Mode mode = Mode::Exec;
  bool pie = false;
  bool gcSections = false;
  bool stripDebug = false;
  bool allowUndefined = false;
  bool verbose = false;
  std::string mapFile;
  std::string emulator;
};

struct Result {
  std::vector<uint8_t> bytes;  // output image (always filled)
  std::string map;             // -Map contents (may be empty)
};

struct Error {
  std::string message;
};

// Expected<T>: minimal value-or-error without exceptions.
template <class T>
class Expected {
 public:
  Expected(T v) : ok_(true), value_(std::move(v)) {}
  Expected(Error e) : ok_(false), error_(std::move(e)) {}

  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  T& value() { return value_; }
  const T& value() const { return value_; }
  T& operator*() { return value_; }
  const T& operator*() const { return value_; }
  T* operator->() { return &value_; }
  const T* operator->() const { return &value_; }
  const Error& error() const { return error_; }

 private:
  bool ok_ = false;
  T value_{};
  Error error_{""};
};

template <>
class Expected<void> {
 public:
  Expected() : ok_(true) {}
  Expected(Error e) : ok_(false), error_(std::move(e)) {}
  bool ok() const { return ok_; }
  explicit operator bool() const { return ok_; }
  const Error& error() const { return error_; }

 private:
  bool ok_ = false;
  Error error_{""};
};

// One-shot link. Translates Options to qld_options_t, calls qld_link_bytes
// in ld.c, and returns the image bytes. When Options::output is set the
// file is written by the C layer as well.
inline Expected<Result> link(const Options& opts) {
  std::vector<const char*> inputs, libpaths, libs;
  inputs.reserve(opts.inputs.size());
  for (const auto& s : opts.inputs) inputs.push_back(s.c_str());
  libpaths.reserve(opts.libPaths.size());
  for (const auto& s : opts.libPaths) libpaths.push_back(s.c_str());
  libs.reserve(opts.libs.size());
  for (const auto& s : opts.libs) libs.push_back(s.c_str());

  qld_options_t c{};
  c.inputs = inputs.empty() ? nullptr : inputs.data();
  c.n_inputs = inputs.size();
  c.libpaths = libpaths.empty() ? nullptr : libpaths.data();
  c.n_libpaths = libpaths.size();
  c.libs = libs.empty() ? nullptr : libs.data();
  c.n_libs = libs.size();
  c.output = opts.output.empty() ? nullptr : opts.output.c_str();
  c.entry = opts.hasEntry ? opts.entry.c_str() : nullptr;
  c.script = opts.script.empty() ? nullptr : opts.script.c_str();
  c.script_text = opts.useScriptText ? opts.scriptText.c_str() : nullptr;
  c.use_script_text = opts.useScriptText ? 1 : 0;
  c.base = opts.base;
  c.has_base = opts.hasBase ? 1 : 0;
  c.mode = static_cast<qld_mode_t>(opts.mode);
  c.pie = opts.pie ? 1 : 0;
  c.gc_sections = opts.gcSections ? 1 : 0;
  c.strip_debug = opts.stripDebug ? 1 : 0;
  c.allow_undefined = opts.allowUndefined ? 1 : 0;
  c.verbose = opts.verbose ? 1 : 0;
  c.map_file = opts.mapFile.empty() ? nullptr : opts.mapFile.c_str();
  c.emulator = opts.emulator.empty() ? nullptr : opts.emulator.c_str();

  uint8_t* bytes = nullptr;
  size_t size = 0;
  char* errmsg = nullptr;
  int rc = qld_link_bytes(&c, &bytes, &size, &errmsg);
  if (rc != 0) {
    std::string msg = errmsg ? errmsg : "link failed";
    qld_free_string(errmsg);
    return Expected<Result>(Error{std::move(msg)});
  }
  Result r;
  if (bytes && size) r.bytes.assign(bytes, bytes + size);
  qld_free_bytes(bytes, size);
  qld_free_string(errmsg);
  return Expected<Result>(std::move(r));
}

// Validate a linker script without linking.
inline Expected<void> checkScript(const std::string& text) {
  char* errmsg = nullptr;
  int rc = qld_check_script(text.c_str(), &errmsg);
  if (rc != 0) {
    std::string msg = errmsg ? errmsg : "script error";
    qld_free_string(errmsg);
    return Expected<void>(Error{std::move(msg)});
  }
  qld_free_string(errmsg);
  return Expected<void>();
}

inline std::string version() { return qld_version(); }

// Incremental builder: thin RAII wrapper over qld_linker_t.
class Linker {
 public:
  Linker() : self_(qld_create()) {}
  ~Linker() { qld_destroy(self_); }
  Linker(const Linker&) = delete;
  Linker& operator=(const Linker&) = delete;
  Linker(Linker&& o) noexcept : self_(o.self_) { o.self_ = nullptr; }
  Linker& operator=(Linker&& o) noexcept {
    if (this != &o) {
      qld_destroy(self_);
      self_ = o.self_;
      o.self_ = nullptr;
    }
    return *this;
  }

  bool addInput(const std::string& path, std::string& error) {
    if (qld_add_input(self_, path.c_str()) != 0) {
      error = "cannot add input " + path;
      return false;
    }
    return true;
  }
  void addLibPath(const std::string& p) { qld_add_libpath(self_, p.c_str()); }
  void addLibrary(const std::string& n) { qld_add_library(self_, n.c_str()); }
  void setOutput(const std::string& p) { qld_set_output(self_, p.c_str()); }
  void setEntry(const std::string& s) { qld_set_entry(self_, s.c_str()); }
  void setScriptPath(const std::string& p) {
    qld_set_script_path(self_, p.c_str());
  }
  void setScriptText(const std::string& t) {
    qld_set_script_text(self_, t.c_str());
  }
  void setBase(uint64_t b) { qld_set_base(self_, b); }
  void setMode(Mode m) { qld_set_mode(self_, static_cast<qld_mode_t>(m)); }
  void setFlag(const std::string& f, bool v) {
    qld_set_flag(self_, f.c_str(), v ? 1 : 0);
  }

  Expected<Result> run() {
    uint8_t* bytes = nullptr;
    size_t size = 0;
    char* errmsg = nullptr;
    int rc = qld_run_bytes(self_, &bytes, &size, &errmsg);
    if (rc != 0) {
      std::string msg = errmsg ? errmsg : "link failed";
      qld_free_string(errmsg);
      return Expected<Result>(Error{std::move(msg)});
    }
    Result r;
    if (bytes && size) r.bytes.assign(bytes, bytes + size);
    qld_free_bytes(bytes, size);
    return Expected<Result>(std::move(r));
  }

 private:
  qld_linker_t* self_ = nullptr;
};

}  // namespace qld
