/* ts_runtime.c -- standalone Termscript runtime (no DomTERM dependency).
 *
 * Implements termscript/termscript.h: the embedding API (TS_VM lifecycle,
 * run/compile) and the native extension ABI (TS_Module tables).  Only
 * standard C headers plus termscript.h are included; linking requires
 * libc and pthreads only.
 *
 * Formal grammar: termscript/Termscript.g, compiled to a parser with
 * scripts/aurocks.pl and built alongside this file.  The generated
 * parser builds the ts_node_t AST below through the ts_* constructors;
 * the tree-walking evaluator and the C transpiler then operate on that
 * tree.
 *
 * Core language subset:
 *   program  := stmt*
 *   stmt     := "const" IDENT "=" expr ";"
 *             | IDENT "=" expr ";"                (assign/declare)
 *             | "while" expr "do" stmt* "end"
 *             | "if" expr "do" stmt* ["else" stmt*] "end"
 *             | expr ";"
 *   expr     := unary ("or" unary)*
 *   unary    := "&" IDENT | primary
 *   primary  := IDENT ":" IDENT args | IDENT | STRING | NUMBER
 *             | "true" | "false" | "nil"
 *   args     := arg*   (each STRING | NUMBER | IDENT | "&" IDENT)
 *
 * Values: nil, bool, int, string, handle (native module or extension
 * data).  Falsiness: nil, false, 0 and "" are false; all else true.
 * `or` short-circuits.  G:die halts with TS_ERR_CANCELLED.
 * Execution is bounded by TS_MAX_STEPS (infinite loops fail). */
#define _POSIX_C_SOURCE 200809L

#include "termscript.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_MAX_STEPS (1u << 20)
#define TS_MAX_FILE (1u << 20)

/* ---------------- status and errors ---------------- */

const char *
ts_status_string (TS_Status status)
{
  switch (status)
    {
    case TS_OK: return "OK";
    case TS_ERR_INVAL: return "INVALID_ARGUMENT";
    case TS_ERR_NOMEM: return "NO_MEMORY";
    case TS_ERR_SYSTEM: return "SYSTEM";
    case TS_ERR_NOTFOUND: return "NOT_FOUND";
    case TS_ERR_PARSE: return "PARSE";
    case TS_ERR_CANCELLED: return "CANCELLED";
    case TS_ERR_LIMIT: return "LIMIT";
    default: return "UNKNOWN";
    }
}

void
ts_error_set (TS_Error *error, TS_Status code, int system_errno,
              size_t offset, const char *fmt, ...)
{
  va_list ap;
  if (!error)
    return;
  error->code = code;
  error->system_errno = system_errno;
  error->offset = offset;
  if (!fmt)
    {
      error->message[0] = '\0';
      return;
    }
  va_start (ap, fmt);
  vsnprintf (error->message, sizeof error->message, fmt, ap);
  va_end (ap);
}

void
ts_error_clear (TS_Error *error)
{
  if (!error)
    return;
  error->code = TS_OK;
  error->system_errno = 0;
  error->offset = 0;
  error->message[0] = '\0';
}

/* ---------------- value handles ---------------- */

/* Identity token for module handles (address used for tag checks). */
static const char ts_module_tag_id = 0;

struct TS_Handle
{
  int refcount;
  void *ptr;
  void (*free_fn) (void *ptr);
  const void *tag;
  char desc[96];
};

static TS_Handle *
ts_handle_new (void *ptr, void (*free_fn) (void *), const void *tag,
               const char *desc)
{
  TS_Handle *h = calloc (1, sizeof *h);
  if (!h)
    return NULL;
  h->refcount = 1;
  h->ptr = ptr;
  h->free_fn = free_fn;
  h->tag = tag;
  snprintf (h->desc, sizeof h->desc, "%s", desc ? desc : "<handle>");
  return h;
}

static TS_Handle *
ts_handle_new_module (const TS_Module *mod)
{
  char desc[96];
  snprintf (desc, sizeof desc, "<module %s>",
            mod && mod->name ? mod->name : "?");
  return ts_handle_new ((void *) mod, NULL, &ts_module_tag_id, desc);
}

static const TS_Module *
ts_handle_module (const TS_Handle *h)
{
  if (!h || h->tag != &ts_module_tag_id)
    return NULL;
  return (const TS_Module *) h->ptr;
}

static void
ts_handle_retain (TS_Handle *h)
{
  if (h)
    h->refcount++;
}

static void
ts_handle_release (TS_Handle *h)
{
  if (!h)
    return;
  if (--h->refcount > 0)
    return;
  if (h->free_fn)
    h->free_fn (h->ptr);
  free (h);
}

/* ---------------- VM ---------------- */

typedef struct ts_var
{
  char *name;
  TS_Value value;
  bool is_const;
  struct ts_var *next;
} ts_var_t;

struct TS_VM
{
  ts_var_t *vars;
  const TS_Module *modules[TS_MAX_MODULES];
  size_t nmodules;
  /* Guards modules/nmodules so registration may race running scripts
   * (mirrors the old global-registry lock); never held across callbacks. */
  pthread_mutex_t mod_mutex;
  void *userdata;
  char *output;
  size_t olen, ocap;
  unsigned long steps;
  char *compiled;
  /* Import machinery (see ts_vm_import): ordered search directories plus
   * a stack of in-progress import paths for cycle detection. */
  char **spaths;
  size_t nspaths, spaths_cap;
  char **istack;
  size_t nistack, istack_cap;
};

static const TS_Module *
ts_vm_lookup (TS_VM *vm, const char *name)
{
  size_t i;
  const TS_Module *found = NULL;
  if (!vm || !name)
    return NULL;
  pthread_mutex_lock (&vm->mod_mutex);
  for (i = 0; i < vm->nmodules; i++)
    if (vm->modules[i]->name && strcmp (vm->modules[i]->name, name) == 0)
      {
        found = vm->modules[i];
        break;
      }
  pthread_mutex_unlock (&vm->mod_mutex);
  return found;
}

TS_Status
ts_vm_register_module (TS_VM *vm, const TS_Module *module, TS_Error *error)
{
  size_t i;
  if (!vm || !module || !module->name || !module->funcs)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad module");
      return TS_ERR_INVAL;
    }
  for (i = 0; module->funcs[i].name; i++)
    if (!module->funcs[i].func)
      {
        ts_error_set (error, TS_ERR_INVAL, 0, 0,
                      "module func '%s' is NULL", module->funcs[i].name);
        return TS_ERR_INVAL;
      }
  pthread_mutex_lock (&vm->mod_mutex);
  for (i = 0; i < vm->nmodules; i++)
    if (strcmp (vm->modules[i]->name, module->name) == 0)
      {
        pthread_mutex_unlock (&vm->mod_mutex);
        ts_error_set (error, TS_ERR_INVAL, 0, 0,
                      "module '%s' already registered", module->name);
        return TS_ERR_INVAL;
      }
  if (vm->nmodules >= TS_MAX_MODULES)
    {
      pthread_mutex_unlock (&vm->mod_mutex);
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "module registry full");
      return TS_ERR_LIMIT;
    }
  vm->modules[vm->nmodules++] = module;
  pthread_mutex_unlock (&vm->mod_mutex);
  return TS_OK;
}

void
ts_vm_set_userdata (TS_VM *vm, void *userdata)
{
  if (vm)
    vm->userdata = userdata;
}

void *
ts_vm_get_userdata (const TS_VM *vm)
{
  return vm ? vm->userdata : NULL;
}

static void
ts_value_release (TS_Value *v)
{
  if (!v)
    return;
  if (v->type == TS_STRING)
    free (v->as.string);
  else if (v->type == TS_HANDLE)
    ts_handle_release ((TS_Handle *) v->as.handle);
  v->type = TS_NIL;
  v->as.handle = NULL;
}

TS_Status
ts_value_copy (TS_Value *dst, const TS_Value *src)
{
  dst->type = src->type;
  if (src->type == TS_STRING)
    {
      dst->as.string = src->as.string ? strdup (src->as.string) : NULL;
      if (src->as.string && !dst->as.string)
        return TS_ERR_NOMEM;
    }
  else if (src->type == TS_HANDLE)
    {
      dst->as.handle = src->as.handle;
      ts_handle_retain ((TS_Handle *) dst->as.handle);
    }
  else
    dst->as = src->as;
  return TS_OK;
}

void
ts_value_free (TS_Value *value)
{
  ts_value_release (value);
}

void
ts_value_make_nil (TS_Value *v)
{
  if (!v)
    return;
  ts_value_release (v);
  v->type = TS_NIL;
}

void
ts_value_make_bool (TS_Value *v, bool b)
{
  if (!v)
    return;
  ts_value_release (v);
  v->type = TS_BOOL;
  v->as.boolean = b;
}

void
ts_value_make_int (TS_Value *v, int64_t i)
{
  if (!v)
    return;
  ts_value_release (v);
  v->type = TS_INT;
  v->as.integer = i;
}

TS_Status
ts_value_make_string (TS_Value *v, const char *s)
{
  char *copy;
  if (!v)
    return TS_ERR_INVAL;
  copy = strdup (s ? s : "");
  if (!copy)
    return TS_ERR_NOMEM;
  ts_value_release (v);
  v->type = TS_STRING;
  v->as.string = copy;
  return TS_OK;
}

TS_Status
ts_value_make_handle (TS_Value *v, void *ptr,
                      TS_HandleFree free_fn, const char *desc,
                      const void *tag)
{
  TS_Handle *h;
  if (!v)
    return TS_ERR_INVAL;
  h = ts_handle_new (ptr, free_fn, tag, desc);
  if (!h)
    return TS_ERR_NOMEM;
  ts_value_release (v);
  v->type = TS_HANDLE;
  v->as.handle = h;
  return TS_OK;
}

void *
ts_value_handle (const TS_Value *v, const void *tag)
{
  TS_Handle *h;
  if (!v || v->type != TS_HANDLE || !(h = v->as.handle))
    return NULL;
  if (tag && h->tag != tag)
    return NULL;
  return h->ptr;
}

const char *
ts_handle_desc (const TS_Value *v)
{
  TS_Handle *h;
  if (!v || v->type != TS_HANDLE || !(h = v->as.handle))
    return "<handle>";
  return h->desc;
}

bool
ts_value_truthy (const TS_Value *v)
{
  switch (v->type)
    {
    case TS_NIL: return false;
    case TS_BOOL: return v->as.boolean;
    case TS_INT: return v->as.integer != 0;
    case TS_STRING:
      return v->as.string && v->as.string[0] != '\0';
    case TS_HANDLE: return true;
    }
  return false;
}

TS_Status
ts_value_to_string (const TS_Value *value, char **out,
                        TS_Error *error)
{
  char tmp[64];
  const char *s = NULL;
  if (!value || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  switch (value->type)
    {
    case TS_NIL: s = "nil"; break;
    case TS_BOOL: s = value->as.boolean ? "true" : "false"; break;
    case TS_INT:
      snprintf (tmp, sizeof tmp, "%lld", (long long) value->as.integer);
      s = tmp;
      break;
    case TS_STRING: s = value->as.string ? value->as.string : ""; break;
    case TS_HANDLE:
      s = value->as.handle ? ((TS_Handle *) value->as.handle)->desc :
                             "<handle>";
      break;
    }
  *out = strdup (s);
  if (!*out)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

/* ---------------- builtin modules ---------------- */

TS_Status
ts_check_argc (TS_VM *vm, const TS_Value *argv, size_t argc, size_t want,
           TS_Error *error)
{
  (void) vm;
  (void) argv;
  if (argc != want)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                  "want %zu args, got %zu", want, argc);
      return TS_ERR_INVAL;
    }
  return TS_OK;
}

static bool
term_is_int (const TS_Value *v)
{
  return v->type == TS_INT;
}

