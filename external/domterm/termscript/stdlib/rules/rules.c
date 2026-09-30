/* rules.c -- std.rules: horizontal and vertical text rules. */
#include "rules/rules.h"

#include "common/ts_std_common.h"

#include <stdint.h>
#include <stdlib.h>

char *
ts_std_rules_table_border (const size_t *widths, size_t count, char fill)
{
  ts_sbuf_t out;
  size_t i, j;
  if (!widths || !count)
    return NULL;
  ts_sbuf_init (&out);
  for (i = 0; i < count; i++)
    {
      if (ts_sbuf_ch (&out, '+') != 0)
        goto fail;
      for (j = 0; j < widths[i] + 2; j++)
        if (ts_sbuf_ch (&out, fill) != 0)
          goto fail;
    }
  if (ts_sbuf_ch (&out, '+') != 0)
    goto fail;
  return ts_sbuf_take (&out);
fail:
  ts_sbuf_free (&out);
  return NULL;
}

static TS_Status
r_horizontal (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
              TS_Value *ret, TS_Error *error)
{
  int64_t width;
  const char *glyph = "-";
  ts_sbuf_t out;
  int64_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "horizontal") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &width, error, "horizontal") != TS_OK ||
      (argc == 2 && ts_std_str (&argv[1], &glyph, error, "horizontal") != TS_OK))
    return TS_ERR_INVAL;
  if (width < 0 || width > 10000 || !glyph[0])
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "horizontal: bad args");
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&out);
  for (i = 0; i < width; i++)
    if (ts_sbuf_ch (&out, glyph[0]) != 0)
      goto oom;
  {
    char *text = ts_sbuf_take (&out);
    ts_value_free (ret);
    ret->type = TS_STRING;
    ret->as.string = text;
    if (text)
      return TS_OK;
  }
oom:
  ts_sbuf_free (&out);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
r_vertical (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  int64_t height;
  const char *glyph = "|";
  ts_sbuf_t out;
  int64_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "vertical") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &height, error, "vertical") != TS_OK ||
      (argc == 2 && ts_std_str (&argv[1], &glyph, error, "vertical") != TS_OK))
    return TS_ERR_INVAL;
  if (height < 0 || height > 10000 || !glyph[0])
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "vertical: bad args");
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&out);
  for (i = 0; i < height; i++)
    {
      if (i && ts_sbuf_ch (&out, '\n') != 0)
        goto oom;
      if (ts_sbuf_ch (&out, glyph[0]) != 0)
        goto oom;
    }
  {
    char *text = ts_sbuf_take (&out);
    ts_value_free (ret);
    ret->type = TS_STRING;
    ret->as.string = text;
    if (text)
      return TS_OK;
  }
oom:
  ts_sbuf_free (&out);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef rules_funcs[] = {
  { "horizontal", r_horizontal, NULL },
  { "vertical", r_vertical, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_rules_module = { "std.rules", rules_funcs };
