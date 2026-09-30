// ld.h - C API for the qobjld linker.
//
// This header is the C-accessible entry point to the linker facilities.
// The implementation lives in ld.c (compiled as C++ for access to the
// header-only readers in ../fd/). The C++ wrapper in ld.hpp builds on
// these functions.
#ifndef QOBJLD_LD_H
#define QOBJLD_LD_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(QLD_BUILD_SHARED)
#  ifdef qld_EXPORTS
#    define QLD_API __declspec(dllexport)
#  else
#    define QLD_API __declspec(dllimport)
#  endif
#else
#  define QLD_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Link output mode. */
typedef enum qld_mode {
  QLD_MODE_EXEC = 0,      /* ET_EXEC static executable (default) */
  QLD_MODE_SHARED = 1,    /* ET_DYN shared object (-shared) */
  QLD_MODE_RELOCATABLE = 2 /* ET_REL partial link (-r) */
} qld_mode_t;

/* Stable result codes returned by every operation. */
typedef enum qld_status {
  QLD_OK = 0,
  QLD_ERROR = 1,
  QLD_INVALID_ARGUMENT = 2,
  QLD_OUT_OF_MEMORY = 3,
  QLD_IO_ERROR = 4,
  QLD_PARSE_ERROR = 5,
  QLD_UNDEFINED_SYMBOL = 6,
  QLD_UNSUPPORTED = 7
} qld_status_t;

/* One-shot link options. All pointers are borrowed for the call duration;
 * none are retained. Strings use NUL-terminated UTF-8 path/byte semantics
 * of the host filesystem. */
typedef struct qld_options {
  const char* const* inputs;     /* object / archive / script paths */
  size_t n_inputs;
  const char* const* libpaths;   /* -L directories */
  size_t n_libpaths;
  const char* const* libs;       /* -l names (without lib prefix/suffix) */
  size_t n_libs;
  const char* output;            /* -o path, or NULL for in-memory only */
  const char* entry;             /* -e symbol, or NULL for default */
  const char* script;            /* -T path, or NULL */
  const char* script_text;       /* inline script, or NULL */
  int use_script_text;           /* nonzero when script_text is valid */
  uint64_t base;                 /* --image-base value */
  int has_base;                  /* nonzero when base is valid */
  qld_mode_t mode;               /* output mode */
  int pie;                       /* -pie: ET_DYN position-independent exec */
  int gc_sections;               /* --gc-sections */
  int strip_debug;               /* -s: drop debug sections/symbols */
  int allow_undefined;           /* --allow-undefined */
  int verbose;                   /* --verbose */
  const char* map_file;          /* -Map path, or NULL */
  const char* emulator;          /* -m name, informational only */
} qld_options_t;

/* Initialize an options record to documented defaults. This is ABI-safe for
 * callers that do not use C designated initializers. */
QLD_API void qld_options_init(qld_options_t* opts);

/* Link once. Returns 0 on success, nonzero on failure.
 * On failure and when errmsg is non-NULL, *errmsg receives a malloc'd
 * NUL-terminated message the caller must release with qld_free_string(). */
QLD_API int qld_link(const qld_options_t* opts, char** errmsg);

/* Link to memory. Like qld_link but always captures the output image into
 * malloc'd bytes (*out_bytes, *out_size) regardless of opts->output.
 * The caller releases the buffer with qld_free_bytes(). When opts->output
 * is set the file is also written. */
QLD_API int qld_link_bytes(const qld_options_t* opts, uint8_t** out_bytes,
                   size_t* out_size, char** errmsg);

/* Validate a linker script without linking. Returns 0 when the script
 * parses, nonzero otherwise (with *errmsg set as in qld_link). */
QLD_API int qld_check_script(const char* text, char** errmsg);

/* Library version string, e.g. "1.0". Never NULL. */
QLD_API const char* qld_version(void);
QLD_API const char* qld_status_string(int status);

/* Release helpers for qld_link / qld_link_bytes diagnostics. */
QLD_API void qld_free_string(char* s);
QLD_API void qld_free_bytes(uint8_t* p, size_t n);

/* Incremental builder API for bindings that prefer many small calls. */
typedef struct qld_linker qld_linker_t;

qld_linker_t* qld_create(void);
void qld_destroy(qld_linker_t* self);
int qld_add_input(qld_linker_t* self, const char* path);
int qld_add_libpath(qld_linker_t* self, const char* path);
int qld_add_library(qld_linker_t* self, const char* name);
void qld_set_output(qld_linker_t* self, const char* path);
void qld_set_entry(qld_linker_t* self, const char* symbol);
void qld_set_script_path(qld_linker_t* self, const char* path);
void qld_set_script_text(qld_linker_t* self, const char* text);
void qld_set_base(qld_linker_t* self, uint64_t base);
void qld_set_mode(qld_linker_t* self, qld_mode_t mode);
void qld_set_flag(qld_linker_t* self, const char* flag, int value);
int qld_run(qld_linker_t* self, char** errmsg);
int qld_run_bytes(qld_linker_t* self, uint8_t** out_bytes, size_t* out_size,
                  char** errmsg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* QOBJLD_LD_H */
