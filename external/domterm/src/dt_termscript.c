/* dt_termscript.c -- DomTERM Termscript bindings (thin adapter).
 *
 * This file implements the DT_Term* API from domterm.h on top of the
 * standalone Termscript runtime (termscript/termscript.h,
 * libtermscript_standalone).  It owns no language machinery itself:
 *
 * - DT_TermVM wraps a TS_VM; per-VM state (variables, output) lives there.
 * - DT_TermModule tables are wrapped as TS_Module tables with a
 *   trampoline converting values/errors across the ABI boundary.
 * - The global DT registry is fanned out into every live DT VM, so a
 *   module registered after VM creation is visible immediately (the
 *   historical contract exercised by the test suite).
 * - "std.terminfo" is implemented here (it needs DT_TIProf) as a native
 *   TS_Module; profiles travel in TS handles carrying an identity tag.
 * - The C transpiler emits DT-based code (link libtermscript) after
 *   validating with ts_check_syntax.
 *
 * Status codes are shared numerically with TS_Status (verified with
 * _Static_assert below); only the type names differ.
 */
#include "domterm.h"
#include "termscript/termscript.h"
#include "dt_internal.h"
/* After dt_internal.h: the stdlib headers pull system headers that
 * would otherwise trip its _POSIX_C_SOURCE definition. */
#include "termscript/stdlib/ts_stdlib.h"

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DT_TermVM
{
  TS_VM *tsvm;
  char *compiled; /* Last transpiled unit (borrowed by callers). */
};

_Static_assert ((int) TS_OK == (int) DT_OK, "status mirror");
_Static_assert ((int) TS_ERR_INVAL == (int) DT_ERR_INVALID_ARGUMENT,
                "status mirror");
_Static_assert ((int) TS_ERR_NOMEM == (int) DT_ERR_NO_MEMORY,
                "status mirror");
_Static_assert ((int) TS_ERR_SYSTEM == (int) DT_ERR_SYSTEM,
                "status mirror");
_Static_assert ((int) TS_ERR_NOTFOUND == (int) DT_ERR_NOT_FOUND,
                "status mirror");
_Static_assert ((int) TS_ERR_PARSE == (int) DT_ERR_PARSE, "status mirror");
_Static_assert ((int) TS_ERR_CANCELLED == (int) DT_ERR_CANCELLED,
                "status mirror");
_Static_assert ((int) TS_ERR_LIMIT == (int) DT_ERR_LIMIT, "status mirror");
_Static_assert (sizeof ((TS_Error *) 0)->message ==
                sizeof ((DT_Error *) 0)->message, "error message size");

/* ---------------- value / error conversion ---------------- */

static void
dt_convert_error (DT_Error *dst, const TS_Error *src)
{
  if (!dst || !src)
    return;
  dst->code = (DT_Status) src->code;
  dst->system_errno = src->system_errno;
  dst->offset = src->offset;
  memcpy (dst->message, src->message, sizeof dst->message);
}

/* Reverse direction: DomTERM error into a TS_Error (for trampolines). */
static void
ts_convert_error (TS_Error *dst, const DT_Error *src)
{
  if (!dst || !src)
    return;
  dst->code = (TS_Status) src->code;
  dst->system_errno = src->system_errno;
  dst->offset = src->offset;
  memcpy (dst->message, src->message, sizeof dst->message);
}

static void
dt_release_value (DT_TermValue *v)
{
  if (!v)
    return;
  if (v->type == DT_TERM_STRING)
    free (v->as.string);
  else if (v->type == DT_TERM_HANDLE && v->as.handle)
    {
      TS_Value tmp;
      tmp.type = TS_HANDLE;
      tmp.as.handle = (TS_Handle *) v->as.handle;
      ts_value_free (&tmp);
    }
  v->type = DT_TERM_NIL;
  v->as.handle = NULL;
}