static TS_Status
term_arith (TS_VM *vm, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error, char op)
{
  if (ts_check_argc (vm, argv, argc, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (!term_is_int (&argv[0]) || !term_is_int (&argv[1]))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "ints required");
      return TS_ERR_INVAL;
    }
  ret->type = TS_INT;
  switch (op)
    {
    case '+': ret->as.integer = argv[0].as.integer + argv[1].as.integer; break;
    case '-': ret->as.integer = argv[0].as.integer - argv[1].as.integer; break;
    case '*': ret->as.integer = argv[0].as.integer * argv[1].as.integer; break;
    case '/':
      if (argv[1].as.integer == 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, 0, "division by zero");
          return TS_ERR_PARSE;
        }
      ret->as.integer = argv[0].as.integer / argv[1].as.integer;
      break;
    case 'm':
      if (argv[1].as.integer == 0)
        {
          ts_error_set (error, TS_ERR_PARSE, 0, 0, "modulo by zero");
          return TS_ERR_PARSE;
        }
      ret->as.integer = argv[0].as.integer % argv[1].as.integer;
      break;
    default:
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad op");
      return TS_ERR_INVAL;
    }
  return TS_OK;
}

static TS_Status g_add (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                        TS_Value *r, TS_Error *e)
{ (void) ud; return term_arith (v, a, n, r, e, '+'); }
static TS_Status g_sub (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                        TS_Value *r, TS_Error *e)
{ (void) ud; return term_arith (v, a, n, r, e, '-'); }
static TS_Status g_mul (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                        TS_Value *r, TS_Error *e)
{ (void) ud; return term_arith (v, a, n, r, e, '*'); }
static TS_Status g_div (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                        TS_Value *r, TS_Error *e)
{ (void) ud; return term_arith (v, a, n, r, e, '/'); }
static TS_Status g_mod (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                        TS_Value *r, TS_Error *e)
{ (void) ud; return term_arith (v, a, n, r, e, 'm'); }

static TS_Status
term_cmp (TS_VM *vm, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error, char op)
{
  bool b = false;
  if (ts_check_argc (vm, argv, argc, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (op == 'e')
    {
      if (argv[0].type != argv[1].type)
        b = false;
      else if (argv[0].type == TS_INT)
        b = argv[0].as.integer == argv[1].as.integer;
      else if (argv[0].type == TS_BOOL)
        b = argv[0].as.boolean == argv[1].as.boolean;
      else if (argv[0].type == TS_STRING)
        b = strcmp (argv[0].as.string ? argv[0].as.string : "",
                    argv[1].as.string ? argv[1].as.string : "") == 0;
      else if (argv[0].type == TS_NIL)
        b = true;
      else
        b = argv[0].as.handle == argv[1].as.handle;
    }
  else
    {
      if (!term_is_int (&argv[0]) || !term_is_int (&argv[1]))
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0,
                      "ints required");
          return TS_ERR_INVAL;
        }
      if (op == 'l')
        b = argv[0].as.integer < argv[1].as.integer;
      else if (op == 'g')
        b = argv[0].as.integer > argv[1].as.integer;
      else if (op == 'L')
        b = argv[0].as.integer <= argv[1].as.integer;
      else
        b = argv[0].as.integer >= argv[1].as.integer;
    }
  ret->type = TS_BOOL;
  ret->as.boolean = b;
  return TS_OK;
}

static TS_Status g_eq (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e)
{ (void) ud; return term_cmp (v, a, n, r, e, 'e'); }
static TS_Status g_lt (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e)
{ (void) ud; return term_cmp (v, a, n, r, e, 'l'); }
static TS_Status g_gt (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e)
{ (void) ud; return term_cmp (v, a, n, r, e, 'g'); }
static TS_Status g_le (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e)
{ (void) ud; return term_cmp (v, a, n, r, e, 'L'); }
static TS_Status g_ge (TS_VM *v, void *ud, const TS_Value *a, size_t n,
                       TS_Value *r, TS_Error *e)
{ (void) ud; return term_cmp (v, a, n, r, e, 'G'); }

static TS_Status
g_not (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  (void) ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  ret->type = TS_BOOL;
  ret->as.boolean = !ts_value_truthy (&argv[0]);
  return TS_OK;
}

static TS_Status
g_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  (void) ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "string required");
      return TS_ERR_INVAL;
    }
  ret->type = TS_INT;
  ret->as.integer =
    (int64_t) strlen (argv[0].as.string ? argv[0].as.string : "");
  return TS_OK;
}

static TS_Status
g_concat (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void) ud;
  char *a = NULL, *b = NULL;
  if (ts_check_argc (vm, argv, argc, 2, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_value_to_string (&argv[0], &a, error) != TS_OK)
    return TS_ERR_NOMEM;
  if (ts_value_to_string (&argv[1], &b, error) != TS_OK)
    {
      free (a);
      return TS_ERR_NOMEM;
    }
  ret->type = TS_STRING;
  ret->as.string = malloc (strlen (a) + strlen (b) + 1);
  if (!ret->as.string)
    {
      free (a);
      free (b);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  strcpy (ret->as.string, a);
  strcat (ret->as.string, b);
  free (a);
  free (b);
  return TS_OK;
}

static TS_Status
g_type (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  (void) ud;
  const char *t = "nil";
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  switch (argv[0].type)
    {
    case TS_NIL: t = "nil"; break;
    case TS_BOOL: t = "bool"; break;
    case TS_INT: t = "int"; break;
    case TS_STRING: t = "string"; break;
    case TS_HANDLE: t = "handle"; break;
    }
  ret->type = TS_STRING;
  ret->as.string = strdup (t);
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
g_load (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  (void) ud;
  const TS_Module *mod;
  TS_Handle *h;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                  "module name must be a string");
      return TS_ERR_INVAL;
    }
  mod = ts_vm_lookup (vm, argv[0].as.string);
  if (!mod)
    {
      ret->type = TS_NIL; /* Load failure is nil (see `or`). */
      return TS_OK;
    }
  h = ts_handle_new_module (mod);
  if (!h)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ret->type = TS_HANDLE;
  ret->as.handle = h;
  return TS_OK;
}

static TS_Status
g_puts (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  (void) ud;
  char *s = NULL;
  size_t n;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_value_to_string (&argv[0], &s, error) != TS_OK)
    return TS_ERR_NOMEM;
  n = strlen (s);
  if (vm->olen + n + 2 > vm->ocap)
    {
      size_t nc = vm->ocap ? vm->ocap * 2 : 256;
      char *nb;
      while (nc < vm->olen + n + 2)
        nc *= 2;
      nb = realloc (vm->output, nc);
      if (!nb)
        {
          free (s);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      vm->output = nb;
      vm->ocap = nc;
    }
  memcpy (vm->output + vm->olen, s, n);
  vm->olen += n;
  vm->output[vm->olen++] = '\n';
  vm->output[vm->olen] = '\0';
  free (s);
  ret->type = TS_NIL;
  return TS_OK;
}

static TS_Status
g_die (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  (void) ud;
  char *s = NULL;
  (void) vm;
  (void) ret;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_value_to_string (&argv[0], &s, error) != TS_OK)
    s = NULL;
  ts_error_set (error, TS_ERR_CANCELLED, 0, 0, "%s", s ? s : "die");
  free (s);
  return TS_ERR_CANCELLED;
}

static TS_Status
g_import (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void) ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "import name must be a string");
      return TS_ERR_INVAL;
    }
  if (ts_vm_import (vm, argv[0].as.string, error) != TS_OK)
    return error ? error->code : TS_ERR_NOTFOUND;
  ret->type = TS_NIL;
  return TS_OK;
}

static TS_Status
g_import_path (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
               TS_Value *ret, TS_Error *error)
{
  (void) ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "path must be a string");
      return TS_ERR_INVAL;
    }
  if (ts_vm_add_search_path (vm, argv[0].as.string, error) != TS_OK)
    return error ? error->code : TS_ERR_INVAL;
  ret->type = TS_NIL;
  return TS_OK;
}

static const TS_FuncDef g_funcs[] = {
  { "load", g_load, NULL }, { "puts", g_puts, NULL }, { "die", g_die, NULL },
  { "import", g_import, NULL }, { "import_path", g_import_path, NULL },
  { "add", g_add, NULL }, { "sub", g_sub, NULL }, { "mul", g_mul, NULL },
  { "div", g_div, NULL }, { "mod", g_mod, NULL }, { "eq", g_eq, NULL },
  { "lt", g_lt, NULL }, { "gt", g_gt, NULL }, { "le", g_le, NULL }, { "ge", g_ge, NULL },
  { "not", g_not, NULL }, { "len", g_len, NULL }, { "concat", g_concat, NULL },
  { "type", g_type, NULL }, { NULL, NULL, NULL }
};

static const TS_Module g_module = { "G", g_funcs };

/* ---------------- VM construction helpers ---------------- */

/* ---------------- AST ---------------- */

typedef enum
{
  N_SEQ, N_CONST, N_ASSIGN, N_EXPR_STMT, N_OR, N_VAR, N_CALL,
  N_LIT_INT, N_LIT_STR, N_LIT_BOOL, N_LIT_NIL, N_WHILE, N_IF
} node_kind_t;

typedef struct dt_arg
{
  bool is_ref;
  char *name; /* For IDENT / &IDENT. */
  int64_t number;
  char *string;
  int tag; /* 0=name, 1=number, 2=string, 3=bool, 4=nil. */
  bool boolean;
} ts_arg_t;

typedef struct dt_node
{
  node_kind_t kind;
  unsigned line;
  struct dt_node *next; /* For sequences. */
  struct dt_node *first; /* Body / sequence head. */
  struct dt_node *cond; /* while/if condition. */
  struct dt_node *otherwise; /* if-else branch head. */
  struct dt_node *lhs; /* or-branches. */
  struct dt_node *rhs;
  char *name; /* const/assign/var. */
  char *module; /* call. */
  char *func; /* call. */
  ts_arg_t *args;
  size_t nargs;
  int64_t number;
  char *string;
  bool boolean;
} ts_node_t;

static void
ts_node_free (ts_node_t *n)
{
  size_t i;
  while (n)
    {
      ts_node_t *nx = n->next;
      if (n->kind == N_SEQ)
        {
          /* Sequence wrapper from ts_mk_seq/ts_seq_append: first owns the
           * statement chain, rhs is a non-owning tail link into it. */
          ts_node_t *ch = n->first;
          free (n);
          n = ch;
          continue;
        }
      ts_node_free (n->first);
      ts_node_free (n->cond);
      ts_node_free (n->otherwise);
      ts_node_free (n->lhs);
      ts_node_free (n->rhs);
      free (n->name);
      free (n->module);
      free (n->func);
      for (i = 0; i < n->nargs; i++)
        {
          free (n->args[i].name);
          free (n->args[i].string);
        }
      free (n->args);
      free (n->string);
      free (n);
      n = nx;
    }
}

static ts_node_t *
ts_node_new (node_kind_t kind, unsigned line)
{
  ts_node_t *n = calloc (1, sizeof *n);
  if (n)
    {
      n->kind = kind;
      n->line = line;
    }
  return n;
}

/* ---------------- aurocks front end ---------------- */
/*
 * Termscript is parsed by the aurocks-generated parser compiled from
 * termscript/Termscript.g (see "perl scripts/aurocks.pl Termscript.g").
 * The actions in the grammar call the ts_* constructors below, which
 * build the same ts_node_t AST the evaluator walks.  Ownership rules:
 *
 * - Every ts_* helper either adopts each `char *` it is given or frees
 *   it before returning; callers never touch adopted/freed buffers.
 * - Helpers return NULL only on allocation failure (recorded with
 *   ts_oom()) or deliberate rule rejection (ts_check_or on a non-`or`
 *   word, ts_mk_argword/ts_mk_unary on the reserved word `or`, and the
 *   %empty untail/if_tail markers).  Grammar actions turn helper NULLs
 *   into `return 0` (backtrack), except the two intentional-NULL slots
 *   consumed by ts_mk_unary (plain variable) and ts_mk_if (missing
 *   else), which succeed.
 * - A top-level NULL result with ts_oom() set means TS_ERR_NOMEM;
 *   without it, a syntax error (with a best-effort offset).
 * - Line numbers come from ts_curline(attempt.s): the byte offset is
 *   rewound over trailing layout before counting newlines, so lines are
 *   exact unless trailing `#` comment lines intervene (documented
 *   approximation, off by the comment count).
 * - Parse calls are serialized by ts_parse_mutex because the generated
 *   entry takes no user data; the per-parse input window lives in
 *   ts_pctx.  Evaluation itself stays per-VM and lock-free.
 */

