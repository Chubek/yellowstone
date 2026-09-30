/* termscript.h -- standalone Termscript embedding + native extension API.
 *
 * Termscript is a small Turing-complete scripting language for terminal
 * automation.  This header is deliberately independent of DomTERM: it
 * includes only standard C headers, and programs need only link
 * libtermscript_standalone.  Including this header never pulls in
 * domterm.h, and no DomTERM library is required at link time.
 *
 * Two audiences:
 *
 * 1. Embedding: create a VM (ts_vm_create), optionally register native
 *    modules (ts_vm_register_module), run source (ts_vm_run_string /
 *    ts_vm_run_file), collect G:puts output, free the VM.  Use
 *    ts_check_syntax to validate without executing, and
 *    ts_vm_compile_to_c for the transpiled-C backend (generated C links
 *    against libtermscript_standalone).
 *
 * 2. Native extensions: implement TS_Module tables (static storage) of
 *    TS_Func callbacks and register them per VM.  Build return values
 *    with ts_value_make_*; wrap opaque native data with
 *    ts_value_make_handle (destructor runs when the value dies).
 *    TS_FuncDef.userdata carries author context (e.g. an instance
 *    pointer); it is borrowed and must outlive the registration.
 *
 * Ownership and lifetime:
 *
 * - TS_VM, and the AST it builds per run, are owned by the caller
 *   (ts_vm_create / ts_vm_free, NULL-safe free).
 * - TS_Value string payloads are heap copies owned by the value; free
 *   with ts_value_free (NULL-safe).  ts_value_copy deep-copies.
 * - TS_Module / TS_FuncDef tables and their names are borrowed; they
 *   must stay alive while any VM may call into them (static storage).
 * - Run output (*output) is malloc'd; the caller frees it with free().
 *   ts_vm_compile_to_c returns a borrowed string valid until the next
 *   compile call or VM free.
 * - A TS_Value passed to a TS_Func as argv is borrowed for the call;
 *   callbacks must copy what they keep.  The `ret` value is adopted.
 *
 * Thread-safety: a TS_VM is single-threaded (use one VM per thread, or
 * external locking).  Parsing is internally serialized, so concurrent
 * runs on distinct VMs are safe.  Registering a module may race scripts
 * running on the same VM.  Module tables must be thread-safe if shared
 * across threads (pure functions are ideal).
 *
 * Status codes intentionally mirror DomTERM's DT_Status numbering so
 * thin bindings can cast; see the _Static_assert guidance in DomTERM's
 * own adapter, not here.
 */

#ifndef TERMSCRIPT_H
#define TERMSCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define TS_VERSION_MAJOR 0
#define TS_VERSION_MINOR 1
#define TS_VERSION_PATCH 0

#define TS_ERROR_MSG_SIZE 256
#define TS_MAX_MODULES 64

/* Status codes.  Numeric values match DomTERM DT_Status on purpose. */
typedef enum
{
  TS_OK = 0,
  TS_ERR_INVAL = 1,     /* Bad argument, arity or type; const reassign. */
  TS_ERR_NOMEM = 2,     /* Allocation failure. */
  TS_ERR_SYSTEM = 7,    /* OS failure (e.g. unreadable script file). */
  TS_ERR_NOTFOUND = 4,  /* Unbound variable/module/function. */
  TS_ERR_PARSE = 5,     /* Syntax error or script-level arithmetic fault. */
  TS_ERR_CANCELLED = 12,/* G:die halt. */
  TS_ERR_LIMIT = 14     /* Step budget, size caps, registry full. */
} TS_Status;

typedef struct
{
  TS_Status code;
  int system_errno;             /* Preserved errno, 0 when not applicable. */
  size_t offset;                /* Byte offset for parse failures. */
  char message[TS_ERROR_MSG_SIZE]; /* Always NUL-terminated. */
} TS_Error;

const char *ts_status_string (TS_Status status);
void ts_error_set (TS_Error *error, TS_Status code, int system_errno,
                   size_t offset, const char *fmt, ...);
void ts_error_clear (TS_Error *error); /* NULL-safe. */

/* Values. */
typedef enum
{
  TS_NIL = 0,
  TS_BOOL,
  TS_INT,
  TS_STRING,
  TS_HANDLE /* Opaque native data, see ts_value_make_handle. */
} TS_ValueType;

typedef struct TS_Handle TS_Handle; /* Opaque; refcounted internally. */

typedef struct TS_Value TS_Value;
struct TS_Value
{
  TS_ValueType type;
  union
  {
    bool boolean;
    int64_t integer;
    char *string;       /* Owned when type == TS_STRING. */
    TS_Handle *handle;  /* Owned (refcounted) when TS_HANDLE. */
  } as;
};

/* Constructors (string payloads are heap copies).  The _make_handle
 * variant wraps native data: free_fn (may be NULL) runs with ptr when
 * the last value referencing it dies; desc is a borrowed short label
 * used by ts_value_to_string; tag is an opaque identity token used by
 * ts_value_handle to type-check (pass a pointer to a static const). */
void ts_value_make_nil (TS_Value *v);
void ts_value_make_bool (TS_Value *v, bool b);
void ts_value_make_int (TS_Value *v, int64_t i);
TS_Status ts_value_make_string (TS_Value *v, const char *s);
typedef void (*TS_HandleFree) (void *ptr);
TS_Status ts_value_make_handle (TS_Value *v, void *ptr,
                                TS_HandleFree free_fn, const char *desc,
                                const void *tag);
/* Returns the wrapped pointer when v is a handle created with a matching
 * tag, else NULL.  May be NULL even for handles (tag mismatch). */
void *ts_value_handle (const TS_Value *v, const void *tag);
const char *ts_handle_desc (const TS_Value *v); /* Never NULL. */