/* TS -> DT (retains shared handles). */
static TS_Status
dt_from_ts (DT_TermValue *d, const TS_Value *s)
{
  TS_Value tmp;
  switch (s->type)
    {
    case TS_NIL:
      d->type = DT_TERM_NIL;
      return TS_OK;
    case TS_BOOL:
      d->type = DT_TERM_BOOL;
      d->as.boolean = s->as.boolean;
      return TS_OK;
    case TS_INT:
      d->type = DT_TERM_INT;
      d->as.integer = s->as.integer;
      return TS_OK;
    case TS_STRING:
      d->type = DT_TERM_STRING;
      d->as.string = s->as.string ? strdup (s->as.string) : NULL;
      if (s->as.string && !d->as.string)
        return TS_ERR_NOMEM;
      return TS_OK;
    case TS_HANDLE:
      tmp.type = TS_HANDLE;
      tmp.as.handle = s->as.handle;
      if (ts_value_copy (&tmp, s) != TS_OK)
        return TS_ERR_NOMEM;
      /* Ownership of the retained reference moves to d. */
      d->type = DT_TERM_HANDLE;
      d->as.handle = tmp.as.handle;
      return TS_OK;
    }
  d->type = DT_TERM_NIL;
  return TS_ERR_INVAL;
}

/* DT -> TS (retains shared handles). */
static TS_Status
ts_from_dt (TS_Value *d, const DT_TermValue *s)
{
  TS_Value tmp;
  switch (s->type)
    {
    case DT_TERM_NIL:
      d->type = TS_NIL;
      return TS_OK;
    case DT_TERM_BOOL:
      d->type = TS_BOOL;
      d->as.boolean = s->as.boolean;
      return TS_OK;
    case DT_TERM_INT:
      d->type = TS_INT;
      d->as.integer = s->as.integer;
      return TS_OK;
    case DT_TERM_STRING:
      d->type = TS_STRING;
      d->as.string = s->as.string ? strdup (s->as.string) : NULL;
      if (s->as.string && !d->as.string)
        return TS_ERR_NOMEM;
      return TS_OK;
    case DT_TERM_HANDLE:
      tmp.type = TS_HANDLE;
      tmp.as.handle = (TS_Handle *) s->as.handle;
      if (ts_value_copy (d, &tmp) != TS_OK)
        return TS_ERR_NOMEM;
      return TS_OK;
    }
  d->type = TS_NIL;
  return TS_ERR_INVAL;
}

/* ---------------- module trampoline ---------------- */

/* Wraps one DT_TermFuncDef as a TS_FuncDef (userdata points back at the
 * DT def, borrowed, static lifetime like the module table itself). */