void *ts_gram_parse (const char *input); /* generated parser */

/* ts_mk_string/ts_mk_number are defined below. */
void *ts_mk_string (char *tok, unsigned line);
void *ts_mk_number (char *tok, unsigned line);

static pthread_mutex_t ts_parse_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct
{
  const char *start;
  const char *end;
  bool oom;
} ts_pctx = { NULL, NULL, false };

void
ts_oom (void)
{
  ts_pctx.oom = true;
}

unsigned
ts_curline (const char *pos)
{
  const char *start = ts_pctx.start;
  const char *end = ts_pctx.end;
  const char *p = pos;
  const char *q;
  unsigned line = 1;
  if (!start || !end)
    return 1;
  if (p < start)
    p = start;
  if (p > end)
    p = end;
  while (p > start && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' ||
                       p[-1] == '\n'))
    p--;
  for (q = start; q < p; q++)
    if (*q == '\n')
      line++;
  return line;
}

void *
ts_kw (char *tok, const char **pos, unsigned kwlen)
{
  /* Keyword-with-boundary match (e.g. "const " or "end" at EOF): return
   * the single boundary character to the input so the rest of the
   * grammar sees it.  Losing it would corrupt parsing (`const=x` would
   * misparse with the `=` silently eaten).  The boundary is exactly one
   * character, or empty at end of input (kwlen match, `$` branch). */
  size_t len;
  if (!tok || !pos || !*pos)
    {
      free (tok);
      ts_oom ();
      return NULL;
    }
  len = strlen (tok);
  if (len < kwlen)
    {
      free (tok);
      ts_oom ();
      return NULL;
    }
  *pos -= len - kwlen;
  free (tok);
  return (void *) 1;
}

void *
ts_check_or (char *tok)
{
  bool is_or;
  if (!tok)
    {
      ts_oom ();
      return NULL;
    }
  is_or = strcmp (tok, "or") == 0;
  free (tok);
  return is_or ? (void *) 1 : NULL;
}

static bool ts_reserved (const char *tok);

char *
ts_ident (char *tok)
{
  if (!tok)
    {
      ts_oom ();
      return NULL;
    }
  if (ts_reserved (tok))
    {
      free (tok); /* Reserved word where a name is required. */
      return NULL;
    }
  return tok; /* Adopted as the identifier text. */
}

static ts_node_t *
ts_newnode (node_kind_t kind, unsigned line)
{
  ts_node_t *n = ts_node_new (kind, line);
  if (!n)
    ts_oom ();
  return n;
}

void *
ts_mk_seq (void)
{
  /* N_SEQ wrapper: first = statement chain head, rhs = tail link
   * (non-owning; the tail is owned through the chain).  ts_node_free
   * knows rhs must not be freed twice. */
  return ts_newnode (N_SEQ, 1);
}

void *
ts_seq_append (void *seq, void *stmt)
{
  ts_node_t *s = (ts_node_t *) seq;
  ts_node_t *st = (ts_node_t *) stmt;
  if (!s || !st || s->kind != N_SEQ)
    return NULL; /* OOM upstream already recorded. */
  if (!s->first)
    s->first = st;
  else
    s->rhs->next = st;
  s->rhs = st;
  return s;
}

/* Argument accumulator (private to the front end). */
typedef struct
{
  ts_arg_t *v;
  size_t n;
  size_t cap;
} ts_args_t;

static void
ts_arg_free (ts_arg_t *a)
{
  if (!a)
    return;
  free (a->name);
  free (a->string);
  free (a);
}

static void
ts_args_free_all (ts_args_t *ac)
{
  size_t i;
  if (!ac)
    return;
  for (i = 0; i < ac->n; i++)
    {
      free (ac->v[i].name);
      free (ac->v[i].string);
    }
  free (ac->v);
  free (ac);
}

void *
ts_args_new (void)
{
  ts_args_t *ac = calloc (1, sizeof *ac);
  if (!ac)
    {
      ts_oom ();
      return NULL;
    }
  ac->v = calloc (4, sizeof *ac->v);
  if (!ac->v)
    {
      free (ac);
      ts_oom ();
      return NULL;
    }
  ac->cap = 4;
  return ac;
}

void *
ts_args_add (void *args, void *arg)
{
  ts_args_t *ac = (ts_args_t *) args;
  ts_arg_t *item = (ts_arg_t *) arg;
  ts_arg_t *nv;
  if (!ac || !item)
    {
      ts_arg_free (item);
      if (ac)
        ts_args_free_all (ac);
      return NULL;
    }
  if (ac->n == ac->cap)
    {
      size_t nc = ac->cap * 2;
      nv = realloc (ac->v, nc * sizeof *nv);
      if (!nv)
        {
          ts_arg_free (item);
          ts_args_free_all (ac);
          ts_oom ();
          return NULL;
        }
      ac->v = nv;
      ac->cap = nc;
    }
  ac->v[ac->n++] = *item;
  free (item);
  return ac;
}

static ts_arg_t *
ts_arg_new (unsigned line)
{
  ts_arg_t *a = calloc (1, sizeof *a);
  (void) line;
  if (!a)
    ts_oom ();
  return a;
}

void *
ts_mk_argstr (char *tok, unsigned line)
{
  ts_arg_t *a;
  ts_node_t *lit;
  if (!tok)
    return NULL;
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  /* Reuse literal string unescaping, then move the decoded text over. */
  lit = (ts_node_t *) ts_mk_string (tok, line);
  if (!lit)
    {
      free (a);
      return NULL;
    }
  a->tag = 2;
  a->string = lit->string;
  lit->string = NULL;
  ts_node_free (lit);
  return a;
}

void *
ts_mk_argnum (char *tok, unsigned line)
{
  ts_arg_t *a;
  ts_node_t *lit;
  if (!tok)
    return NULL;
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  lit = (ts_node_t *) ts_mk_number (tok, line);
  if (!lit)
    {
      free (a);
      return NULL;
    }
  a->tag = 1;
  a->number = lit->number;
  ts_node_free (lit);
  return a;
}

/* Shared word dispatch for ts_mk_unary (tail == NULL), ts_mk_argword
 * and ts_ident: true/false/nil become literals, reserved words are
 * rejected (the old hand lexer produced distinct keyword tokens, so
 * none of these could ever be a reference).  `or`/`do`/`end`/`else`
 * must fail in argument position or greedy call arguments would swallow
 * the operator/body delimiters that follow a call.  Returns 1 and fills
 * *kind_out for a literal, 0 for a plain name, -1 for reserved. */
static bool
ts_reserved (const char *tok)
{
  static const char *const words[] = {
    "or", "do", "end", "else", "while", "if", "const", NULL
  };
  size_t i;
  for (i = 0; words[i]; i++)
    if (strcmp (tok, words[i]) == 0)
      return true;
  return false;
}

static int
ts_word_lit (const char *tok, node_kind_t *kind_out, bool *boolean_out)
{
  if (strcmp (tok, "true") == 0)
    {
      *kind_out = N_LIT_BOOL;
      *boolean_out = true;
      return 1;
    }
  if (strcmp (tok, "false") == 0)
    {
      *kind_out = N_LIT_BOOL;
      *boolean_out = false;
      return 1;
    }
  if (strcmp (tok, "nil") == 0)
    {
      *kind_out = N_LIT_NIL;
      *boolean_out = false;
      return 1;
    }
  if (ts_reserved (tok))
    return -1;
  return 0;
}

void *
ts_mk_argword (char *tok, unsigned line)
{
  ts_arg_t *a;
  node_kind_t kind;
  bool boolean = false;
  int disp;
  if (!tok)
    return NULL;
  disp = ts_word_lit (tok, &kind, &boolean);
  if (disp < 0)
    {
      free (tok); /* Reserved `or`: fail the arg, backtrack. */
      return NULL;
    }
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  if (disp > 0)
    {
      free (tok);
      if (kind == N_LIT_NIL)
        a->tag = 4;
      else
        {
          a->tag = 3;
          a->boolean = boolean;
        }
      return a;
    }
  a->tag = 0;
  a->name = tok;
  return a;
}

void *
ts_mk_argref (char *tok, unsigned line)
{
  ts_arg_t *a;
  if (!tok)
    return NULL;
  a = ts_arg_new (line);
  if (!a)
    {
      free (tok);
      return NULL;
    }
  a->tag = 0;
  a->is_ref = true;
  a->name = tok;
  return a;
}

void *
ts_mk_number (char *tok, unsigned line)
{
  ts_node_t *n;
  char *end = NULL;
  long long v;
  if (!tok)
    return NULL;
  errno = 0;
  v = strtoll (tok, &end, 10);
  if (errno == ERANGE || !end || *end != '\0')
    {
      free (tok); /* Malformed/out-of-range: fail the rule. */
      return NULL;
    }
  n = ts_newnode (N_LIT_INT, line);
  free (tok);
  if (!n)
    return NULL;
  n->number = (int64_t) v;
  return n;
}

void *
ts_mk_string (char *tok, unsigned line)
{
  ts_node_t *n;
  size_t len;
  char *out;
  size_t i, o;
  if (!tok)
    return NULL;
  len = strlen (tok);
  if (len < 2 || tok[0] != '"' || tok[len - 1] != '"')
    {
      free (tok);
      return NULL;
    }
  out = malloc (len); /* Decoded form is strictly shorter. */
  if (!out)
    {
      free (tok);
      ts_oom ();
      return NULL;
    }
  /* Same escape set as the historical hand lexer: n t r " \ 0. */
  for (i = 1, o = 0; i + 1 < len;)
    {
      char c = tok[i++];
      if (c != '\\')
        {
          out[o++] = c;
          continue;
        }
      if (i >= len)
        break;
      switch (tok[i++])
        {
        case 'n': out[o++] = '\n'; break;
        case 't': out[o++] = '\t'; break;
        case 'r': out[o++] = '\r'; break;
        case '"': out[o++] = '"'; break;
        case '\\': out[o++] = '\\'; break;
        case '0': out[o++] = '\0'; break;
        default:
          free (out);
          free (tok);
          return NULL; /* Bad escape: fail the rule. */
        }
    }
  out[o] = '\0';
  n = ts_newnode (N_LIT_STR, line);
  free (tok);
  if (!n)
    {
      free (out);
      return NULL;
    }
  n->string = out;
  return n;
}

void *
ts_mk_var (char *name, unsigned line)
{
  ts_node_t *n;
  if (!name)
    return NULL;
  n = ts_newnode (N_VAR, line);
  if (!n)
    {
      free (name);
      return NULL;
    }
  n->name = name;
  return n;
}

void *
ts_mk_ref (char *name, unsigned line)
{
  /* `&name` is an explicit-reference sigil with identical value
   * semantics to a plain reference; kept as N_VAR. */
  return ts_mk_var (name, line);
}

void *
ts_mk_callrest (char *func, void *args, unsigned line)
{
  ts_args_t *ac = (ts_args_t *) args;
  ts_node_t *n;
  if (!func || !ac)
    {
      free (func);
      if (ac)
        ts_args_free_all (ac);
      return NULL;
    }
  n = ts_newnode (N_CALL, line);
  if (!n)
    {
      free (func);
      ts_args_free_all (ac);
      return NULL;
    }
  n->func = func; /* module filled in by ts_mk_unary. */
  n->args = ac->v;
  n->nargs = ac->n;
  free (ac);
  return n;
}

