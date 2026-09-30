// ld.c - C API implementation for the qobjld linker.
//
// Compiled as C++ (see ld/CMakeLists.txt LANGUAGE property) so it can use
// the header-only readers in ../fd/ through the internal driver. The public
// contract is the C header ld.h; the C++ wrapper ld.hpp calls these
// functions.
#include "ld.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "internal.hpp"
#include "parser.hpp"

extern const char* qld_detail_version();

namespace {

void setErr(char** errmsg, const std::string& msg) {
  if (!errmsg) return;
  *errmsg = nullptr;
  if (msg.empty()) return;
  char* p = (char*)std::malloc(msg.size() + 1);
  if (!p) return;
  std::memcpy(p, msg.c_str(), msg.size() + 1);
  *errmsg = p;
}

qld::detail::LinkOptions toInternal(const qld_options_t* o) {
  qld::detail::LinkOptions in;
  if (!o) return in;
  for (size_t i = 0; i < o->n_inputs; ++i)
    if (o->inputs[i]) in.inputs.emplace_back(o->inputs[i]);
  for (size_t i = 0; i < o->n_libpaths; ++i)
    if (o->libpaths[i]) in.libPaths.emplace_back(o->libpaths[i]);
  for (size_t i = 0; i < o->n_libs; ++i)
    if (o->libs[i]) in.libs.emplace_back(o->libs[i]);
  if (o->output) in.output = o->output;
  if (o->entry) in.entry = o->entry;
  if (o->script) in.script = o->script;
  if (o->use_script_text && o->script_text) {
    in.scriptText = o->script_text;
    in.scriptIsText = true;
  }
  if (o->has_base) {
    in.base = o->base;
    in.hasBase = true;
  }
  switch (o->mode) {
    case QLD_MODE_SHARED: in.mode = qld::detail::Mode::Shared; break;
    case QLD_MODE_RELOCATABLE:
      in.mode = qld::detail::Mode::Relocatable;
      break;
    default: in.mode = qld::detail::Mode::Exec; break;
  }
  in.pie = o->pie != 0;
  if (in.pie && in.mode == qld::detail::Mode::Exec) {
    // PIE executables link as shared-type images with an entry point.
    in.mode = qld::detail::Mode::Shared;
  }
  in.gcSections = o->gc_sections != 0;
  in.stripDebug = o->strip_debug != 0;
  in.allowUndefined = o->allow_undefined != 0;
  in.verbose = o->verbose != 0;
  if (o->map_file) in.mapFile = o->map_file;
  if (o->emulator) in.emulator = o->emulator;
  return in;
}

bool writeOutput(const std::string& path, const std::vector<uint8_t>& bytes,
                 std::string& error) {
  if (path.empty()) return true;
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "cannot write " + path;
    return false;
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            std::streamsize(bytes.size()));
  out.close();
  if (!out) {
    error = "write failed: " + path;
    return false;
  }
#ifndef _WIN32
  // Linked executables should run: mirror the input mode with +x.
  ::chmod(path.c_str(), 0755);
#endif
  return true;
}

int runInternal(const qld_options_t* opts, std::vector<uint8_t>& outBytes,
                char** errmsg) {
  if (!opts) {
    setErr(errmsg, "null options");
    return 1;
  }
  qld::detail::LinkState state;
  state.options = toInternal(opts);
  std::string error;
  std::vector<uint8_t> bytes;
  if (!qld::detail::linkState(state, bytes, error)) {
    setErr(errmsg, error.empty() ? "link failed" : error);
    return 1;
  }
  // Write -o and -Map side effects here so both C and C++ paths share them.
  if (!state.options.output.empty()) {
    if (!writeOutput(state.options.output, bytes, error)) {
      setErr(errmsg, error);
      return 1;
    }
  }
  if (state.options.verbose && !state.warnings.empty()) {
    std::string w;
    for (const auto& s : state.warnings) w += "qobjld: warning: " + s + "\n";
    // Warnings do not fail the link; surface through errmsg as info? Keep
    // stdout behaviour to the CLI; the API stays quiet.
    (void)w;
  }
  outBytes = std::move(bytes);
  if (errmsg) *errmsg = nullptr;
  return 0;
}

}  // namespace

const char* qld_version(void) { return "1.0"; }

void qld_free_string(char* s) { std::free(s); }

void qld_free_bytes(uint8_t* p, size_t n) {
  (void)n;
  std::free(p);
}

int qld_link(const qld_options_t* opts, char** errmsg) {
  std::vector<uint8_t> bytes;
  return runInternal(opts, bytes, errmsg);
}

int qld_link_bytes(const qld_options_t* opts, uint8_t** out_bytes,
                   size_t* out_size, char** errmsg) {
  std::vector<uint8_t> bytes;
  int rc = runInternal(opts, bytes, errmsg);
  if (rc != 0) {
    if (out_bytes) *out_bytes = nullptr;
    if (out_size) *out_size = 0;
    return rc;
  }
  uint8_t* p = nullptr;
  if (!bytes.empty()) {
    p = (uint8_t*)std::malloc(bytes.size());
    if (!p) {
      setErr(errmsg, "out of memory");
      if (out_size) *out_size = 0;
      if (out_bytes) *out_bytes = nullptr;
      return 1;
    }
    std::memcpy(p, bytes.data(), bytes.size());
  }
  if (out_bytes) *out_bytes = p;
  if (out_size) *out_size = bytes.size();
  return 0;
}