static TS_Status
dt_trampoline (TS_VM *tsvm, void *userdata, const TS_Value *argv,
               size_t argc, TS_Value *ret, TS_Error *tserr)
{
  const DT_TermFuncDef *def = (const DT_TermFuncDef *) userdata;
  DT_TermVM *dtvm = tsvm ? ts_vm_get_userdata (tsvm) : NULL;
  DT_TermValue *dargv = NULL;
  DT_TermValue dret;
  DT_Error dterr;
  DT_Status st;
  size_t i;
  if (!def || !def->func || !dtvm)
    {
      ts_error_set (tserr, TS_ERR_INVAL, 0, 0, "bad trampoline");
      return TS_ERR_INVAL;
    }
  dargv = calloc (argc ? argc : 1, sizeof *dargv);
  if (!dargv)
    {
      ts_error_set (tserr, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  for (i = 0; i < argc; i++)
    if (dt_from_ts (&dargv[i], &argv[i]) != TS_OK)
      {
        size_t k;
        for (k = 0; k < i; k++)
          dt_release_value (&dargv[k]);
        free (dargv);
        ts_error_set (tserr, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
        return TS_ERR_NOMEM;
      }
  dret.type = DT_TERM_NIL;
  dt_error_clear (&dterr);
  st = def->func (dtvm, dargv, argc, &dret, &dterr);
  for (i = 0; i < argc; i++)
    dt_release_value (&dargv[i]);
  free (dargv);
  if (ts_from_dt (ret, &dret) != TS_OK)
    {
      dt_release_value (&dret);
      ts_error_set (tserr, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  dt_release_value (&dret);
  ts_convert_error (tserr, &dterr);
  return (TS_Status) st;
}

/* ---------------- global DT registry + live VMs ---------------- */

#define DT_TS_MAX_MODULES 64

static const DT_TermModule *dt_globals[DT_TS_MAX_MODULES];
static TS_Module *dt_wraps[DT_TS_MAX_MODULES];
static size_t dt_nglobals = 0;
static DT_TermVM **dt_live = NULL;
static size_t dt_nlive = 0, dt_lcap = 0;
static pthread_mutex_t dt_ts_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Names owned by the standalone core / this adapter / the stdlib;
 * user modules under these names are duplicates.  The whole "std."
 * prefix belongs to the standard library (native halves registered
 * below, companions resolved through the stdlib search path). */
static bool
dt_name_reserved (const char *name)
{
  return strcmp (name, "G") == 0 || strcmp (name, "std.terminfo") == 0 ||
         strncmp (name, "std.", 4) == 0;
}

/* Caller holds dt_ts_mutex. Wraps a DT module as a TS module; the
 * TS_FuncDef array is heap-owned here and lives as long as the
 * registration (process lifetime for globals). */
static TS_Module *
dt_wrap_module (const DT_TermModule *mod)
{
  size_t n = 0;
  TS_Module *wrap;
  TS_FuncDef *funcs;
  while (mod->funcs[n].name)
    n++;
  wrap = malloc (sizeof *wrap);
  funcs = malloc ((n + 1) * sizeof *funcs);
  if (!wrap || !funcs)
    {
      free (wrap);
      free (funcs);
      return NULL;
    }
  for (n = 0; mod->funcs[n].name; n++)
    {
      funcs[n].name = mod->funcs[n].name;
      funcs[n].func = dt_trampoline;
      funcs[n].userdata = (void *) &mod->funcs[n];
    }
  funcs[n].name = NULL;
  funcs[n].func = NULL;
  funcs[n].userdata = NULL;
  wrap->name = mod->name;
  wrap->funcs = funcs;
  return wrap;
}

DT_Status
dt_termscript_register_module (const DT_TermModule *module, DT_Error *error)
{
  size_t i;
  TS_Module *wrap;
  DT_Status st = DT_OK;
  if (!module || !module->name || !module->funcs)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad module");
      return DT_ERR_INVALID_ARGUMENT;
    }
  for (i = 0; module->funcs[i].name; i++)
    if (!module->funcs[i].func)
      {
        dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                    "module func '%s' is NULL", module->funcs[i].name);
        return DT_ERR_INVALID_ARGUMENT;
      }
  wrap = dt_wrap_module (module);
  if (!wrap)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  pthread_mutex_lock (&dt_ts_mutex);
  for (i = 0; i < dt_nglobals; i++)
    if (strcmp (dt_globals[i]->name, module->name) == 0 ||
        dt_name_reserved (module->name))
      {
        pthread_mutex_unlock (&dt_ts_mutex);
        free ((void *) wrap->funcs);
        free (wrap);
        dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                    "module '%s' already registered", module->name);
        return DT_ERR_INVALID_ARGUMENT;
      }
  if (dt_nglobals >= DT_TS_MAX_MODULES)
    {
      pthread_mutex_unlock (&dt_ts_mutex);
      free ((void *) wrap->funcs);
      free (wrap);
      dt_err_set (error, DT_ERR_LIMIT, 0, 0, "module registry full");
      return DT_ERR_LIMIT;
    }
  dt_globals[dt_nglobals] = module;
  dt_wraps[dt_nglobals] = wrap;
  dt_nglobals++;
  /* Fan the single shared wrapper out to live VMs (best-effort;
   * per-VM tables cannot collide here because the global check above
   * passed).  Wrappers live as long as the process. */
  for (i = 0; i < dt_nlive; i++)
    {
      TS_Error tserr;
      ts_error_clear (&tserr);
      if (ts_vm_register_module (dt_live[i]->tsvm, wrap, &tserr) != TS_OK)
        st = (DT_Status) tserr.code;
    }
  pthread_mutex_unlock (&dt_ts_mutex);
  return st;
}

/* ---------------- std.terminfo (DomTERM binding) ---------------- */

/* Identity token for terminfo-profile handles. */
static const char ti_profile_tag_id = 0;

typedef struct
{
  DT_TIProf *prof;
} ti_prof_t;

static void
ti_prof_free (void *p)
{
  ti_prof_t *w = p;
  if (!w)
    return;
  dt_tiprof_free (w->prof);
  free (w);
}

static TS_Status
ti_check_profile (const TS_Value *v, DT_TIProf **prof, TS_Error *error)
{
  ti_prof_t *w = ts_value_handle (v, &ti_profile_tag_id);
  if (!w || !w->prof)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "terminfo profile handle required");
      return TS_ERR_INVAL;
    }
  *prof = w->prof;
  return TS_OK;
}