void *
ts_mk_unary (char *name, void *tail, unsigned line)
{
  ts_node_t *call = (ts_node_t *) tail;
  if (!name)
    {
      if (call)
        ts_node_free (call);
      return NULL;
    }
  if (!call)
    {
      node_kind_t kind;
      bool boolean = false;
      int disp = ts_word_lit (name, &kind, &boolean);
      if (disp < 0)
        {
          free (name); /* Reserved `or`: fail the rule. */
          return NULL;
        }
      if (disp > 0)
        {
          ts_node_t *n = ts_newnode (kind, line);
          free (name);
          if (!n)
            return NULL;
          if (kind == N_LIT_BOOL)
            n->boolean = boolean;
          return n;
        }
      return ts_mk_var (name, line);
    }
  call->module = name;
  call->line = line;
  return call;
}

static void *
ts_mk_stmt2 (node_kind_t kind, const char *name, void *rhs, unsigned line)
{
  ts_node_t *n = ts_newnode (kind, line);
  if (!n)
    {
      free ((void *) name);
      ts_node_free ((ts_node_t *) rhs);
      return NULL;
    }
  n->name = (char *) name;
  n->rhs = (ts_node_t *) rhs;
  if (!n->name || !n->rhs)
    {
      /* Partial build can only happen on OOM, already recorded. */
      ts_node_free (n);
      return NULL;
    }
  return n;
}

void *
ts_mk_const (char *name, void *expr, unsigned line)
{
  return ts_mk_stmt2 (N_CONST, name, expr, line);
}

void *
ts_mk_assign (char *name, void *expr, unsigned line)
{
  return ts_mk_stmt2 (N_ASSIGN, name, expr, line);
}

void *
ts_mk_exprstmt (void *expr, unsigned line)
{
  ts_node_t *n = ts_newnode (N_EXPR_STMT, line);
  if (!n)
    {
      ts_node_free ((ts_node_t *) expr);
      return NULL;
    }
  n->rhs = (ts_node_t *) expr;
  if (!n->rhs)
    {
      ts_node_free (n);
      return NULL;
    }
  return n;
}

void *
ts_mk_while (void *cond, void *body, unsigned line)
{
  ts_node_t *n = ts_newnode (N_WHILE, line);
  if (!n)
    {
      ts_node_free ((ts_node_t *) cond);
      ts_node_free ((ts_node_t *) body);
      return NULL;
    }
  n->cond = (ts_node_t *) cond;
  {
    ts_node_t *b = (ts_node_t *) body;
    if (!n->cond || !b)
      {
        ts_node_free (b);
        ts_node_free (n);
        return NULL;
      }
    /* Unwrap the N_SEQ body wrapper, keeping its statement chain. */
    n->first = b->first;
    free (b);
  }
  return n;
}

void *
ts_mk_if (void *cond, void *thenb, void *elseb, unsigned line)
{
  ts_node_t *n = ts_newnode (N_IF, line);
  ts_node_t *then_seq = (ts_node_t *) thenb;
  ts_node_t *else_seq = (ts_node_t *) elseb;
  if (!n)
    {
      ts_node_free ((ts_node_t *) cond);
      ts_node_free (then_seq);
      ts_node_free (else_seq);
      return NULL;
    }
  n->cond = (ts_node_t *) cond;
  if (!n->cond || !then_seq)
    {
      ts_node_free (then_seq);
      ts_node_free (else_seq);
      ts_node_free (n);
      return NULL;
    }
  /* Unwrap the N_SEQ branch wrappers, keeping their chains. */
  n->first = then_seq->first;
  n->otherwise = else_seq ? else_seq->first : NULL;
  free (then_seq);
  free (else_seq);
  return n;
}

void *
ts_mk_or (void *lhs, void *rhs, unsigned line)
{
  ts_node_t *n = ts_newnode (N_OR, line);
  if (!n)
    {
      ts_node_free ((ts_node_t *) lhs);
      ts_node_free ((ts_node_t *) rhs);
      return NULL;
    }
  n->lhs = (ts_node_t *) lhs;
  n->rhs = (ts_node_t *) rhs;
  if (!n->lhs || !n->rhs)
    {
      ts_node_free (n);
      return NULL;
    }
  return n;
}

/* Parse one input under the front-end lock.  Returns the N_SEQ program
 * wrapper, or NULL (oom set on allocation failure). */
static void *
ts_try_parse (const char *input, bool *oom)
{
  void *r;
  pthread_mutex_lock (&ts_parse_mutex);
  ts_pctx.start = input;
  ts_pctx.end = input + strlen (input);
  ts_pctx.oom = false;
  r = ts_gram_parse (input);
  if (oom)
    *oom = ts_pctx.oom;
  ts_pctx.start = NULL;
  ts_pctx.end = NULL;
  pthread_mutex_unlock (&ts_parse_mutex);
  return r;
}

/* Best-effort syntax-error offset: longest complete-parse prefix ending
 * after a newline or semicolon.  Any complete prefix proves the error is
 * at or after its end.  Bounded (count and input size) to stay cheap. */
static size_t
ts_error_offset (const char *source)
{
  size_t len = strlen (source);
  size_t *cands = NULL;
  size_t ncands = 0, cap = 0;
  size_t i, best = 0;
  bool oom = false;
  void *r;
  if (len > 65536)
    return 0;
  for (i = 0; i < len && ncands < 2000; i++)
    if (source[i] == '\n' || source[i] == ';')
      {
        if (ncands == cap)
          {
            size_t ncap = cap ? cap * 2 : 64;
            size_t *nv = realloc (cands, ncap * sizeof *nv);
            if (!nv)
              break;
            cands = nv;
            cap = ncap;
          }
        cands[ncands++] = i + 1;
      }
  for (i = ncands; i > 0; i--)
    {
      size_t off = cands[i - 1];
      char *prefix = malloc (off + 1);
      if (!prefix)
        break;
      memcpy (prefix, source, off);
      prefix[off] = '\0';
      r = ts_try_parse (prefix, &oom);
      free (prefix);
      if (oom)
        break;
      if (r)
        {
          ts_node_free ((ts_node_t *) r);
          best = off;
          break;
        }
    }
  free (cands);
  return best;
}

static TS_Status
ts_parse_program (const char *source, ts_node_t **out, TS_Error *error)
{
  bool oom = false;
  void *r = ts_try_parse (source, &oom);
  if (r)
    {
      *out = (ts_node_t *) r;
      return TS_OK;
    }
  if (oom)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  *out = NULL;
  ts_error_set (error, TS_ERR_PARSE, 0, ts_error_offset (source),
              "termscript syntax error");
  return TS_ERR_PARSE;
}
/* ---------------- interpreter ---------------- */

static ts_var_t *
vm_lookup (TS_VM *vm, const char *name)
{
  ts_var_t *v;
  for (v = vm->vars; v; v = v->next)
    if (strcmp (v->name, name) == 0)
      return v;
  return NULL;
}

static TS_Status
vm_step_budget (TS_VM *vm, TS_Error *error)
{
  if (++vm->steps > TS_MAX_STEPS)
    {
      ts_error_set (error, TS_ERR_LIMIT, 0, 0, "termscript step budget hit");
      return TS_ERR_LIMIT;
    }
  return TS_OK;
}

static TS_Status eval_node (TS_VM *vm, ts_node_t *n, TS_Value *out,
                            TS_Error *error);
static TS_Status exec_list (TS_VM *vm, ts_node_t *list, TS_Error *error);

/* Shared call dispatcher: invoke `func` on the module carried by the
 * handle value `modval` (borrowed).  Used by the tree-walking
 * interpreter and by ts_rt_call for ahead-of-time compiled C. */
static TS_Status
ts_call_with_handle (TS_VM *vm, const TS_Value *modval, const char *modname,
                     const char *func, const TS_Value *argv, size_t argc,
                     TS_Value *ret, unsigned line, TS_Error *error)
{
  TS_Handle *h;
  const TS_FuncDef *f = NULL;
  const TS_Module *mod = NULL;
  size_t i;
  if (!modval || modval->type != TS_HANDLE ||
      !(h = modval->as.handle) || !(mod = ts_handle_module (h)))
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, line,
                  "module '%s' is not bound", modname ? modname : "?");
      return TS_ERR_NOTFOUND;
    }
  for (i = 0; mod->funcs[i].name; i++)
    if (strcmp (mod->funcs[i].name, func) == 0)
      {
        f = &mod->funcs[i];
        break;
      }
  if (!f)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, line,
                  "no such function '%s:%s'", modname ? modname : "?",
                  func ? func : "?");
      return TS_ERR_NOTFOUND;
    }
  ret->type = TS_NIL;
  return f->func (vm, f->userdata, argv, argc, ret, error);
}

static TS_Status
eval_call (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  ts_var_t *mv = vm_lookup (vm, n->module);
  TS_Value *argv = NULL;
  TS_Value modval;
  TS_Status st;
  size_t i;
  modval.type = TS_NIL;
  if (mv)
    {
      if (ts_value_copy (&modval, &mv->value) != TS_OK)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  if (n->nargs)
    {
      argv = calloc (n->nargs, sizeof *argv);
      if (!argv)
        {
          ts_value_release (&modval);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      for (i = 0; i < n->nargs; i++)
        {
          ts_arg_t *a = &n->args[i];
          if (a->tag == 0)
            {
              ts_var_t *vv = vm_lookup (vm, a->name);
              if (!vv)
                {
                  size_t k;
                  for (k = 0; k < i; k++)
                    ts_value_release (&argv[k]);
                  free (argv);
                  ts_value_release (&modval);
                  ts_error_set (error, TS_ERR_NOTFOUND, 0, n->line,
                              "unbound variable '%s'", a->name);
                  return TS_ERR_NOTFOUND;
                }
              if (ts_value_copy (&argv[i], &vv->value) != TS_OK)
                {
                  size_t k;
                  for (k = 0; k < i; k++)
                    ts_value_release (&argv[k]);
                  free (argv);
                  ts_value_release (&modval);
                  ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                              "out of memory");
                  return TS_ERR_NOMEM;
                }
            }
          else if (a->tag == 1)
            {
              argv[i].type = TS_INT;
              argv[i].as.integer = a->number;
            }
          else if (a->tag == 2)
            {
              argv[i].type = TS_STRING;
              argv[i].as.string = strdup (a->string);
              if (!argv[i].as.string)
                {
                  size_t k;
                  for (k = 0; k < i; k++)
                    ts_value_release (&argv[k]);
                  free (argv);
                  ts_value_release (&modval);
                  ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                              "out of memory");
                  return TS_ERR_NOMEM;
                }
            }
          else if (a->tag == 3)
            {
              argv[i].type = TS_BOOL;
              argv[i].as.boolean = a->boolean;
            }
          else
            argv[i].type = TS_NIL;
        }
    }
  st = ts_call_with_handle (vm, mv ? &modval : NULL, n->module, n->func,
                            argv, n->nargs, out, n->line, error);
  ts_value_release (&modval);
  if (argv)
    {
      for (i = 0; i < n->nargs; i++)
        ts_value_release (&argv[i]);
      free (argv);
    }
  return st;
}

static TS_Status
eval_node (TS_VM *vm, ts_node_t *n, TS_Value *out, TS_Error *error)
{
  TS_Status st;
  if (!n)
    {
      out->type = TS_NIL;
      return TS_OK;
    }
  if (vm_step_budget (vm, error) != TS_OK)
    return TS_ERR_LIMIT;
  switch (n->kind)
    {
    case N_VAR:
      {
        ts_var_t *v = vm_lookup (vm, n->name);
        if (!v)
          {
            ts_error_set (error, TS_ERR_NOTFOUND, 0, n->line,
                        "unbound variable '%s'", n->name);
            return TS_ERR_NOTFOUND;
          }
        return ts_value_copy (out, &v->value) == TS_OK ? TS_OK :
                                                        TS_ERR_NOMEM;
      }
    case N_LIT_INT:
      out->type = TS_INT;
      out->as.integer = n->number;
      return TS_OK;
    case N_LIT_STR:
      out->type = TS_STRING;
      out->as.string = strdup (n->string);
      if (!out->as.string)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      return TS_OK;
    case N_LIT_BOOL:
      out->type = TS_BOOL;
      out->as.boolean = n->boolean;
      return TS_OK;
    case N_LIT_NIL:
      out->type = TS_NIL;
      return TS_OK;
    case N_CALL:
      return eval_call (vm, n, out, error);
    case N_OR:
      {
        TS_Value lv;
        lv.type = TS_NIL;
        st = eval_node (vm, n->lhs, &lv, error);
        if (st != TS_OK)
          return st;
        if (ts_value_truthy (&lv))
          {
            *out = lv;
            return TS_OK;
          }
        ts_value_release (&lv);
        return eval_node (vm, n->rhs, out, error);
      }
    default:
      ts_error_set (error, TS_ERR_PARSE, 0, n->line,
                  "not an expression");
      return TS_ERR_PARSE;
    }
}

