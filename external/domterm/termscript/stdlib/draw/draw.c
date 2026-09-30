/* draw.c -- std.draw: plain-text boxes, rules and meters.
 *
 * All functions return strings (newlines embedded); scripts print
 * them with G:puts.  Widths are character counts, fractions are
 * 0..1 clamped.
 */
#include "draw/draw.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <string.h>

static TS_Status
emit_str (TS_Value *ret, ts_sbuf_t *b, TS_Error *error)
{
  char *out = ts_sbuf_take (b);
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
d_hline (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  int64_t n;
  const char *ch = "-";
  ts_sbuf_t b;
  int64_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "hline") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &n, error, "hline") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_str (&argv[1], &ch, error, "hline") != TS_OK)
    return TS_ERR_INVAL;
  if (n < 0 || n > 10000 || !ch[0])
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "hline: bad args");
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  for (i = 0; i < n; i++)
    if (ts_sbuf_ch (&b, ch[0]) != 0)
      {
        ts_sbuf_free (&b);
        ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
        return TS_ERR_NOMEM;
      }
  return emit_str (ret, &b, error);
}

static TS_Status
d_box (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  int64_t w, h;
  const char *title = "";
  ts_sbuf_t b;
  int64_t r, c;
  size_t tlen;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "box") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &w, error, "box") != TS_OK ||
      ts_std_int (&argv[1], &h, error, "box") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_str (&argv[2], &title, error, "box") != TS_OK)
    return TS_ERR_INVAL;
  if (w < 2 || w > 500 || h < 2 || h > 500)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "box: bad size");
      return TS_ERR_INVAL;
    }
  tlen = strlen (title);
  ts_sbuf_init (&b);
  /* Top border, optionally titled: +-- title --+ */
  if (ts_sbuf_ch (&b, '+') != 0)
    goto oom;
  if (tlen && tlen + 4 < (size_t) w)
    {
      if (ts_sbuf_str (&b, "-- ") != 0 || ts_sbuf_str (&b, title) != 0 ||
          ts_sbuf_str (&b, " ") != 0)
        goto oom;
      for (c = (int64_t) (tlen + 4); c < w - 1; c++)
        if (ts_sbuf_ch (&b, '-') != 0)
          goto oom;
    }
  else
    for (c = 1; c < w - 1; c++)
      if (ts_sbuf_ch (&b, '-') != 0)
        goto oom;
  if (ts_sbuf_str (&b, "+\n") != 0)
    goto oom;
  for (r = 0; r < h - 2; r++)
    {
      if (ts_sbuf_ch (&b, '|') != 0)
        goto oom;
      for (c = 1; c < w - 1; c++)
        if (ts_sbuf_ch (&b, ' ') != 0)
          goto oom;
      if (ts_sbuf_str (&b, "|\n") != 0)
        goto oom;
    }
  if (ts_sbuf_ch (&b, '+') != 0)
    goto oom;
  for (c = 1; c < w - 1; c++)
    if (ts_sbuf_ch (&b, '-') != 0)
      goto oom;
  if (ts_sbuf_str (&b, "+\n") != 0)
    goto oom;
  return emit_str (ret, &b, error);
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
d_bar (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  int64_t pct, width = 20;
  int64_t filled, i;
  ts_sbuf_t b;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "bar") != TS_OK)
    return TS_ERR_INVAL;
  /* Percent 0..100 (ints keep the language exact). */
  if (ts_std_int (&argv[0], &pct, error, "bar") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_int (&argv[1], &width, error, "bar") != TS_OK)
    return TS_ERR_INVAL;
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  if (width < 1 || width > 500)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bar: bad width");
      return TS_ERR_INVAL;
    }
  filled = (pct * width) / 100;
  ts_sbuf_init (&b);
  if (ts_sbuf_ch (&b, '[') != 0)
    goto oom;
  for (i = 0; i < width; i++)
    if (ts_sbuf_ch (&b, i < filled ? '#' : '-') != 0)
      goto oom;
  if (ts_sbuf_printf (&b, "] %lld%%", (long long) pct) != 0)
    goto oom;
  return emit_str (ret, &b, error);
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
d_progress (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  int64_t cur, total, width = 20;
  int64_t pct;
  TS_Value bv[2];
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "progress") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &cur, error, "progress") != TS_OK ||
      ts_std_int (&argv[1], &total, error, "progress") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_int (&argv[2], &width, error, "progress") !=
      TS_OK)
    return TS_ERR_INVAL;
  pct = total <= 0 ? 0 : (cur * 100) / total;
  /* Shallow ints only: d_bar reads but never frees argv. */
  bv[0].type = TS_INT;
  bv[0].as.integer = pct;
  if (argc == 3)
    bv[1] = argv[2];
  return d_bar (vm, ud, bv, argc == 3 ? 2 : 1, ret, error);
}

static TS_Status
d_center (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  int64_t width;
  size_t n;
  int64_t pad, i;
  ts_sbuf_t b;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "center") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "center") != TS_OK ||
      ts_std_int (&argv[1], &width, error, "center") != TS_OK)
    return TS_ERR_INVAL;
  if (width < 0 || width > 10000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "center: bad width");
      return TS_ERR_INVAL;
    }
  n = strlen (s);
  ts_sbuf_init (&b);
  if ((int64_t) n >= width)
    {
      if (ts_sbuf_str (&b, s) != 0)
        goto oom;
    }
  else
    {
      pad = (width - (int64_t) n) / 2;
      for (i = 0; i < pad; i++)
        if (ts_sbuf_ch (&b, ' ') != 0)
          goto oom;
      if (ts_sbuf_str (&b, s) != 0)
        goto oom;
      for (i = pad + (int64_t) n; i < width; i++)
        if (ts_sbuf_ch (&b, ' ') != 0)
          goto oom;
    }
  return emit_str (ret, &b, error);
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef draw_funcs[] = {
  { "hline", d_hline, NULL },
  { "box", d_box, NULL },
  { "bar", d_bar, NULL },
  { "progress", d_progress, NULL },
  { "center", d_center, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_draw_module = { "std.draw", draw_funcs };