static TS_Status
ti_load (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  DT_TIDB *db;
  DT_TIProf *prof = NULL;
  ti_prof_t *w;
  (void) vm;
  (void) ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "terminal name must be a string");
      return TS_ERR_INVAL;
    }
  {
    DT_Error dderr;
    dt_error_clear (&dderr);
    db = dt_tidb_open_default (&dderr);
    if (!db)
      {
        ret->type = TS_NIL;
        return TS_OK;
      }
    prof = dt_tiprof_load (db, argv[0].as.string, &dderr);
    dt_tidb_close (db);
  }
  if (!prof)
    {
      ret->type = TS_NIL;
      return TS_OK;
    }
  w = malloc (sizeof *w);
  if (!w)
    {
      dt_tiprof_free (prof);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  w->prof = prof;
  {
    char desc[96];
    snprintf (desc, sizeof desc, "<terminfo-profile %s>",
              dt_tiprof_name (prof) ? dt_tiprof_name (prof) : "?");
    if (ts_value_make_handle (ret, w, ti_prof_free, desc,
                              &ti_profile_tag_id) != TS_OK)
      {
        ti_prof_free (w);
        ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
        return TS_ERR_NOMEM;
      }
  }
  return TS_OK;
}

static TS_Status
ti_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  DT_TIProf *prof;
  DT_TIValue v;
  (void) vm;
  (void) ud;
  if (argc != 2)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "ti:get wants 2 args");
      return TS_ERR_INVAL;
    }
  if (ti_check_profile (&argv[0], &prof, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[1].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "capability name must be a string");
      return TS_ERR_INVAL;
    }
  if (!dt_tiprof_get (prof, argv[1].as.string, &v))
    {
      ret->type = TS_NIL;
      return TS_OK;
    }
  if (v.type == DT_TI_BOOL)
    {
      ret->type = TS_BOOL;
      ret->as.boolean = v.as.boolean;
    }
  else if (v.type == DT_TI_NUMBER)
    {
      ret->type = TS_INT;
      ret->as.integer = v.as.number;
    }
  else
    {
      ret->type = TS_STRING;
      ret->as.string = strdup (v.as.string);
      if (!ret->as.string)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  return TS_OK;
}