static TS_Status
vm_define (TS_VM *vm, const char *name, TS_Value *val, bool is_const,
           unsigned line, TS_Error *error)
{
  ts_var_t *v = vm_lookup (vm, name);
  if (v)
    {
      if (v->is_const)
        {
          ts_error_set (error, TS_ERR_INVAL, 0, line,
                      "cannot reassign const '%s'", name);
          return TS_ERR_INVAL;
        }
      ts_value_release (&v->value);
      v->value = *val;
      val->type = TS_NIL;
      val->as.handle = NULL;
      v->is_const = is_const;
      return TS_OK;
    }
  v = calloc (1, sizeof *v);
  if (!v)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v->name = strdup (name);
  if (!v->name)
    {
      free (v);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v->value = *val;
  val->type = TS_NIL;
  val->as.handle = NULL;
  v->is_const = is_const;
  v->next = vm->vars;
  vm->vars = v;
  return TS_OK;
}

static TS_Status
exec_list (TS_VM *vm, ts_node_t *list, TS_Error *error)
{
  ts_node_t *s;
  for (s = list; s; s = s->next)
    {
      TS_Status st;
      if (vm_step_budget (vm, error) != TS_OK)
        return TS_ERR_LIMIT;
      switch (s->kind)
        {
        case N_CONST:
        case N_ASSIGN:
          {
            TS_Value v;
            v.type = TS_NIL;
            st = eval_node (vm, s->rhs, &v, error);
            if (st != TS_OK)
              return st;
            st = vm_define (vm, s->name, &v, s->kind == N_CONST, s->line,
                            error);
            ts_value_release (&v);
            if (st != TS_OK)
              return st;
          }
          break;
        case N_EXPR_STMT:
          {
            TS_Value v;
            v.type = TS_NIL;
            st = eval_node (vm, s->rhs, &v, error);
            ts_value_release (&v);
            if (st != TS_OK)
              return st;
          }
          break;
        case N_WHILE:
          for (;;)
            {
              TS_Value c;
              bool go;
              c.type = TS_NIL;
              st = eval_node (vm, s->cond, &c, error);
              if (st != TS_OK)
                return st;
              go = ts_value_truthy (&c);
              ts_value_release (&c);
              if (!go)
                break;
              st = exec_list (vm, s->first, error);
              if (st != TS_OK)
                return st;
            }
          break;
        case N_IF:
          {
            TS_Value c;
            bool go;
            c.type = TS_NIL;
            st = eval_node (vm, s->cond, &c, error);
            if (st != TS_OK)
              return st;
            go = ts_value_truthy (&c);
            ts_value_release (&c);
            st = exec_list (vm, go ? s->first : s->otherwise, error);
            if (st != TS_OK)
              return st;
          }
          break;
        default:
          ts_error_set (error, TS_ERR_PARSE, 0, s->line, "bad statement");
          return TS_ERR_PARSE;
        }
    }
  return TS_OK;
}

/* ---------------- compiled-code runtime ---------------- */

TS_Status
ts_rt_call (TS_VM *vm, const TS_Value *mod, const char *modname,
            const char *func, const TS_Value *argv, size_t argc,
            TS_Value *ret, unsigned line, TS_Error *error)
{
  if (!vm || !mod || !func || !ret)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line, "bad call");
      return TS_ERR_INVAL;
    }
  return ts_call_with_handle (vm, mod, modname, func, argv, argc, ret,
                              line, error);
}

TS_Status
ts_rt_get (TS_VM *vm, const char *name, TS_Value *out, unsigned line,
           TS_Error *error)
{
  ts_var_t *v;
  if (!vm || !name || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line, "bad arguments");
      return TS_ERR_INVAL;
    }
  v = vm_lookup (vm, name);
  if (!v)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, line,
                    "unbound variable '%s'", name);
      return TS_ERR_NOTFOUND;
    }
  if (ts_value_copy (out, &v->value) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

TS_Status
ts_rt_set (TS_VM *vm, const char *name, TS_Value *val, bool is_const,
           unsigned line, TS_Error *error)
{
  if (!vm || !name || !val)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, line, "bad arguments");
      return TS_ERR_INVAL;
    }
  return vm_define (vm, name, val, is_const, line, error);
}

TS_Status
ts_rt_step (TS_VM *vm, TS_Error *error)
{
  if (!vm)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad vm");
      return TS_ERR_INVAL;
    }
  return vm_step_budget (vm, error);
}

const char *
ts_rt_output (const TS_VM *vm)
{
  if (!vm || !vm->output)
    return "";
  return vm->output;
}

/* ---------------- VM lifetime ---------------- */

static TS_Status
vm_bind_core (TS_VM *vm, TS_Error *error)
{
  const TS_Module *g = ts_vm_lookup (vm, "G");
  TS_Handle *h;
  TS_Value v;
  if (!g)
    {
      ts_error_set (error, TS_ERR_SYSTEM, 0, 0, "G module missing");
      return TS_ERR_SYSTEM;
    }
  h = ts_handle_new_module (g);
  if (!h)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v.type = TS_HANDLE;
  v.as.handle = h;
  return vm_define (vm, "G", &v, true, 0, error);
}

TS_VM *
ts_vm_create (TS_Error *error)
{
  TS_VM *vm = calloc (1, sizeof *vm);
  TS_Status st;
  if (!vm)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  if (pthread_mutex_init (&vm->mod_mutex, NULL) != 0)
    {
      free (vm);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  st = ts_vm_register_module (vm, &g_module, error);
  if (st != TS_OK)
    {
      ts_vm_free (vm);
      return NULL;
    }
  if (vm_bind_core (vm, error) != TS_OK)
    {
      ts_vm_free (vm);
      return NULL;
    }
  return vm;
}

void
ts_vm_free (TS_VM *vm)
{
  ts_var_t *v;
  if (!vm)
    return;
  v = vm->vars;
  while (v)
    {
      ts_var_t *nx = v->next;
      free (v->name);
      ts_value_release (&v->value);
      free (v);
      v = nx;
    }
  free (vm->output);
  free (vm->compiled);
  {
    size_t i;
    for (i = 0; i < vm->nspaths; i++)
      free (vm->spaths[i]);
    free (vm->spaths);
    for (i = 0; i < vm->nistack; i++)
      free (vm->istack[i]);
    free (vm->istack);
  }
  pthread_mutex_destroy (&vm->mod_mutex);
  free (vm);
}

static void
vm_reset_run (TS_VM *vm)
{
  vm->steps = 0;
  vm->olen = 0;
  if (vm->output)
    vm->output[0] = '\0';
}

/* Parse `source` and execute it.  When `reset` is true the per-run
 * state (step budget, output accumulator) is cleared first, as for
 * ts_vm_run_string; imports pass false so variables, output and the
 * step budget are shared with the importing program. */
static TS_Status
ts_exec_source (TS_VM *vm, const char *source, bool reset, TS_Error *error)
{
  ts_node_t *prog;
  TS_Status st;
  if (ts_parse_program (source, &prog, error) != TS_OK)
    return error ? error->code : TS_ERR_PARSE;
  if (reset)
    vm_reset_run (vm);
  st = exec_list (vm, prog->first, error);
  ts_node_free (prog);
  return st;
}

TS_Status
ts_vm_run_string (TS_VM *vm, const char *source, char **output,
                          TS_Error *error)
{
  TS_Status st;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  st = ts_exec_source (vm, source, true, error);
  if (st != TS_OK)
    return st;
  if (output)
    {
      *output = strdup (vm->output ? vm->output : "");
      if (!*output)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  return TS_OK;
}

TS_Status
ts_vm_run_file (TS_VM *vm, const char *path, char **output,
                        TS_Error *error)
{
  FILE *f;
  char *buf = NULL;
  size_t cap = 4096, len = 0, r;
  TS_Status st;
  if (!vm || !path)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  f = fopen (path, "rb");
  if (!f)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "cannot open '%s'", path);
      return TS_ERR_SYSTEM;
    }
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  while ((r = fread (buf + len, 1, cap - len - 1, f)) > 0)
    {
      len += r;
      if (len + 1 >= cap)
        {
          char *nb;
          if (cap >= TS_MAX_FILE)
            {
              free (buf);
              fclose (f);
              ts_error_set (error, TS_ERR_LIMIT, 0, 0, "script too large");
              return TS_ERR_LIMIT;
            }
          cap *= 2;
          nb = realloc (buf, cap);
          if (!nb)
            {
              free (buf);
              fclose (f);
              ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                          "out of memory");
              return TS_ERR_NOMEM;
            }
          buf = nb;
        }
    }
  fclose (f);
  buf[len] = '\0';
  st = ts_vm_run_string (vm, buf, output, error);
  free (buf);
  return st;
}

TS_Status
ts_vm_add_search_path (TS_VM *vm, const char *dir, TS_Error *error)
{
  char *copy;
  char **nv;
  size_t i;
  if (!vm || !dir || !*dir)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad search path");
      return TS_ERR_INVAL;
    }
  for (i = 0; i < vm->nspaths; i++)
    if (strcmp (vm->spaths[i], dir) == 0)
      return TS_OK; /* Idempotent: duplicates are ignored. */
  copy = strdup (dir);
  if (!copy)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (vm->nspaths == vm->spaths_cap)
    {
      size_t nc = vm->spaths_cap ? vm->spaths_cap * 2 : 4;
      nv = realloc (vm->spaths, nc * sizeof *nv);
      if (!nv)
        {
          free (copy);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      vm->spaths = nv;
      vm->spaths_cap = nc;
    }
  vm->spaths[vm->nspaths++] = copy;
  return TS_OK;
}

/* Names may only name a stdlib leaf: no directories, no escapes. */
static bool
ts_import_name_ok (const char *name)
{
  size_t i;
  if (!name || !*name || strlen (name) > 128)
    return false;
  if (strcmp (name, ".") == 0 || strcmp (name, "..") == 0)
    return false;
  for (i = 0; name[i]; i++)
    {
      char c = name[i];
      if (!(isalnum ((unsigned char) c) || c == '_' || c == '-' ||
            c == '.'))
        return false;
    }
  return true;
}

/* Read a whole file (caller frees). NULL with error set on failure. */
static char *
ts_slurp (const char *path, TS_Error *error)
{
  FILE *f = fopen (path, "rb");
  char *buf;
  size_t cap = 4096, len = 0, r;
  if (!f)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0,
                    "cannot open '%s'", path);
      return NULL;
    }
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  while ((r = fread (buf + len, 1, cap - len - 1, f)) > 0)
    {
      char *nb;
      len += r;
      if (len + 1 < cap)
        continue;
      if (cap >= TS_MAX_FILE)
        {
          free (buf);
          fclose (f);
          ts_error_set (error, TS_ERR_LIMIT, 0, 0, "script too large");
          return NULL;
        }
      cap *= 2;
      nb = realloc (buf, cap);
      if (!nb)
        {
          free (buf);
          fclose (f);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return NULL;
        }
      buf = nb;
    }
  fclose (f);
  buf[len] = '\0';
  return buf;
}

/* Try "<dir>/<name>.tsc" then "<dir>/<name>/<name>.tsc".  Returns a
 * malloc'd path on success, NULL otherwise (no error details: callers
 * keep scanning further directories). */
