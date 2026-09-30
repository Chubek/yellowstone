/* theme.c -- std.theme: named palettes over role names.
 *
 * Three built-ins (default, solarized, mono); roles are fg/bg/accent/
 * error/warn/ok/info/muted.  A theme handle renders roles to SGR
 * sequences; theme.tsc aliases the default theme.
 */
#include "theme/theme.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const theme_roles[] = {
  "fg", "bg", "accent", "error", "warn", "ok", "info", "muted", NULL
};

/* SGR parameter strings per role (without ESC[ / m). */
typedef struct
{
  const char *name;
  const char *codes[8];
} theme_def_t;

static const theme_def_t theme_defs[] = {
  { "default",
    { "37", "40", "1;36", "1;31", "1;33", "1;32", "1;34", "2;37" } },
  { "solarized",
    { "36", "44", "1;33", "1;31", "33", "32", "34", "36" } },
  { "mono",
    { "37", "40", "1;37", "1;37", "37", "37", "37", "2;37" } },
  { NULL, { NULL } }
};

typedef struct
{
  const theme_def_t *def;
} theme_t;

static const char theme_tag_id = 0;

static theme_t *
theme_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &theme_tag_id, error, what);
}

static int
role_index (const char *role)
{
  size_t i;
  for (i = 0; theme_roles[i]; i++)
    if (strcmp (role, theme_roles[i]) == 0)
      return (int) i;
  return -1;
}

static TS_Status
t_list (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *sep = ",";
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 1, error, "list") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &sep, error, "list") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (i = 0; theme_defs[i].name; i++)
    {
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, theme_defs[i].name) != 0)
        goto oom;
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    goto oom;
  return TS_OK;
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
t_load (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *name;
  size_t i;
  theme_t *t;
  char desc[64];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "load") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &name, error, "load") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; theme_defs[i].name; i++)
    if (strcmp (name, theme_defs[i].name) == 0)
      {
        t = malloc (sizeof *t);
        if (!t)
          {
            ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
            return TS_ERR_NOMEM;
          }
        t->def = &theme_defs[i];
        snprintf (desc, sizeof desc, "<theme %s>", name);
        if (ts_value_make_handle (ret, t, free, desc,
                                  &theme_tag_id) != TS_OK)
          {
            free (t);
            ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
            return TS_ERR_NOMEM;
          }
        return TS_OK;
      }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
theme_role_seq (TS_VM *vm, const TS_Value *argv, size_t argc,
                TS_Value *ret, TS_Error *error, const char *fname,
                bool bg)
{
  theme_t *t;
  const char *role;
  int ri;
  char seq[64];
  (void) vm;
  (void) argc;
  t = theme_unwrap (&argv[0], error, fname);
  if (!t || ts_std_str (&argv[1], &role, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  ri = role_index (role);
  if (ri < 0)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: unknown role '%s'",
                    fname, role);
      return TS_ERR_INVAL;
    }
  /* Background roles shift 3x->4x when the code is a plain color. */
  if (bg && t->def->codes[ri][1] == '\0')
    snprintf (seq, sizeof seq, "\x1b[4%cm", t->def->codes[ri][0]);
  else
    snprintf (seq, sizeof seq, "\x1b[%sm", t->def->codes[ri]);
  return ts_std_ret_str (ret, seq, error);
}

static TS_Status
t_fg (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void) ud;
  if (ts_std_argc (v, n, 2, 2, e, "fg") != TS_OK)
    return TS_ERR_INVAL;
  return theme_role_seq (v, a, n, r, e, "fg", false);
}

static TS_Status
t_bg (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void) ud;
  if (ts_std_argc (v, n, 2, 2, e, "bg") != TS_OK)
    return TS_ERR_INVAL;
  return theme_role_seq (v, a, n, r, e, "bg", true);
}

static TS_Status
t_roles (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *sep = ",";
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 1, error, "roles") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &sep, error, "roles") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (i = 0; theme_roles[i]; i++)
    {
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, theme_roles[i]) != 0)
        goto oom;
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    goto oom;
  return TS_OK;
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef theme_funcs[] = {
  { "list", t_list, NULL },
  { "load", t_load, NULL },
  { "fg", t_fg, NULL },
  { "bg", t_bg, NULL },
  { "roles", t_roles, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_theme_module = { "std.theme", theme_funcs };