int qld_check_script(const char* text, char** errmsg) {
  if (!text) {
    setErr(errmsg, "null script");
    return 1;
  }
  qld::script::ScriptFile sf;
  std::string error;
  if (!qld::script::parseText(text, sf, error)) {
    setErr(errmsg, error.empty() ? "script parse error" : error);
    return 1;
  }
  if (errmsg) *errmsg = nullptr;
  return 0;
}

// --- Incremental builder -----------------------------------------------

struct qld_linker {
  qld::detail::LinkOptions opts;
};

qld_linker_t* qld_create(void) { return new (std::nothrow) qld_linker(); }

void qld_destroy(qld_linker_t* self) { delete self; }

int qld_add_input(qld_linker_t* self, const char* path) {
  if (!self || !path) return 1;
  self->opts.inputs.emplace_back(path);
  return 0;
}

int qld_add_libpath(qld_linker_t* self, const char* path) {
  if (!self || !path) return 1;
  self->opts.libPaths.emplace_back(path);
  return 0;
}

int qld_add_library(qld_linker_t* self, const char* name) {
  if (!self || !name) return 1;
  self->opts.libs.emplace_back(name);
  return 0;
}

void qld_set_output(qld_linker_t* self, const char* path) {
  if (!self) return;
  self->opts.output = path ? path : "";
}

void qld_set_entry(qld_linker_t* self, const char* symbol) {
  if (!self) return;
  self->opts.entry = symbol ? symbol : "_start";
}

void qld_set_script_path(qld_linker_t* self, const char* path) {
  if (!self) return;
  self->opts.script = path ? path : "";
  self->opts.scriptIsText = false;
}

void qld_set_script_text(qld_linker_t* self, const char* text) {
  if (!self) return;
  self->opts.scriptText = text ? text : "";
  self->opts.scriptIsText = true;
}

void qld_set_base(qld_linker_t* self, uint64_t base) {
  if (!self) return;
  self->opts.base = base;
  self->opts.hasBase = true;
}

void qld_set_mode(qld_linker_t* self, qld_mode_t mode) {
  if (!self) return;
  switch (mode) {
    case QLD_MODE_SHARED:
      self->opts.mode = qld::detail::Mode::Shared;
      break;
    case QLD_MODE_RELOCATABLE:
      self->opts.mode = qld::detail::Mode::Relocatable;
      break;
    default: self->opts.mode = qld::detail::Mode::Exec; break;
  }
}

void qld_set_flag(qld_linker_t* self, const char* flag, int value) {
  if (!self || !flag) return;
  std::string f = flag;
  bool v = value != 0;
  if (f == "pie") {
    self->opts.pie = v;
    if (v) self->opts.mode = qld::detail::Mode::Shared;
  } else if (f == "gc-sections" || f == "gc_sections") {
    self->opts.gcSections = v;
  } else if (f == "strip-debug" || f == "strip_debug") {
    self->opts.stripDebug = v;
  } else if (f == "allow-undefined" || f == "allow_undefined") {
    self->opts.allowUndefined = v;
  } else if (f == "verbose") {
    self->opts.verbose = v;
  }
}

static qld_options_t toC(const qld_linker* self, std::vector<const char*>& a,
                         std::vector<const char*>& b,
                         std::vector<const char*>& c) {
  qld_options_t o{};
  for (const auto& s : self->opts.inputs) a.push_back(s.c_str());
  for (const auto& s : self->opts.libPaths) b.push_back(s.c_str());
  for (const auto& s : self->opts.libs) c.push_back(s.c_str());
  o.inputs = a.empty() ? nullptr : a.data();
  o.n_inputs = a.size();
  o.libpaths = b.empty() ? nullptr : b.data();
  o.n_libpaths = b.size();
  o.libs = c.empty() ? nullptr : c.data();
  o.n_libs = c.size();
  o.output = self->opts.output.empty() ? nullptr : self->opts.output.c_str();
  o.entry = self->opts.entry.c_str();
  o.script = self->opts.script.empty() ? nullptr : self->opts.script.c_str();
  o.script_text =
      self->opts.scriptIsText ? self->opts.scriptText.c_str() : nullptr;
  o.use_script_text = self->opts.scriptIsText ? 1 : 0;
  o.base = self->opts.base;
  o.has_base = self->opts.hasBase ? 1 : 0;
  o.mode = self->opts.mode == qld::detail::Mode::Shared
               ? QLD_MODE_SHARED
               : self->opts.mode == qld::detail::Mode::Relocatable
                     ? QLD_MODE_RELOCATABLE
                     : QLD_MODE_EXEC;
  o.pie = self->opts.pie ? 1 : 0;
  o.gc_sections = self->opts.gcSections ? 1 : 0;
  o.strip_debug = self->opts.stripDebug ? 1 : 0;
  o.allow_undefined = self->opts.allowUndefined ? 1 : 0;
  o.verbose = self->opts.verbose ? 1 : 0;
  o.map_file =
      self->opts.mapFile.empty() ? nullptr : self->opts.mapFile.c_str();
  return o;
}

int qld_run(qld_linker_t* self, char** errmsg) {
  if (!self) {
    setErr(errmsg, "null linker");
    return 1;
  }
  std::vector<const char*> a, b, c;
  qld_options_t o = toC(self, a, b, c);
  return qld_link(&o, errmsg);
}

int qld_run_bytes(qld_linker_t* self, uint8_t** out_bytes, size_t* out_size,
                  char** errmsg) {
  if (!self) {
    setErr(errmsg, "null linker");
    return 1;
  }
  std::vector<const char*> a, b, c;
  qld_options_t o = toC(self, a, b, c);
  return qld_link_bytes(&o, out_bytes, out_size, errmsg);
}
