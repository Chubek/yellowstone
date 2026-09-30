/* panel.c -- std.panel: bordered boxes and side-by-side columns.
 *
 * box() frames a (possibly multi-line) body with an optional title;
 * columns() merges two multi-line strings side by side with a gap.
 */
#include "panel/panel.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t
widest (const char *body)
{
  size_t best = 0;
  const char *p = body ? body : "";
  for (;;)
    {
      const char *nl = strchr (p, '\n');
      size_t n = nl ? (size_t) (nl - p) : strlen (p);
      if (n > best)
        best = n;
      if (!nl)
        break;
      p = nl + 1;
    }
  return best;
}

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

static int
emit_rule (ts_sbuf_t *b, size_t width)
{
  size_t i;
  if (ts_sbuf_ch (b, '+') != 0)
    return -1;
  for (i = 0; i < width + 2; i++)
    if (ts_sbuf_ch (b, '-') != 0)
      return -1;
  return ts_sbuf_str (b, "+\n");
}

static TS_Status
p_box (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *title, *body;
  int64_t width = 0;
  size_t w;
  ts_sbuf_t b;
  const char *p;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "box") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &title, error, "box") != TS_OK ||
      ts_std_str (&argv[1], &body, error, "box") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_int (&argv[2], &width, error, "box") != TS_OK)
    return TS_ERR_INVAL;
  w = widest (body);
  if (width > 0 && (size_t) width > w)
    w = (size_t) width;
  if (w > 500)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "box: too wide");
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  if (title[0])
    {
      if (ts_sbuf_printf (&b, "+- %s ", title) != 0)
        goto oom;
      {
        size_t used = strlen (title) + 4;
        size_t i;
        for (i = used; i + 1 < w + 4; i++)
          if (ts_sbuf_ch (&b, '-') != 0)
            goto oom;
      }
      if (ts_sbuf_str (&b, "+\n") != 0)
        goto oom;
    }
  else if (emit_rule (&b, w) != 0)
    goto oom;
  for (p = body;;)
    {
      const char *nl = strchr (p, '\n');
      size_t n = nl ? (size_t) (nl - p) : strlen (p);
      size_t i;
      if (ts_sbuf_str (&b, "| ") != 0 ||
          ts_sbuf_put (&b, p, n) != 0)
        goto oom;
      for (i = n; i < w; i++)
        if (ts_sbuf_ch (&b, ' ') != 0)
          goto oom;
      if (ts_sbuf_str (&b, " |\n") != 0)
        goto oom;
      if (!nl)
        break;
      p = nl + 1;
    }
  if (emit_rule (&b, w) != 0)
    goto oom;
  return emit_str (ret, &b, error);
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
p_columns (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *left, *right;
  int64_t gap = 2;
  ts_sbuf_t b;
  const char *lp, *rp;
  size_t lw;
  int64_t g;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "columns") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &left, error, "columns") != TS_OK ||
      ts_std_str (&argv[1], &right, error, "columns") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_int (&argv[2], &gap, error, "columns") !=
      TS_OK)
    return TS_ERR_INVAL;
  if (gap < 0 || gap > 32)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "columns: bad gap");
      return TS_ERR_INVAL;
    }
  lw = widest (left);
  ts_sbuf_init (&b);
  lp = left;
  rp = right;
  for (;;)
    {
      const char *lnl = strchr (lp, '\n');
      const char *rnl = strchr (rp, '\n');
      size_t ln = lnl ? (size_t) (lnl - lp) : strlen (lp);
      size_t rn = rnl ? (size_t) (rnl - rp) : strlen (rp);
      size_t i;
      bool lmore = lnl != NULL, rmore = rnl != NULL;
      if (ts_sbuf_put (&b, lp, ln) != 0)
        goto oom;
      for (i = ln; i < lw; i++)
        if (ts_sbuf_ch (&b, ' ') != 0)
          goto oom;
      if (rmore || rn)
        {
          for (g = 0; g < gap; g++)
            if (ts_sbuf_ch (&b, ' ') != 0)
              goto oom;
          if (ts_sbuf_put (&b, rp, rn) != 0)
            goto oom;
        }
      if (ts_sbuf_ch (&b, '\n') != 0)
        goto oom;
      if (!lmore && !rmore)
        break;
      lp = lmore ? lnl + 1 : "";
      rp = rmore ? rnl + 1 : "";
    }
  return emit_str (ret, &b, error);
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef panel_funcs[] = {
  { "box", p_box, NULL },
  { "columns", p_columns, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_panel_module = { "std.panel", panel_funcs };