static char *
ts_probe_dir (const char *dir, const char *name)
{
  static const char *const layouts[] = { "%s/%s.tsc", "%s/%s/%s.tsc" };
  size_t li;
  for (li = 0; li < 2; li++)
    {
      char path[4096];
      FILE *probe;
      int n;
      if (li == 0)
        n = snprintf (path, sizeof path, layouts[0], dir, name);
      else
        n = snprintf (path, sizeof path, layouts[1], dir, name, name);
      if (n < 0 || (size_t) n >= sizeof path)
        continue;
      probe = fopen (path, "rb");
      if (probe)
        {
          fclose (probe);
          return strdup (path);
        }
    }
  return NULL;
}

TS_Status
ts_vm_import (TS_VM *vm, const char *name, TS_Error *error)
{
  char *path = NULL;
  char *text = NULL;
  TS_Status st;
  size_t i;
  if (!vm || !ts_import_name_ok (name))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad import name '%s'",
                    name ? name : "(null)");
      return TS_ERR_INVAL;
    }
  for (i = 0; i < vm->nspaths && !path; i++)
    path = ts_probe_dir (vm->spaths[i], name);
  if (!path)
    {
      /* TERMSCRIPT_PATH is consulted on every import so tests and
       * install trees can relocate the stdlib without VM state. */
      const char *env = getenv ("TERMSCRIPT_PATH");
      char *dirs = env ? strdup (env) : NULL;
      if (dirs)
        {
          char *save = NULL, *d = strtok_r (dirs, ":", &save);
          while (d && !path)
            {
              if (*d)
                path = ts_probe_dir (d, name);
              d = strtok_r (NULL, ":", &save);
            }
          free (dirs);
        }
    }
  if (!path)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, 0,
                    "stdlib module '%s' not found", name);
      return TS_ERR_NOTFOUND;
    }
  for (i = 0; i < vm->nistack; i++)
    if (strcmp (vm->istack[i], path) == 0)
      {
        free (path);
        ts_error_set (error, TS_ERR_INVAL, 0, 0,
                      "import cycle on '%s'", name);
        return TS_ERR_INVAL;
      }
  text = ts_slurp (path, error);
  if (!text)
    {
      TS_Status code = error ? error->code : TS_ERR_SYSTEM;
      free (path);
      return code;
    }
  if (vm->nistack == vm->istack_cap)
    {
      size_t nc = vm->istack_cap ? vm->istack_cap * 2 : 4;
      char **nv = realloc (vm->istack, nc * sizeof *nv);
      if (!nv)
        {
          free (text);
          free (path);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      vm->istack = nv;
      vm->istack_cap = nc;
    }
  vm->istack[vm->nistack++] = path; /* Owned until pop below. */
  st = ts_exec_source (vm, text, false, error);
  free (text);
  free (vm->istack[--vm->nistack]);
  if (st != TS_OK)
    return st;
  return TS_OK;
}

