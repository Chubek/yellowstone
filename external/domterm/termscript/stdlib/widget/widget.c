/* widget.c -- std.widget: aligned tables, meters and spinners.
 *
 * table() aligns \t-separated columns across \n-separated rows;
 * progress()/gauge() render int-percent meters; spinner() cycles
 * frames for scripts driving their own redraw loops.
 */
#include "widget/widget.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
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
w_table (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *rows, *colsep = "\t";
  int64_t gap = 2;
  ts_sbuf_t b;
  size_t *widths = NULL, ncols = 0;
  const char *p;
  int64_t g;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 3, error, "table") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &rows, error, "table") != TS_OK)
    return TS_ERR_INVAL;
  if (argc >= 2 && ts_std_str (&argv[1], &colsep, error, "table") !=
      TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_int (&argv[2], &gap, error, "table") != TS_OK)
    return TS_ERR_INVAL;
  if (!colsep[0] || gap < 0 || gap > 32)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "table: bad args");
      return TS_ERR_INVAL;
    }
  /* Pass 1: column widths. */
  for (p = rows;;)
    {
      const char *nl = strchr (p, '\n');
      const char *cp = p;
      size_t c = 0;
      for (;;)
        {
          const char *hit = strstr (cp, colsep);
          const char *lim = nl ? nl : cp + strlen (cp);
          const char *end = (hit && hit < lim) ? hit : lim;
          size_t w = (size_t) (end - cp);
          size_t *nw;
          if (c >= ncols)
            {
              nw = realloc (widths, (c + 1) * sizeof *nw);
              if (!nw)
                goto oom;
              widths = nw;
              widths[c] = 0;
              ncols = c + 1;
            }
          if (w > widths[c])
            widths[c] = w;
          if (!hit || (nl && hit >= nl))
            break;
          cp = hit + strlen (colsep);
          c++;
        }
      if (!nl)
        break;
      p = nl + 1;
    }
  /* Pass 2: render. */
  ts_sbuf_init (&b);
  for (p = rows;;)
    {
      const char *nl = strchr (p, '\n');
      const char *cp = p;
      size_t c = 0;
      for (;;)
        {
          const char *hit = strstr (cp, colsep);
          const char *lim = nl ? nl : cp + strlen (cp);
          const char *end = (hit && hit < lim) ? hit : lim;
          size_t w = (size_t) (end - cp);
          size_t i;
          bool last = !hit || (nl && hit >= nl);
          if (ts_sbuf_put (&b, cp, w) != 0)
            goto oom2;
          if (!last)
            {
              for (i = w; i < widths[c]; i++)
                if (ts_sbuf_ch (&b, ' ') != 0)
                  goto oom2;
              for (g = 0; g < gap; g++)
                if (ts_sbuf_ch (&b, ' ') != 0)
                  goto oom2;
            }
          if (last)
            break;
          cp = hit + strlen (colsep);
          c++;
        }
      if (ts_sbuf_ch (&b, '\n') != 0)
        goto oom2;
      if (!nl)
        break;
      p = nl + 1;
    }
  free (widths);
  return emit_str (ret, &b, error);
oom2:
  ts_sbuf_free (&b);
oom:
  free (widths);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
meter (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
       TS_Error *error, const char *fname, bool pct_label)
{
  int64_t pct, width = 20;
  int64_t filled, i;
  ts_sbuf_t b;
  if (ts_std_argc (vm, argc, 2, 3, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &pct, error, fname) != TS_OK ||
      ts_std_int (&argv[1], &width, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3)
    {
      /* Third arg re-specifies width for progress(cur,total,width). */
      if (ts_std_int (&argv[2], &width, error, fname) != TS_OK)
        return TS_ERR_INVAL;
    }
  if (width < 1 || width > 500)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: bad width",
                    fname);
      return TS_ERR_INVAL;
    }
  if (pct < 0)
    pct = 0;
  if (pct > 100)
    pct = 100;
  filled = (pct * width) / 100;
  ts_sbuf_init (&b);
  if (ts_sbuf_ch (&b, '[') != 0)
    goto oom;
  for (i = 0; i < width; i++)
    if (ts_sbuf_ch (&b, i < filled ? '=' : ' ') != 0)
      goto oom;
  if (ts_sbuf_ch (&b, ']') != 0)
    goto oom;
  if (pct_label && ts_sbuf_printf (&b, " %lld%%", (long long) pct) != 0)
    goto oom;
  return emit_str (ret, &b, error);
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
w_progress (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  int64_t cur, total;
  TS_Value mv[2];
  (void) ud;
  /* progress(cur, total[, width]): convert to percent, delegate. */
  if (ts_std_argc (vm, argc, 2, 3, error, "progress") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &cur, error, "progress") != TS_OK ||
      ts_std_int (&argv[1], &total, error, "progress") != TS_OK)
    return TS_ERR_INVAL;
  mv[0].type = TS_INT;
  mv[0].as.integer = total <= 0 ? 0 : (cur * 100) / total;
  mv[1].type = TS_INT;
  mv[1].as.integer = 20;
  if (argc == 3)
    mv[1] = argv[2]; /* ints only: read, never freed. */
  return meter (vm, mv, 2, ret, error, "progress", true);
}

static TS_Status
w_gauge (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return meter (v, a, n, r, e, "gauge", false);
}

static TS_Status
w_spinner (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  int64_t i;
  static const char frames[] = "|/-\\";
  char f[2];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "spinner") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &i, error, "spinner") != TS_OK)
    return TS_ERR_INVAL;
  if (i < 0)
    i = -i;
  f[0] = frames[i % 4];
  f[1] = '\0';
  return ts_std_ret_str (ret, f, error);
}

static const TS_FuncDef widget_funcs[] = {
  { "table", w_table, NULL },
  { "progress", w_progress, NULL },
  { "gauge", w_gauge, NULL },
  { "spinner", w_spinner, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_widget_module = { "std.widget", widget_funcs };