static TS_Status
ti_expand (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  DT_TIProf *prof;
  DT_TIParam params[9];
  size_t i, n;
  char out[DT_TI_MAX_EXPAND + 1];
  size_t need;
  (void) vm;
  (void) ud;
  if (argc < 2 || argc > 11)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "ti:expand wants 2..11 args");
      return TS_ERR_INVAL;
    }
  if (ti_check_profile (&argv[0], &prof, error) != TS_OK)
    return TS_ERR_INVAL;
  if (argv[1].type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "capability name must be a string");
      return TS_ERR_INVAL;
    }
  n = argc - 2 < 9 ? argc - 2 : 9;
  for (i = 0; i < n; i++)
    {
      if (argv[2 + i].type == TS_INT)
        {
          params[i].is_string = false;
          params[i].number = (int32_t) argv[2 + i].as.integer;
          params[i].string = NULL;
        }
      else if (argv[2 + i].type == TS_STRING)
        {
          params[i].is_string = true;
          params[i].number = 0;
          params[i].string = argv[2 + i].as.string;
        }
      else if (argv[2 + i].type == TS_BOOL)
        {
          params[i].is_string = false;
          params[i].number = argv[2 + i].as.boolean ? 1 : 0;
          params[i].string = NULL;
        }
      else
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0,
                        "bad expand parameter");
          return TS_ERR_INVAL;
        }
    }
  {
    DT_Error derr;
    dt_error_clear (&derr);
    need = dt_tiprof_expand_params (prof, argv[1].as.string, params, n,
                                    out, sizeof out, &derr);
    if (need == SIZE_MAX)
      {
        ret->type = TS_NIL; /* Expansion failure is nil (see `or`). */
        return TS_OK;
      }
  }
  ret->type = TS_STRING;
  ret->as.string = strdup (out);
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static void
ti_emit_escaped (char **buf, size_t *len, size_t *cap, const char *s)
{
  /* Minimal terminfo-source escaping for capability strings. */
  for (; *s; s++)
    {
      unsigned char c = (unsigned char) *s;
      char tmp[8];
      if (c == 0x1B)
        dt_buf_put (buf, len, cap, "\\E", 2);
      else if (c < 0x20)
        {
          tmp[0] = '^';
          tmp[1] = (char) (c + '@');
          dt_buf_put (buf, len, cap, tmp, 2);
        }
      else if (c == 0x7F)
        dt_buf_put (buf, len, cap, "^?", 2);
      else if (c == ',' || c == '\\' || c == ':')
        {
          tmp[0] = '\\';
          tmp[1] = (char) c;
          dt_buf_put (buf, len, cap, tmp, 2);
        }
      else
        dt_buf_put (buf, len, cap, (char *) &c, 1);
    }
}

static TS_Status
ti_compile_to_ti (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
                  TS_Value *ret, TS_Error *error)
{
  DT_TIProf *prof;
  char *buf = NULL;
  size_t len = 0, cap = 0;
  size_t total, i;
  DT_TICapInfo info;
  char tmp[128];
  bool first;
  (void) vm;
  (void) ud;
  if (ts_check_argc (vm, argv, argc, 1, error) != TS_OK)
    return TS_ERR_INVAL;
  if (ti_check_profile (&argv[0], &prof, error) != TS_OK)
    return TS_ERR_INVAL;
  dt_buf_str (&buf, &len, &cap, dt_tiprof_name (prof));
  for (i = 0; i < dt_tiprof_alias_count (prof); i++)
    {
      dt_buf_str (&buf, &len, &cap, "|");
      dt_buf_str (&buf, &len, &cap, dt_tiprof_alias (prof, i));
    }
  dt_buf_str (&buf, &len, &cap, ",\n");
  total = dt_tiprof_cap_count ();
  first = true;
  for (i = 0; i < total; i++)
    {
      DT_TIValue v;
      if (!dt_tiprof_cap_by_index (i, &info))
        continue;
      if (!dt_tiprof_get (prof, info.name, &v))
        continue;
      if (!first)
        dt_buf_str (&buf, &len, &cap, ", ");
      first = false;
      if (v.type == DT_TI_BOOL)
        dt_buf_str (&buf, &len, &cap, info.name);
      else if (v.type == DT_TI_NUMBER)
        {
          snprintf (tmp, sizeof tmp, "%s#%d", info.name, (int) v.as.number);
          dt_buf_str (&buf, &len, &cap, tmp);
        }
      else
        {
          snprintf (tmp, sizeof tmp, "%s=", info.name);
          dt_buf_str (&buf, &len, &cap, tmp);
          ti_emit_escaped (&buf, &len, &cap, v.as.string);
        }
      if ((i % 4) == 3)
        dt_buf_str (&buf, &len, &cap, ",\n\t");
    }
  dt_buf_str (&buf, &len, &cap, ",\n");
  if (!buf)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ret->type = TS_STRING;
  ret->as.string = buf;
  return TS_OK;
}