void ts_value_free (TS_Value *value); /* NULL-safe. */
TS_Status ts_value_copy (TS_Value *dst, const TS_Value *src);
bool ts_value_truthy (const TS_Value *v);
/* Heap-allocated spelling ("nil", "true", "42", strings verbatim,
 * "<handle desc>"); caller frees with free(). */
TS_Status ts_value_to_string (const TS_Value *value, char **out,
                              TS_Error *error);

/* Native extension ABI. */
typedef struct TS_VM TS_VM;

typedef TS_Status (*TS_Func) (TS_VM *vm, void *userdata,
                              const TS_Value *argv, size_t argc,
                              TS_Value *ret, TS_Error *error);

typedef struct
{
  const char *name;     /* Borrowed static string. */
  TS_Func func;         /* Must be non-NULL. */
  void *userdata;       /* Borrowed author context, may be NULL. */
} TS_FuncDef;

typedef struct
{
  const char *name;             /* Borrowed static string. */
  const TS_FuncDef *funcs;      /* Borrowed NULL-terminated table. */
} TS_Module;

/* Arity guard for TS_Func implementations. */
TS_Status ts_check_argc (TS_VM *vm, const TS_Value *argv, size_t argc,
                         size_t want, TS_Error *error);

/* Embedding API. */
TS_VM *ts_vm_create (TS_Error *error);
void ts_vm_free (TS_VM *vm); /* NULL-safe. */
/* Register a module on this VM (validates table, rejects duplicates).
 * May race scripts running on the same VM. */
TS_Status ts_vm_register_module (TS_VM *vm, const TS_Module *module,
                                 TS_Error *error);
/* Per-VM author pointer (e.g. back-pointer for trampolines). */
void ts_vm_set_userdata (TS_VM *vm, void *userdata);
void *ts_vm_get_userdata (const TS_VM *vm);
/* Run source text.  Captured G:puts output is appended to *output when
 * non-NULL (caller frees with free()). */
TS_Status ts_vm_run_string (TS_VM *vm, const char *source, char **output,
                            TS_Error *error);
TS_Status ts_vm_run_file (TS_VM *vm, const char *path, char **output,
                          TS_Error *error);
/* Validate without executing. */
TS_Status ts_check_syntax (const char *source, TS_Error *error);
/* Transpile to C (AOT backend for tooling).  Borrowed string, valid
 * until the next compile call or VM free.  Generated C is real,
 * Ahead-of-time compiled C: every statement and expression in `source`
 * is emitted as C control flow and calls into this API (no source text
 * is embedded and nothing is parsed at run time).  Termscript-side
 * libraries loaded with `G:import "name"` (the `.tsc` companions) are
 * resolved through the VM search paths plus TERMSCRIPT_PATH at compile
 * time and inlined as compiled C; only constant-string `G:import`
 * sites are inlined (dynamic names still import at run time).
 * Native modules (`G`, `std.*`, custom extensions) are NOT embedded:
 * link them (statically) and they are resolved through the VM registry
 * at run time.  The standalone backend links only
 * libtermscript_standalone (+ libtermscript_stdlib when `std.*`
 * natives are used); the DomTERM backend links the full closure. */
const char *ts_vm_compile_to_c (TS_VM *vm, const char *source,
                                const char *unit_name, TS_Error *error);

/* Compiled-code runtime: helpers emitted C calls into.  `line` is the
 * Termscript source line carried into error offsets (0 when unknown).
 * ts_rt_set adopts *val on success (resets it to nil); on failure the
 * caller still owns *val. */
TS_Status ts_rt_call (TS_VM *vm, const TS_Value *mod, const char *modname,
                      const char *func, const TS_Value *argv, size_t argc,
                      TS_Value *ret, unsigned line, TS_Error *error);
TS_Status ts_rt_get (TS_VM *vm, const char *name, TS_Value *out,
                     unsigned line, TS_Error *error);
TS_Status ts_rt_set (TS_VM *vm, const char *name, TS_Value *val,
                     bool is_const, unsigned line, TS_Error *error);
TS_Status ts_rt_step (TS_VM *vm, TS_Error *error);
/* Borrowed captured G:puts output ("" when empty, never NULL). */
const char *ts_rt_output (const TS_VM *vm);

/* DomTERM-flavored compile on a standalone VM (shared emitter backend;
 * used by dt_termscript_compile_to_c through the inner TS VM). */
const char *ts_vm_compile_to_c_dt (TS_VM *vm, const char *source,
                                   const char *unit_name,
                                   TS_Error *error);

/* Search paths and Termscript-side modules.
 *
 * A VM carries an ordered list of search directories.  ts_vm_import
 * resolves `name` to `<dir>/<name>.tsc` or `<dir>/<name>/<name>.tsc`
 * (first hit wins), reads the file, and executes it in the *shared*
 * scope of the calling VM: variables bound by the imported file persist
 * (that is the export mechanism — e.g. `const json = G:load "std.json";`),
 * G:puts output accumulates, and the step budget accumulates.  In
 * addition to the VM paths, the colon-separated TERMSCRIPT_PATH
 * environment variable is consulted on every import.
 *
 * Names are restricted to [A-Za-z0-9_.-]+ (no slashes, no "..").
 * Import cycles fail with TS_ERR_INVAL.  Files larger than 1 MiB fail
 * with TS_ERR_LIMIT.  The G module exposes this as `G:import "name"`
 * and `G:import_path "dir". */
TS_Status ts_vm_add_search_path (TS_VM *vm, const char *dir,
                                 TS_Error *error);
TS_Status ts_vm_import (TS_VM *vm, const char *name, TS_Error *error);

const char *ts_version_string (void);

#ifdef __cplusplus
}
#endif

#endif /* TERMSCRIPT_H */
