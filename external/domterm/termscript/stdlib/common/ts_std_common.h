/* ts_std_common.h -- shared internals for the Termscript stdlib.
 *
 * Included by every std.* module implementation (never by Termscript
 * scripts).  Installed for out-of-tree native modules under
 * <termscript/stdlib/common/ts_std_common.h>.
 *
 * Contents: a growable string buffer (ts_sbuf_t), type-checked
 * argument getters, return-value makers, a shared TS_Value list store
 * (used by std.list and by every module that returns sequences), and
 * a small DOM type shared by the std.json / std.yaml / std.toml
 * modules (one parser family, one stringify).
 *
 * Ownership follows termscript.h: getters borrow, makers adopt into
 * `ret`, wrap functions transfer a heap object into a handle value.
 */
#ifndef TS_STD_COMMON_H
#define TS_STD_COMMON_H

#include <termscript/termscript.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ---------------- growable string buffer ---------------- */

typedef struct
{
  char *data;
  size_t len;
  size_t cap;
} ts_sbuf_t;

void ts_sbuf_init (ts_sbuf_t *b);
void ts_sbuf_free (ts_sbuf_t *b);
void ts_sbuf_clear (ts_sbuf_t *b);
/* 0 on success, -1 on OOM. */
int ts_sbuf_put (ts_sbuf_t *b, const char *s, size_t n);
int ts_sbuf_str (ts_sbuf_t *b, const char *s);
int ts_sbuf_ch (ts_sbuf_t *b, char c);
int ts_sbuf_printf (ts_sbuf_t *b, const char *fmt, ...);
/* Release a NUL-terminated copy (caller frees); never NULL (OOM
 * yields an empty string).  Resets the buffer. */
char *ts_sbuf_take (ts_sbuf_t *b);

/* ---------------- argument getters (borrowed) ---------------- */

TS_Status ts_std_str (const TS_Value *v, const char **out, TS_Error *error,
                      const char *what);
TS_Status ts_std_int (const TS_Value *v, int64_t *out, TS_Error *error,
                      const char *what);
/* Type-checks a handle against `tag`; NULL with error set on mismatch.
 * Pass tag NULL to accept any handle. */
void *ts_std_handle (const TS_Value *v, const void *tag, TS_Error *error,
                     const char *what);

/* ---------------- return makers (adopted into ret) ---------------- */

TS_Status ts_std_ret_str (TS_Value *ret, const char *s, TS_Error *error);
TS_Status ts_std_ret_strn (TS_Value *ret, const char *s, size_t n,
                           TS_Error *error);
void ts_std_ret_int (TS_Value *ret, int64_t i);
void ts_std_ret_bool (TS_Value *ret, bool b);
void ts_std_ret_nil (TS_Value *ret);

/* Arity in [lo, hi]; TS_ERR_INVAL naming the function otherwise. */
TS_Status ts_std_argc (TS_VM *vm, size_t argc, size_t lo, size_t hi,
                       TS_Error *error, const char *fname);

/* Fresh stringification of any value (caller frees); NULL on OOM. */
char *ts_std_to_str (const TS_Value *v, TS_Error *error);

/* Element destructor for Q-generated ts_strvec (char * slots). */
void ts_std_free_cstr (char **p);

/* ---------------- shared list store ---------------- */

typedef struct ts_std_list ts_std_list_t;

ts_std_list_t *ts_std_list_new (void);
void ts_std_list_free (void *p); /* TS_HandleFree-compatible. */
TS_Status ts_std_list_push (ts_std_list_t *l, const TS_Value *v);
TS_Status ts_std_list_push_str (ts_std_list_t *l, const char *s);
TS_Status ts_std_list_push_int (ts_std_list_t *l, int64_t i);
size_t ts_std_list_len (const ts_std_list_t *l);
const TS_Value *ts_std_list_get (const ts_std_list_t *l, size_t i);
TS_Status ts_std_list_set (ts_std_list_t *l, size_t i, const TS_Value *v);
void ts_std_list_pop (ts_std_list_t *l);
void ts_std_list_clear (ts_std_list_t *l);

/* Identity token shared by every module that produces/consumes list
 * handles (std.list, std.csv, std.regex, std.glob, ...). */
const void *ts_std_list_tag (void);
/* Wrap (transferring ownership) / unwrap (borrowed, type-checked). */
TS_Status ts_std_list_wrap (TS_Value *ret, ts_std_list_t *l,
                            TS_Error *error);
ts_std_list_t *ts_std_list_unwrap (const TS_Value *v, TS_Error *error,
                                   const char *what);

/* ---------------- shared DOM (json / yaml / toml) ---------------- */

typedef enum
{
  TS_DOM_NULL = 0,
  TS_DOM_BOOL,
  TS_DOM_INT,
  TS_DOM_STR,
  TS_DOM_ARR,
  TS_DOM_OBJ
} ts_dom_kind_t;

typedef struct ts_dom ts_dom_t;

ts_dom_t *ts_dom_new_null (void);
ts_dom_t *ts_dom_new_bool (bool b);
ts_dom_t *ts_dom_new_int (int64_t i);
ts_dom_t *ts_dom_new_str (const char *s);
ts_dom_t *ts_dom_new_arr (void);
ts_dom_t *ts_dom_new_obj (void);
void ts_dom_free (void *p); /* TS_HandleFree-compatible. */
/* Adopt (arrays/objects) / copy (scalars) `v` into the container. */
TS_Status ts_dom_arr_push (ts_dom_t *arr, ts_dom_t *v);
TS_Status ts_dom_obj_set (ts_dom_t *obj, const char *key, ts_dom_t *v);

/* Strict parsers (trailing garbage is an error).  YAML covers the
 * documented subset: block maps/lists by indentation, flow-free
 * scalars, `#` comments.  TOML is delegated to vendored tomlc99. */
TS_Status ts_dom_parse_json (const char *s, ts_dom_t **out, TS_Error *error);
TS_Status ts_dom_parse_yaml (const char *s, ts_dom_t **out, TS_Error *error);
TS_Status ts_dom_parse_toml (const char *s, ts_dom_t **out, TS_Error *error);

/* Navigation: dotted "a.b.0" paths (objects by key, arrays by index).
 * Borrowed; NULL when any step misses. */
const ts_dom_t *ts_dom_path (const ts_dom_t *d, const char *path);
const ts_dom_t *ts_dom_at (const ts_dom_t *d, size_t i);
const ts_dom_t *ts_dom_key (const ts_dom_t *d, const char *key);
size_t ts_dom_len (const ts_dom_t *d);
const char *ts_dom_kind_str (const ts_dom_t *d);

/* JSON spelling (caller frees). */
TS_Status ts_dom_stringify (const ts_dom_t *d, char **out);
/* Encode a scalar / container into a TS value: scalars become native
 * values, arrays/objects become sub-handles (adopted into ret). */
TS_Status ts_dom_to_value (const ts_dom_t *d, TS_Value *ret, TS_Error *error);

const void *ts_dom_tag (void);
TS_Status ts_dom_wrap (TS_Value *ret, ts_dom_t *d, TS_Error *error);
ts_dom_t *ts_dom_unwrap (const TS_Value *v, TS_Error *error,
                         const char *what);

#ifdef __cplusplus
}
#endif

#endif /* TS_STD_COMMON_H */