static const TS_FuncDef ti_funcs[] = {
  { "load", ti_load, NULL }, { "get", ti_get, NULL },
  { "expand", ti_expand, NULL },
  { "compile_to_ti", ti_compile_to_ti, NULL }, { NULL, NULL, NULL }
};

static const TS_Module ti_module = { "std.terminfo", ti_funcs };

/* ---------------- DT VM lifetime ---------------- */

static DT_Status
dt_live_add (DT_TermVM *vm)
{
  DT_TermVM **nl;
  if (dt_nlive == dt_lcap)
    {
      size_t nc = dt_lcap ? dt_lcap * 2 : 8;
      nl = realloc (dt_live, nc * sizeof *nl);
      if (!nl)
        return DT_ERR_NO_MEMORY;
      dt_live = nl;
      dt_lcap = nc;
    }
  dt_live[dt_nlive++] = vm;
  return DT_OK;
}

static void
dt_live_remove (DT_TermVM *vm)
{
  size_t i;
  for (i = 0; i < dt_nlive; i++)
    if (dt_live[i] == vm)
      {
        dt_live[i] = dt_live[--dt_nlive];
        return;
      }
}

DT_TermVM *
dt_termscript_create (DT_Error *error)
{
  DT_TermVM *vm = calloc (1, sizeof *vm);
  TS_Error tserr;
  size_t i;
  if (!vm)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  ts_error_clear (&tserr);
  vm->tsvm = ts_vm_create (&tserr);
  if (!vm->tsvm)
    {
      free (vm);
      dt_err_set (error, (DT_Status) tserr.code, tserr.system_errno, 0,
                  "%s", tserr.message[0] ? tserr.message : "out of memory");
      return NULL;
    }
  ts_vm_set_userdata (vm->tsvm, vm);
  if (ts_vm_register_module (vm->tsvm, &ti_module, &tserr) != TS_OK)
    {
      ts_vm_free (vm->tsvm);
      free (vm);
      dt_err_set (error, (DT_Status) tserr.code, tserr.system_errno, 0,
                  "%s", tserr.message[0] ? tserr.message : "init failed");
      return NULL;
    }
  /* The hybrid standard library: native halves register here, .tsc
   * companions resolve through the default search path (plus
   * TERMSCRIPT_PATH at import time). */
  if (ts_stdlib_register_all (vm->tsvm, &tserr) != TS_OK ||
      ts_stdlib_search_path (vm->tsvm, &tserr) != TS_OK)
    {
      ts_vm_free (vm->tsvm);
      free (vm);
      dt_err_set (error, (DT_Status) tserr.code, tserr.system_errno, 0,
                  "%s", tserr.message[0] ? tserr.message : "init failed");
      return NULL;
    }
  pthread_mutex_lock (&dt_ts_mutex);
  for (i = 0; i < dt_nglobals; i++)
    if (ts_vm_register_module (vm->tsvm, dt_wraps[i], &tserr) != TS_OK)
      {
        pthread_mutex_unlock (&dt_ts_mutex);
        ts_vm_free (vm->tsvm);
        free (vm);
        dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0,
                    "out of memory");
        return NULL;
      }
  if (dt_live_add (vm) != DT_OK)
    {
      pthread_mutex_unlock (&dt_ts_mutex);
      ts_vm_free (vm->tsvm);
      free (vm);
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  pthread_mutex_unlock (&dt_ts_mutex);
  return vm;
}

void
dt_termscript_free (DT_TermVM *vm)
{
  if (!vm)
    return;
  pthread_mutex_lock (&dt_ts_mutex);
  dt_live_remove (vm);
  pthread_mutex_unlock (&dt_ts_mutex);
  ts_vm_free (vm->tsvm);
  free (vm->compiled);
  free (vm);
}