TS_Status
ts_check_syntax (const char *source, TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Status st;
  if (!source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  st = ts_parse_program (source, &prog, error);
  ts_node_free (prog);
  return st;
}

/* ---------------- C backend (AOT transpiler) ----------------
 *
 * ts_vm_compile_to_c compiles Termscript to real C: statements become
 * C control flow and every expression becomes value construction plus
 * calls through the ts_rt_* compiled-code runtime above.  No source
 * text is embedded and nothing is parsed at run time.
 *
 * Non-native libraries (`.tsc` companions loaded with
 * `G:import "name"`) are compiled too: constant-string `G:import`
 * sites are resolved through the compile-time search paths (the VM
 * paths plus TERMSCRIPT_PATH) and their statements are inlined as
 * compiled C.  Anything else (dynamic names, cycles, missing files,
 * unparsable companions) stays a run-time `G:import` call with
 * identical VM semantics.  Native modules are never embedded: the
 * generated unit links them (statically) and resolves them through
 * the VM registry at run time.
 *
 * The same emitter serves the DomTERM backend (dt_mode): only type
 * and helper names change (DT_TermValue / dt_rt_* / DT_*).  See
 * dt_termscript_compile_to_c, which forwards here.
 */

struct ts_cg_paths
{
  char **v;
  size_t n;
  size_t cap;
};

static void
ts_cg_paths_free (struct ts_cg_paths *p)
{
  size_t i;
  if (!p)
    return;
  for (i = 0; i < p->n; i++)
    free (p->v[i]);
  free (p->v);
  p->v = NULL;
  p->n = p->cap = 0;
}

static bool
ts_cg_paths_add (struct ts_cg_paths *p, const char *dir)
{
  size_t i;
  char *copy;
  char **nv;
  for (i = 0; i < p->n; i++)
    if (strcmp (p->v[i], dir) == 0)
      return true;
  copy = strdup (dir);
  if (!copy)
    return false;
  if (p->n == p->cap)
    {
      size_t nc = p->cap ? p->cap * 2 : 4;
      nv = realloc (p->v, nc * sizeof *nv);
      if (!nv)
        {
          free (copy);
          return false;
        }
      p->v = nv;
      p->cap = nc;
    }
  p->v[p->n++] = copy;
  return true;
}

/* Resolve `name` like ts_vm_import: each search dir in order, then
 * TERMSCRIPT_PATH.  Returns a malloc'd path or NULL. */
static char *
ts_cg_resolve (struct ts_cg_paths *p, const char *name)
{
  char *path = NULL;
  size_t i;
  for (i = 0; i < p->n && !path; i++)
    path = ts_probe_dir (p->v[i], name);
  if (!path)
    {
      const char *env = getenv ("TERMSCRIPT_PATH");
      char *dirs = env ? strdup (env) : NULL;
      if (dirs)
        {
          char *save = NULL, *d = strtok_r (dirs, ":", &save);
          while (d && !path)
            {
              if (*d)
                path = ts_probe_dir (d, name);
              d = strtok_r (NULL, ":", &save);
            }
          free (dirs);
        }
    }
  return path;
}

static bool
ts_cg_stack_has (char **stack, size_t nstack, const char *path)
{
  size_t i;
  for (i = 0; i < nstack; i++)
    if (strcmp (stack[i], path) == 0)
      return true;
  return false;
}

/* Free one statement node without touching its `next` link. */
static void
ts_cg_free_one (ts_node_t *s)
{
  ts_node_t *nx;
  if (!s)
    return;
  nx = s->next;
  s->next = NULL;
  ts_node_free (s);
  (void) nx;
}

static bool
ts_cg_is_bare_g_call (ts_node_t *s, const char *func, const char **str_out)
{
  ts_node_t *rhs;
  if (!s || s->kind != N_EXPR_STMT)
    return false;
  rhs = s->rhs;
  if (!rhs || rhs->kind != N_CALL || !rhs->module || !rhs->func)
    return false;
  if (strcmp (rhs->module, "G") != 0 || strcmp (rhs->func, func) != 0)
    return false;
  if (rhs->nargs != 1 || rhs->args[0].tag != 2 || !rhs->args[0].string)
    return false;
  if (str_out)
    *str_out = rhs->args[0].string;
  return true;
}

#define TS_CG_MAX_INLINE_DEPTH 16

/* Inline constant-string `G:import` companions into the chain in
 * place; `*head` is the chain head pointer.  `paths` evolves as
 * `G:import_path` statements are seen (mirroring run-time order). */
static void
ts_cg_inline_chain (ts_node_t **head, struct ts_cg_paths *paths,
                    char **stack, size_t nstack, unsigned depth)
{
  ts_node_t **pp = head;
  while (*pp)
    {
      ts_node_t *s = *pp;
      const char *str = NULL;
      if (s->kind == N_WHILE)
        {
          ts_cg_inline_chain (&s->first, paths, stack, nstack, depth);
          pp = &s->next;
          continue;
        }
      if (s->kind == N_IF)
        {
          ts_cg_inline_chain (&s->first, paths, stack, nstack, depth);
          ts_cg_inline_chain (&s->otherwise, paths, stack, nstack,
                              depth);
          pp = &s->next;
          continue;
        }
      if (ts_cg_is_bare_g_call (s, "import_path", &str))
        {
          /* Compile-time search path tracks run time; the statement
           * itself still runs (later dynamic imports need it). */
          ts_cg_paths_add (paths, str);
          pp = &s->next;
          continue;
        }
      if (!ts_cg_is_bare_g_call (s, "import", &str) ||
          !ts_import_name_ok (str) || depth > TS_CG_MAX_INLINE_DEPTH)
        {
          pp = &s->next;
          continue;
        }
      {
        char *found = ts_cg_resolve (paths, str);
        char *text = NULL;
        bool oom = false;
        void *r = NULL;
        ts_node_t *prog = NULL, *chain = NULL, *tail = NULL;
        TS_Error dummy;
        if (!found || ts_cg_stack_has (stack, nstack, found))
          {
            free (found);
            pp = &s->next;
            continue;
          }
        ts_error_clear (&dummy);
        text = ts_slurp (found, &dummy);
        if (!text)
          {
            free (found);
            pp = &s->next; /* Too large / unreadable: run time reports. */
            continue;
          }
        r = ts_try_parse (text, &oom);
        free (text);
        if (!r || oom)
          {
            /* Unparsable companion: leave the run-time import, which
             * fails identically when executed. */
            free (found);
            pp = &s->next;
            continue;
          }
        prog = (ts_node_t *) r;
        stack[nstack] = found;
        ts_cg_inline_chain (&prog->first, paths, stack, nstack + 1,
                            depth + 1);
        chain = prog->first;
        prog->first = NULL;
        ts_node_free (prog);
        if (!chain)
          {
            /* Empty companion: the import is a silent no-op. */
            *pp = s->next;
            s->next = NULL;
            ts_cg_free_one (s);
            free (found);
            continue;
          }
        for (tail = chain; tail->next; tail = tail->next)
          ;
        tail->next = s->next;
        s->next = NULL;
        ts_cg_free_one (s);
        *pp = chain;
        pp = &tail->next;
        free (found);
      }
    }
}

/* Growable C-text buffer with a sticky OOM flag. */
typedef struct
{
  char *buf;
  size_t len;
  size_t cap;
  unsigned tmp;
  bool dt_mode;
  bool failed;
} ts_cg_t;

static void
ts_cg_put (ts_cg_t *g, const char *s, size_t n)
{
  if (g->failed)
    return;
  if (g->len + n + 1 > g->cap)
    {
      size_t nc = g->cap ? g->cap * 2 : 1024;
      char *nb;
      while (nc < g->len + n + 1)
        nc *= 2;
      nb = realloc (g->buf, nc);
      if (!nb)
        {
          g->failed = true;
          return;
        }
      g->buf = nb;
      g->cap = nc;
    }
  memcpy (g->buf + g->len, s, n);
  g->len += n;
  g->buf[g->len] = '\0';
}

static void
ts_cg_str (ts_cg_t *g, const char *s)
{
  ts_cg_put (g, s, strlen (s));
}

static void
ts_cg_fmt (ts_cg_t *g, const char *fmt, ...)
{
  char tmp[1024];
  va_list ap;
  int n;
  va_start (ap, fmt);
  n = vsnprintf (tmp, sizeof tmp, fmt, ap);
  va_end (ap);
  if (n < 0)
    {
      g->failed = true;
      return;
    }
  if ((size_t) n >= sizeof tmp)
    {
      /* Long names/numbers only: heap path. */
      char *big = malloc ((size_t) n + 1);
      if (!big)
        {
          g->failed = true;
          return;
        }
      va_start (ap, fmt);
      vsnprintf (big, (size_t) n + 1, fmt, ap);
      va_end (ap);
      ts_cg_put (g, big, (size_t) n);
      free (big);
      return;
    }
  ts_cg_put (g, tmp, (size_t) n);
}

/* Name selectors for the two flavors. */
static const char *ts_cg_V (ts_cg_t *g) { return g->dt_mode ? "DT_TermValue" : "TS_Value"; }
static const char *ts_cg_S (ts_cg_t *g) { return g->dt_mode ? "DT_Status" : "TS_Status"; }

static void
ts_cg_cstr (ts_cg_t *g, const char *s)
{
  /* C string literal with octal escapes (always 3 digits, so no
   * \x-style run-on into following hex digits). */
  const unsigned char *p = (const unsigned char *) (s ? s : "");
  ts_cg_str (g, "\"");
  for (; *p; p++)
    {
      switch (*p)
        {
        case '"': ts_cg_str (g, "\\\""); break;
        case '\\': ts_cg_str (g, "\\\\"); break;
        case '\n': ts_cg_str (g, "\\n"); break;
        case '\t': ts_cg_str (g, "\\t"); break;
        case '\r': ts_cg_str (g, "\\r"); break;
        default:
          if (*p < 0x20 || *p > 0x7E)
            ts_cg_fmt (g, "\\%03o", *p);
          else
            ts_cg_put (g, (const char *) p, 1);
          break;
        }
    }
  ts_cg_str (g, "\"");
}

static unsigned
ts_cg_temp (ts_cg_t *g)
{
  return g->tmp++;
}

/* Short aliases so the emitter reads uniformly. */
#define CG_VMN(g) ((g)->dt_mode ? "dt_vm" : "ts_vm")
#define CG_ERN(g) ((g)->dt_mode ? "dt_err" : "ts_err")
#define CG_OK(g) ((g)->dt_mode ? "DT_OK" : "TS_OK")
#define CG_NOMEM(g) ((g)->dt_mode ? "DT_ERR_NO_MEMORY" : "TS_ERR_NOMEM")
#define CG_CALL(g) ((g)->dt_mode ? "dt_rt_call" : "ts_rt_call")
#define CG_GET(g) ((g)->dt_mode ? "dt_rt_get" : "ts_rt_get")
#define CG_SET(g) ((g)->dt_mode ? "dt_rt_set" : "ts_rt_set")
#define CG_STEP(g) ((g)->dt_mode ? "dt_rt_step" : "ts_rt_step")
#define CG_FREE(g) ((g)->dt_mode ? "dt_termvalue_free" : "ts_value_free")
#define CG_MKSTR(g) ((g)->dt_mode ? "dt_rt_mkstring" : "ts_value_make_string")
#define CG_TRUTHY(g) ((g)->dt_mode ? "dt_rt_truthy" : "ts_value_truthy")
#define CG_ERRSET(g) ((g)->dt_mode ? "dt_error_set" : "ts_error_set")
#define CG_PARSE(g) ((g)->dt_mode ? "DT_ERR_PARSE" : "TS_ERR_PARSE")

static void ts_cg_list (ts_cg_t *g, ts_node_t *list);
static void ts_cg_expr (ts_cg_t *g, ts_node_t *n, const char *out);

/* Emit one call argument into OUT (a declared value slot).  On failure
 * the snippet jumps to `fail` after running CLEANUP. */
static void
ts_cg_arg (ts_cg_t *g, ts_arg_t *a, const char *out, unsigned line,
           const char *cleanup)
{
  if (a->tag == 0)
    {
      ts_cg_fmt (g, "if (%s (%s, \"%s\", &%s, %u, %s) != %s) { %s }\n",
                 CG_GET (g), CG_VMN (g), a->name, out, line, CG_ERN (g),
                 CG_OK (g), cleanup);
    }
  else if (a->tag == 1)
    {
      ts_cg_fmt (g, "%s.type = %s; %s.as.integer = %lld;\n", out,
                 g->dt_mode ? "DT_TERM_INT" : "TS_INT", out,
                 (long long) a->number);
    }
  else if (a->tag == 2)
    {
      /* dt_rt_mkstring takes the error param and reports itself;
       * ts_value_make_string does not, so the caller reports NOMEM. */
      ts_cg_fmt (g, "if (%s (&%s, ", CG_MKSTR (g), out);
      ts_cg_cstr (g, a->string);
      if (g->dt_mode)
        ts_cg_fmt (g, ", %s) != %s) { %s }\n", CG_ERN (g), CG_OK (g),
                   cleanup);
      else
        ts_cg_fmt (g, ") != %s) { %s (%s, %s, ENOMEM, 0, \"out of memory\"); %s }\n",
                   CG_OK (g), CG_ERRSET (g), CG_ERN (g), CG_NOMEM (g),
                   cleanup);
    }
  else if (a->tag == 3)
    {
      ts_cg_fmt (g, "%s.type = %s; %s.as.boolean = %s;\n", out,
                 g->dt_mode ? "DT_TERM_BOOL" : "TS_BOOL", out,
                 a->boolean ? "true" : "false");
    }
  else
    ts_cg_fmt (g, "%s.type = %s;\n", out,
               g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
}

static void
ts_cg_call (ts_cg_t *g, ts_node_t *n, const char *out)
{
  unsigned m = ts_cg_temp (g);
  size_t i;
  char mv[64], av[64], rv[64], sv[64];
  snprintf (mv, sizeof mv, "ts_m%u", m);
  snprintf (av, sizeof av, "ts_a%u", m);
  snprintf (rv, sizeof rv, "ts_r%u", m);
  snprintf (sv, sizeof sv, "ts_s%u", m);
  ts_cg_fmt (g, "{\n%s %s; %s %s;\n", ts_cg_V (g), mv, ts_cg_V (g), rv);
  if (n->nargs)
    ts_cg_fmt (g, "%s %s[%zu]; size_t ts_k%u;\nfor (ts_k%u = 0; ts_k%u < %zu; ts_k%u++) %s[ts_k%u].type = %s;\n",
               ts_cg_V (g), av, n->nargs, m, m, m, n->nargs, m, av, m,
               g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
  ts_cg_fmt (g, "%s %s;\n", ts_cg_S (g), sv);
  ts_cg_fmt (g, "%s.type = %s; %s.type = %s;\n", mv,
             g->dt_mode ? "DT_TERM_NIL" : "TS_NIL", rv,
             g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
  ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
             CG_VMN (g), CG_ERN (g), CG_OK (g));
  ts_cg_fmt (g, "if (%s (%s, \"%s\", &%s, %u, %s) != %s) goto fail;\n",
             CG_GET (g), CG_VMN (g), n->module, mv, n->line, CG_ERN (g),
             CG_OK (g));
  for (i = 0; i < n->nargs; i++)
    {
      char slot[96], cl[512];
      snprintf (slot, sizeof slot, "%s[%zu]", av, i);
      if (i == 0)
        snprintf (cl, sizeof cl, "%s (&%s); goto fail;",
                  CG_FREE (g), mv);
      else
        snprintf (cl, sizeof cl,
                  "%s (&%s); { size_t ts_j%u; for (ts_j%u = 0; ts_j%u < %zu; ts_j%u++) %s (&%s[ts_j%u]); } goto fail;",
                  CG_FREE (g), mv, m, m, m, i, m, CG_FREE (g), av, m);
      ts_cg_arg (g, &n->args[i], slot, n->line, cl);
    }
  ts_cg_fmt (g, "%s = %s (%s, &%s, \"%s\", \"%s\", %s, %zu, &%s, %u, %s);\n",
             sv, CG_CALL (g), CG_VMN (g), mv, n->module, n->func,
             n->nargs ? av : "NULL", n->nargs, rv, n->line, CG_ERN (g));
  if (n->nargs)
    ts_cg_fmt (g, "%s (&%s); { size_t ts_k%u; for (ts_k%u = 0; ts_k%u < %zu; ts_k%u++) %s (&%s[ts_k%u]); }\n",
               CG_FREE (g), mv, m, m, m, n->nargs, m, CG_FREE (g), av, m);
  else
    ts_cg_fmt (g, "%s (&%s);\n", CG_FREE (g), mv);
  ts_cg_fmt (g, "if (%s != %s) { %s (&%s); goto fail; }\n%s = %s; %s.type = %s;\n}\n",
             sv, CG_OK (g), CG_FREE (g), rv, out, rv, rv,
             g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
}

static void
ts_cg_expr (ts_cg_t *g, ts_node_t *n, const char *out)
{
  switch (n->kind)
    {
    case N_VAR:
      ts_cg_fmt (g, "if (%s (%s, \"%s\", &%s, %u, %s) != %s) goto fail;\n",
                 CG_GET (g), CG_VMN (g), n->name, out, n->line,
                 CG_ERN (g), CG_OK (g));
      break;
    case N_LIT_INT:
      ts_cg_fmt (g, "%s.type = %s; %s.as.integer = %lld;\n", out,
                 g->dt_mode ? "DT_TERM_INT" : "TS_INT", out,
                 (long long) n->number);
      break;
    case N_LIT_STR:
      ts_cg_fmt (g, "if (%s (&%s, ", CG_MKSTR (g), out);
      ts_cg_cstr (g, n->string);
      if (g->dt_mode)
        ts_cg_fmt (g, ", %s) != %s) goto fail;\n", CG_ERN (g),
                   CG_OK (g));
      else
        ts_cg_fmt (g, ") != %s) { %s (%s, %s, ENOMEM, 0, \"out of memory\"); goto fail; }\n",
                   CG_OK (g), CG_ERRSET (g), CG_ERN (g), CG_NOMEM (g));
      break;
    case N_LIT_BOOL:
      ts_cg_fmt (g, "%s.type = %s; %s.as.boolean = %s;\n", out,
                 g->dt_mode ? "DT_TERM_BOOL" : "TS_BOOL", out,
                 n->boolean ? "true" : "false");
      break;
    case N_LIT_NIL:
      ts_cg_fmt (g, "%s.type = %s;\n", out,
                 g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
      break;
    case N_CALL:
      ts_cg_call (g, n, out);
      break;
    case N_OR:
      {
        unsigned m = ts_cg_temp (g);
        char lv[64];
        snprintf (lv, sizeof lv, "ts_l%u", m);
        ts_cg_fmt (g, "{\n%s %s; %s.type = %s;\n", ts_cg_V (g), lv, lv,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_expr (g, n->lhs, lv);
        ts_cg_fmt (g, "if (%s (&%s)) { %s = %s; %s.type = %s; }\nelse { %s (&%s); ",
                   CG_TRUTHY (g), lv, out, lv, lv,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL", CG_FREE (g),
                   lv);
        ts_cg_expr (g, n->rhs, out);
        ts_cg_str (g, "}\n}\n");
      }
      break;
    default:
      ts_cg_fmt (g, "%s (%s, %s, 0, %u, \"not an expression\"); goto fail;\n",
                 CG_ERRSET (g), CG_ERN (g), CG_PARSE (g), n->line);
      break;
    }
}

static void
ts_cg_stmt (ts_cg_t *g, ts_node_t *s)
{
  switch (s->kind)
    {
    case N_CONST:
    case N_ASSIGN:
      {
        unsigned m = ts_cg_temp (g);
        bool is_const = s->kind == N_CONST;
        ts_cg_fmt (g, "{\n%s ts_e%u; ts_e%u.type = %s;\n", ts_cg_V (g),
                   m, m, g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_e%u", m);
          ts_cg_expr (g, s->rhs, slot);
        }
        ts_cg_fmt (g, "if (%s (%s, \"%s\", &ts_e%u, %s, %u, %s) != %s) { %s (&ts_e%u); goto fail; }\n",
                   CG_SET (g), CG_VMN (g), s->name, m,
                   is_const ? "true" : "false", s->line, CG_ERN (g),
                   CG_OK (g), CG_FREE (g), m);
        ts_cg_str (g, "}\n");
      }
      break;
    case N_EXPR_STMT:
      {
        unsigned m = ts_cg_temp (g);
        ts_cg_fmt (g, "{\n%s ts_e%u; ts_e%u.type = %s;\n", ts_cg_V (g),
                   m, m, g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_e%u", m);
          ts_cg_expr (g, s->rhs, slot);
        }
        ts_cg_fmt (g, "%s (&ts_e%u);\n}\n", CG_FREE (g), m);
      }
      break;
    case N_WHILE:
      {
        unsigned m = ts_cg_temp (g);
        ts_cg_fmt (g, "for (;;) {\n%s ts_c%u; ts_c%u.type = %s;\n",
                   ts_cg_V (g), m, m,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL");
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_c%u", m);
          ts_cg_expr (g, s->cond, slot);
        }
        ts_cg_fmt (g, "{ bool ts_g%u = %s (&ts_c%u) ? true : false; %s (&ts_c%u); if (!ts_g%u) break; }\n",
                   m, CG_TRUTHY (g), m, CG_FREE (g), m, m);
        ts_cg_list (g, s->first);
        ts_cg_str (g, "}\n");
      }
      break;
    case N_IF:
      {
        unsigned m = ts_cg_temp (g);
        ts_cg_fmt (g, "{\n%s ts_c%u; ts_c%u.type = %s;\nbool ts_g%u;\n",
                   ts_cg_V (g), m, m,
                   g->dt_mode ? "DT_TERM_NIL" : "TS_NIL", m);
        ts_cg_fmt (g, "if (%s (%s, %s) != %s) goto fail;\n", CG_STEP (g),
                   CG_VMN (g), CG_ERN (g), CG_OK (g));
        {
          char slot[64];
          snprintf (slot, sizeof slot, "ts_c%u", m);
          ts_cg_expr (g, s->cond, slot);
        }
        ts_cg_fmt (g, "ts_g%u = %s (&ts_c%u) ? true : false; %s (&ts_c%u);\nif (ts_g%u) {\n",
                   m, CG_TRUTHY (g), m, CG_FREE (g), m, m);
        ts_cg_list (g, s->first);
        if (s->otherwise)
          {
            ts_cg_str (g, "} else {\n");
            ts_cg_list (g, s->otherwise);
            ts_cg_str (g, "}\n");
          }
        else
          ts_cg_str (g, "}\n");
        ts_cg_str (g, "}\n");
      }
      break;
    default:
      ts_cg_fmt (g, "%s (%s, %s, 0, %u, \"bad statement\"); goto fail;\n",
                 CG_ERRSET (g), CG_ERN (g), CG_PARSE (g), s->line);
      break;
    }
}

static void
ts_cg_list (ts_cg_t *g, ts_node_t *list)
{
  ts_node_t *s;
  for (s = list; s && !g->failed; s = s->next)
    ts_cg_stmt (g, s);
}

/* Emit the whole unit.  `vm` supplies compile-time search paths. */
static const char *
ts_cg_emit (TS_VM *vm, ts_node_t *prog, const char *unit_name, bool dt_mode,
            TS_Error *error)
{
  ts_cg_t g;
  char ident[128];
  size_t i;
  struct ts_cg_paths paths;
  char *stack[64];
  memset (&g, 0, sizeof g);
  g.dt_mode = dt_mode;
  memset (&paths, 0, sizeof paths);
  for (i = 0; i < vm->nspaths; i++)
    ts_cg_paths_add (&paths, vm->spaths[i]);
  memset (stack, 0, sizeof stack);
  ts_cg_inline_chain (&prog->first, &paths, stack, 0, 0);
  ts_cg_paths_free (&paths);
  if (!unit_name || !*unit_name)
    unit_name = "termscript_unit";
  for (i = 0; i + 1 < sizeof ident && unit_name[i]; i++)
    ident[i] = isalnum ((unsigned char) unit_name[i]) ? unit_name[i] : '_';
  ident[i] = '\0';
  if (dt_mode)
    ts_cg_str (&g,
               "/* Generated by the DomTERM Termscript C backend (AOT).\n"
               " * Termscript statements were compiled to C; Termscript-side\n"
               " * libraries (`.tsc` companions) were inlined at compile time.\n"
               " * Native modules (std.*, custom) link statically and resolve\n"
               " * through the VM registry at run time.\n"
               " * Compile: cc -o UNIT UNIT.c -ltermscript -ltermscript_standalone -ldomterm\n"
               " * Static:  cc -static -o UNIT UNIT.c -ltermscript -ltermscript_standalone -ldomterm\n"
               " */\n"
               "#include <stdio.h>\n"
               "#include <stdlib.h>\n"
               "#include <string.h>\n"
               "#include <stdbool.h>\n"
               "#include <stddef.h>\n"
               "#include <stdint.h>\n"
               "#include <errno.h>\n"
               "#include <domterm/domterm.h>\n\n");
  else
    ts_cg_str (&g,
               "/* Generated by the Termscript C backend (AOT).\n"
               " * Termscript statements were compiled to C; Termscript-side\n"
               " * libraries (`.tsc` companions) were inlined at compile time.\n"
               " * Native modules link statically and resolve through the VM\n"
               " * registry at run time: weak references call\n"
               " * ts_stdlib_register_all/search_path when linked in, so\n"
               " * G-only units link libtermscript_standalone alone while\n"
               " * std.* users add the stdlib (force archive extraction with\n"
               " * -u, since weak references alone do not pull it in):\n"
               " * Compile: cc -o UNIT UNIT.c -ltermscript_standalone -lpthread\n"
               " *   (+ -u ts_stdlib_register_all -u ts_stdlib_search_path\n"
               " *      -ltermscript_stdlib for std.* native modules)\n"
               " * Static:  cc -static -o UNIT UNIT.c ... (same libraries)\n"
               " */\n"
               "#include <stdio.h>\n"
               "#include <stdlib.h>\n"
               "#include <string.h>\n"
               "#include <stdbool.h>\n"
               "#include <stddef.h>\n"
               "#include <stdint.h>\n"
               "#include <errno.h>\n"
               "#include <termscript/termscript.h>\n\n"
               "#if defined(__GNUC__) || defined(__clang__)\n"
               "__attribute__((weak)) TS_Status ts_stdlib_register_all (TS_VM *, TS_Error *);\n"
               "__attribute__((weak)) TS_Status ts_stdlib_search_path (TS_VM *, TS_Error *);\n"
               "#endif\n\n");
  if (dt_mode)
    ts_cg_fmt (&g, "static DT_Status\ntsc_run_%s (DT_TermVM *dt_vm, DT_Error *dt_err)\n{\n(void) dt_vm; (void) dt_err;\n",
               ident);
  else
    ts_cg_fmt (&g, "static TS_Status\ntsc_run_%s (TS_VM *ts_vm, TS_Error *ts_err)\n{\n(void) ts_vm; (void) ts_err;\n",
               ident);
  ts_cg_list (&g, prog->first);
  /* Keeps `fail` referenced even for empty programs (avoids an
   * unused-label warning under -Wall). */
  ts_cg_str (&g, "if (0) goto fail;\n");
  ts_cg_fmt (&g, "return %s;\nfail:\nreturn %s->code != %s ? %s->code : %s;\n}\n\n",
             CG_OK (&g), CG_ERN (&g), CG_OK (&g), CG_ERN (&g),
             CG_NOMEM (&g));
  ts_cg_str (&g, "int\nmain (int argc, char **argv)\n{\n");
  if (dt_mode)
    {
      ts_cg_fmt (&g, "DT_TermVM *vm_%s;\nDT_Error err_%s;\n(void) argc; (void) argv;\n",
                 ident, ident);
      ts_cg_fmt (&g, "dt_error_clear (&err_%s);\nvm_%s = dt_termscript_create (&err_%s);\n",
                 ident, ident, ident);
      ts_cg_fmt (&g, "if (!vm_%s) { fprintf (stderr, \"termscript: %%s\\n\", err_%s.message); return 1; }\n",
                 ident, ident);
      ts_cg_fmt (&g, "if (tsc_run_%s (vm_%s, &err_%s) != DT_OK) {\n"
                     "fprintf (stderr, \"termscript: %%s\\n\", err_%s.message);\n"
                     "dt_termscript_free (vm_%s);\nreturn 1;\n}\n",
                 ident, ident, ident, ident, ident);
      ts_cg_fmt (&g, "fputs (dt_rt_output (vm_%s) ? dt_rt_output (vm_%s) : \"\", stdout);\n",
                 ident, ident);
      ts_cg_fmt (&g, "dt_termscript_free (vm_%s);\nreturn 0;\n}\n",
                 ident);
    }
  else
    {
      ts_cg_fmt (&g, "TS_VM *vm_%s;\nTS_Error err_%s;\n(void) argc; (void) argv;\n",
                 ident, ident);
      ts_cg_fmt (&g, "ts_error_clear (&err_%s);\nvm_%s = ts_vm_create (&err_%s);\n",
                 ident, ident, ident);
      ts_cg_fmt (&g, "if (!vm_%s) { fprintf (stderr, \"termscript: %%s\\n\", err_%s.message); return 1; }\n",
                 ident, ident);
      ts_cg_str (&g, "#if defined(__GNUC__) || defined(__clang__)\n");
      ts_cg_fmt (&g, "if (ts_stdlib_register_all) { if (ts_stdlib_register_all (vm_%s, &err_%s) != TS_OK) "
                     "{ fprintf (stderr, \"termscript: %%s\\n\", err_%s.message); ts_vm_free (vm_%s); return 1; } }\n",
                 ident, ident, ident, ident);
      ts_cg_fmt (&g, "if (ts_stdlib_search_path) ts_stdlib_search_path (vm_%s, &err_%s);\n",
                 ident, ident);
      ts_cg_str (&g, "#endif\n");
      ts_cg_fmt (&g, "if (tsc_run_%s (vm_%s, &err_%s) != TS_OK) {\n"
                     "fprintf (stderr, \"termscript: %%s\\n\", err_%s.message);\n"
                     "ts_vm_free (vm_%s);\nreturn 1;\n}\n",
                 ident, ident, ident, ident, ident);
      ts_cg_fmt (&g, "fputs (ts_rt_output (vm_%s) ? ts_rt_output (vm_%s) : \"\", stdout);\n",
                 ident, ident);
      ts_cg_fmt (&g, "ts_vm_free (vm_%s);\nreturn 0;\n}\n",
                 ident);
    }
  if (g.failed || !g.buf)
    {
      free (g.buf);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return NULL;
    }
  free (vm->compiled);
  vm->compiled = g.buf;
  return vm->compiled;
}

const char *
ts_vm_compile_to_c (TS_VM *vm, const char *source,
                            const char *unit_name, TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Error derr;
  const char *r;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return NULL;
    }
  /* Parse (no execution); syntax errors fail the compile. */
  ts_error_clear (&derr);
  if (ts_parse_program (source, &prog, &derr) != TS_OK)
    {
      ts_error_set (error, derr.code != TS_OK ? derr.code : TS_ERR_PARSE,
                  derr.system_errno, derr.offset,
                  "termscript parse error: %s",
                  derr.message[0] ? derr.message : "invalid syntax");
      return NULL;
    }
  r = ts_cg_emit (vm, prog, unit_name, false, error);
  ts_node_free (prog);
  return r;
}

/* DomTERM-flavored compile on a standalone VM (shared emitter).  Used
 * by dt_termscript_compile_to_c via the inner TS VM. */
const char *
ts_vm_compile_to_c_dt (TS_VM *vm, const char *source,
                       const char *unit_name, TS_Error *error)
{
  ts_node_t *prog = NULL;
  TS_Error derr;
  const char *r;
  if (!vm || !source)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return NULL;
    }
  ts_error_clear (&derr);
  if (ts_parse_program (source, &prog, &derr) != TS_OK)
    {
      ts_error_set (error, derr.code != TS_OK ? derr.code : TS_ERR_PARSE,
                  derr.system_errno, derr.offset,
                  "termscript parse error: %s",
                  derr.message[0] ? derr.message : "invalid syntax");
      return NULL;
    }
  r = ts_cg_emit (vm, prog, unit_name, true, error);
  ts_node_free (prog);
  return r;
}

const char *
ts_version_string (void)
{
  static char buf[32];
  snprintf (buf, sizeof buf, "%d.%d.%d",
            TS_VERSION_MAJOR, TS_VERSION_MINOR, TS_VERSION_PATCH);
  return buf;
}
