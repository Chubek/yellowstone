/* color.c -- std.color: named ANSI colors and attributes.
 *
 * Pure string builders: every function returns escape sequences or
 * wrapped text, never writes.  The palette is the 8 standard colors
 * plus bright- variants, by name.
 */
#include "color/color.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <string.h>

static const char *const color_names[] = {
  "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white",
  NULL
};

static int
color_index (const char *name, bool *bright)
{
  size_t i;
  const char *base = name;
  *bright = false;
  if (strncmp (name, "bright-", 7) == 0)
    {
      *bright = true;
      base = name + 7;
    }
  for (i = 0; color_names[i]; i++)
    if (strcmp (base, color_names[i]) == 0)
      return (int) i;
  return -1;
}

static TS_Status
color_code (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
            TS_Error *error, const char *fname, int ground)
{
  const char *name;
  bool bright = false;
  int idx;
  char seq[32];
  if (ts_std_argc (vm, argc, 1, 1, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &name, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  idx = color_index (name, &bright);
  if (idx < 0)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: unknown color '%s'",
                    fname, name);
      return TS_ERR_INVAL;
    }
  snprintf (seq, sizeof seq, "\x1b[%dm", ground + idx + (bright ? 60 : 0));
  return ts_std_ret_str (ret, seq, error);
}

static TS_Status
c_fg (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void) ud;
  return color_code (v, a, n, r, e, "fg", 30);
}

static TS_Status
c_bg (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
      TS_Error *e)
{
  (void) ud;
  return color_code (v, a, n, r, e, "bg", 40);
}

static TS_Status
c_rgb (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  int64_t cr, cg, cb;
  char seq[32];
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "rgb") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &cr, error, "rgb") != TS_OK ||
      ts_std_int (&argv[1], &cg, error, "rgb") != TS_OK ||
      ts_std_int (&argv[2], &cb, error, "rgb") != TS_OK)
    return TS_ERR_INVAL;
  if (cr < 0 || cr > 255 || cg < 0 || cg > 255 || cb < 0 || cb > 255)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "rgb: 0..255 required");
      return TS_ERR_INVAL;
    }
  snprintf (seq, sizeof seq, "\x1b[38;2;%lld;%lld;%lldm", (long long) cr,
            (long long) cg, (long long) cb);
  return ts_std_ret_str (ret, seq, error);
}

static TS_Status
c_on_rgb (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  int64_t cr, cg, cb;
  char seq[32];
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "on_rgb") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &cr, error, "on_rgb") != TS_OK ||
      ts_std_int (&argv[1], &cg, error, "on_rgb") != TS_OK ||
      ts_std_int (&argv[2], &cb, error, "on_rgb") != TS_OK)
    return TS_ERR_INVAL;
  if (cr < 0 || cr > 255 || cg < 0 || cg > 255 || cb < 0 || cb > 255)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "on_rgb: 0..255 required");
      return TS_ERR_INVAL;
    }
  snprintf (seq, sizeof seq, "\x1b[48;2;%lld;%lld;%lldm", (long long) cr,
            (long long) cg, (long long) cb);
  return ts_std_ret_str (ret, seq, error);
}

static TS_Status
c_attr (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error, const char *fname, const char *seq)
{
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, seq, error);
}

static TS_Status
c_reset (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return c_attr (v, a, n, r, e, "reset", "\x1b[0m");
}

static TS_Status
c_bold (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void) ud;
  return c_attr (v, a, n, r, e, "bold", "\x1b[1m");
}

static TS_Status
c_underline (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
             TS_Error *e)
{
  (void) ud;
  return c_attr (v, a, n, r, e, "underline", "\x1b[4m");
}

static TS_Status
c_reverse (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  (void) ud;
  return c_attr (v, a, n, r, e, "reverse", "\x1b[7m");
}

static TS_Status
c_wrap (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *code, *text;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "wrap") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &code, error, "wrap") != TS_OK ||
      ts_std_str (&argv[1], &text, error, "wrap") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (ts_sbuf_str (&b, code) != 0 || ts_sbuf_str (&b, text) != 0 ||
      ts_sbuf_str (&b, "\x1b[0m") != 0)
    {
      ts_sbuf_free (&b);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
c_names (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *sep = ",";
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 1, error, "names") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &sep, error, "names") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (i = 0; color_names[i]; i++)
    {
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, color_names[i]) != 0)
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

static const TS_FuncDef color_funcs[] = {
  { "fg", c_fg, NULL },
  { "bg", c_bg, NULL },
  { "rgb", c_rgb, NULL },
  { "on_rgb", c_on_rgb, NULL },
  { "reset", c_reset, NULL },
  { "bold", c_bold, NULL },
  { "underline", c_underline, NULL },
  { "reverse", c_reverse, NULL },
  { "wrap", c_wrap, NULL },
  { "names", c_names, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_color_module = { "std.color", color_funcs };