TS_VM *
dt_termscript_inner_vm (DT_TermVM *vm)
{
  return vm ? vm->tsvm : NULL;
}

DT_Status
dt_termscript_run_string (DT_TermVM *vm, const char *source, char **output,
                          DT_Error *error)
{
  TS_Error tserr;
  TS_Status st;
  if (!vm || !source)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  ts_error_clear (&tserr);
  st = ts_vm_run_string (vm->tsvm, source, output, &tserr);
  dt_convert_error (error, &tserr);
  return (DT_Status) st;
}

DT_Status
dt_termscript_run_file (DT_TermVM *vm, const char *path, char **output,
                        DT_Error *error)
{
  TS_Error tserr;
  TS_Status st;
  if (!vm || !path)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  ts_error_clear (&tserr);
  st = ts_vm_run_file (vm->tsvm, path, output, &tserr);
  dt_convert_error (error, &tserr);
  return (DT_Status) st;
}

/* ---------------- compiled-code runtime (dt_rt_*) ----------------
 *
 * Thin DT_TermValue wrappers over the ts_rt_* helpers on the inner
 * TS VM.  Emitted C (see ts_cg_emit in ts_runtime.c) calls these, so
 * compiled units share exact interpreter semantics: variables live in
 * the VM scope, modules resolve through the registry, the step budget
 * is shared, and error offsets carry Termscript line numbers. */

DT_Status
dt_rt_call (DT_TermVM *vm, const DT_TermValue *mod, const char *modname,
            const char *func, const DT_TermValue *argv, size_t argc,
            DT_TermValue *ret, unsigned line, DT_Error *error)
{
  TS_Value tmod, tret, *targv = NULL;
  TS_Error tserr;
  TS_Status st;
  size_t i;
  if (!vm || !mod || !func || !ret)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, line, "bad call");
      return DT_ERR_INVALID_ARGUMENT;
    }
  tmod.type = TS_NIL;
  tret.type = TS_NIL;
  if (ts_from_dt (&tmod, mod) != TS_OK)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  if (argc)
    {
      targv = calloc (argc, sizeof *targv);
      if (!targv)
        {
          ts_value_free (&tmod);
          dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0,
                      "out of memory");
          return DT_ERR_NO_MEMORY;
        }
      for (i = 0; i < argc; i++)
        if (ts_from_dt (&targv[i], &argv[i]) != TS_OK)
          {
            size_t k;
            for (k = 0; k < i; k++)
              ts_value_free (&targv[k]);
            free (targv);
            ts_value_free (&tmod);
            dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0,
                        "out of memory");
            return DT_ERR_NO_MEMORY;
          }
    }
  ts_error_clear (&tserr);
  st = ts_rt_call (vm->tsvm, &tmod, modname, func, targv, argc, &tret,
                   line, &tserr);
  ts_value_free (&tmod);
  if (targv)
    {
      for (i = 0; i < argc; i++)
        ts_value_free (&targv[i]);
      free (targv);
    }
  if (st != TS_OK)
    {
      ts_value_free (&tret);
      dt_convert_error (error, &tserr);
      return (DT_Status) st;
    }
  if (dt_from_ts (ret, &tret) != TS_OK)
    {
      ts_value_free (&tret);
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  ts_value_free (&tret);
  return DT_OK;
}

DT_Status
dt_rt_get (DT_TermVM *vm, const char *name, DT_TermValue *out,
           unsigned line, DT_Error *error)
{
  TS_Value tmp;
  TS_Error tserr;
  TS_Status st;
  if (!vm || !name || !out)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, line,
                  "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  tmp.type = TS_NIL;
  ts_error_clear (&tserr);
  st = ts_rt_get (vm->tsvm, name, &tmp, line, &tserr);
  if (st != TS_OK)
    {
      dt_convert_error (error, &tserr);
      return (DT_Status) st;
    }
  if (dt_from_ts (out, &tmp) != TS_OK)
    {
      ts_value_free (&tmp);
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  ts_value_free (&tmp);
  return DT_OK;
}

DT_Status
dt_rt_set (DT_TermVM *vm, const char *name, DT_TermValue *val,
           bool is_const, unsigned line, DT_Error *error)
{
  TS_Value tmp;
  TS_Error tserr;
  TS_Status st;
  if (!vm || !name || !val)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, line,
                  "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  tmp.type = TS_NIL;
  if (ts_from_dt (&tmp, val) != TS_OK)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  ts_error_clear (&tserr);
  st = ts_rt_set (vm->tsvm, name, &tmp, is_const, line, &tserr);
  if (st != TS_OK)
    {
      ts_value_free (&tmp);
      dt_convert_error (error, &tserr);
      return (DT_Status) st;
    }
  /* Adopted: the VM owns the converted copy; release the caller copy. */
  dt_release_value (val);
  return DT_OK;
}

DT_Status
dt_rt_step (DT_TermVM *vm, DT_Error *error)
{
  TS_Error tserr;
  TS_Status st;
  if (!vm)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad vm");
      return DT_ERR_INVALID_ARGUMENT;
    }
  ts_error_clear (&tserr);
  st = ts_rt_step (vm->tsvm, &tserr);
  if (st != TS_OK)
    dt_convert_error (error, &tserr);
  return (DT_Status) st;
}

DT_Status
dt_rt_mkstring (DT_TermValue *val, const char *s, DT_Error *error)
{
  if (!val)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  val->type = DT_TERM_STRING;
  val->as.string = strdup (s ? s : "");
  if (!val->as.string)
    {
      val->type = DT_TERM_NIL;
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  return DT_OK;
}

bool
dt_rt_truthy (const DT_TermValue *val)
{
  if (!val)
    return false;
  switch (val->type)
    {
    case DT_TERM_NIL: return false;
    case DT_TERM_BOOL: return val->as.boolean;
    case DT_TERM_INT: return val->as.integer != 0;
    case DT_TERM_STRING:
      return val->as.string && val->as.string[0] != '\0';
    case DT_TERM_HANDLE: return true;
    }
  return false;
}

const char *
dt_rt_output (const DT_TermVM *vm)
{
  if (!vm)
    return "";
  return ts_rt_output (vm->tsvm);
}

/* ---------------- C backend (DT flavor, shared emitter) ---------------- */

const char *
dt_termscript_compile_to_c (DT_TermVM *vm, const char *source,
                            const char *unit_name, DT_Error *error)
{
  TS_Error tserr;
  const char *code;
  char *copy;
  if (!vm || !source)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return NULL;
    }
  /* The shared emitter (ts_runtime.c) parses, inlines Termscript-side
   * imports as compiled C, and emits DT-flavored code; only native
   * modules stay linked (statically) instead of embedded. */
  ts_error_clear (&tserr);
  code = ts_vm_compile_to_c_dt (vm->tsvm, source, unit_name, &tserr);
  if (!code)
    {
      dt_convert_error (error, &tserr);
      return NULL;
    }
  copy = strdup (code);
  if (!copy)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  free (vm->compiled);
  vm->compiled = copy;
  return vm->compiled;
}

void
dt_termvalue_free (DT_TermValue *value)
{
  dt_release_value (value);
}

DT_Status
dt_termvalue_to_string (const DT_TermValue *value, char **out,
                        DT_Error *error)
{
  TS_Value tmp;
  TS_Status st;
  char *s = NULL;
  if (!value || !out)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (ts_from_dt (&tmp, value) != TS_OK)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  {
    TS_Error tserr;
    ts_error_clear (&tserr);
    st = ts_value_to_string (&tmp, &s, &tserr);
    if (st == TS_OK)
      *out = s;
    else
      dt_convert_error (error, &tserr);
    ts_value_free (&tmp);
  }
  return (DT_Status) st;
}
