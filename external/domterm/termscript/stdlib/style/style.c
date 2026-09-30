/* style.c -- std.style: SGR composition and escape hygiene.
 *
 * sgr() takes integer codes variadically; wrap() sandwiches text;
 * strip()/vislen()/pad() let scripts align styled columns.
 */
#include "style/style.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <string.h>

static TS_Status
s_sgr (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 16, error, "sgr") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (ts_sbuf_str (&b, "\x1b[") != 0)
    goto oom;
  for (i = 0; i < argc; i++)
    {
      int64_t code;
      if (ts_std_int (&argv[i], &code, error, "sgr") != TS_OK)
        {
          ts_sbuf_free (&b);
          return TS_ERR_INVAL;
        }
      if (i && ts_sbuf_ch (&b, ';') != 0)
        goto oom;
      if (ts_sbuf_printf (&b, "%lld", (long long) code) != 0)
        goto oom;
    }
  if (ts_sbuf_ch (&b, 'm') != 0)
    goto oom;
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
s_wrap (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *style, *text;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "wrap") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &style, error, "wrap") != TS_OK ||
      ts_std_str (&argv[1], &text, error, "wrap") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (ts_sbuf_str (&b, style) != 0 || ts_sbuf_str (&b, text) != 0 ||
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

/* Visible length: skip ESC[...m / ESC…letter and single ESC + char. */
static size_t
vis_len (const char *s)
{
  size_t n = 0;
  for (; *s; s++)
    {
      if ((unsigned char) *s == 0x1B && s[1])
        {
          s++;
          if (*s == '[')
            {
              s++;
              while (*s && !(*s >= '@' && *s <= '~'))
                s++;
              if (!*s)
                break;
            }
        }
      else
        n++;
    }
  return n;
}

static TS_Status
s_strip (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "strip") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "strip") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      if ((unsigned char) *s == 0x1B && s[1])
        {
          s++;
          if (*s == '[')
            {
              s++;
              while (s[0] && !(s[0] >= '@' && s[0] <= '~'))
                s++;
              if (!s[0])
                break;
            }
        }
      else if (ts_sbuf_ch (&b, *s) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
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
s_vislen (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "vislen") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "vislen") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) vis_len (s));
  return TS_OK;
}

static TS_Status
s_pad (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *s;
  int64_t width;
  size_t vis, i;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "pad") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "pad") != TS_OK ||
      ts_std_int (&argv[1], &width, error, "pad") != TS_OK)
    return TS_ERR_INVAL;
  if (width < 0 || width > 10000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "pad: bad width");
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  if (ts_sbuf_str (&b, s) != 0)
    goto oom;
  vis = vis_len (s);
  for (i = vis; (int64_t) i < width; i++)
    if (ts_sbuf_ch (&b, ' ') != 0)
      goto oom;
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

static const TS_FuncDef style_funcs[] = {
  { "sgr", s_sgr, NULL },
  { "wrap", s_wrap, NULL },
  { "strip", s_strip, NULL },
  { "vislen", s_vislen, NULL },
  { "pad", s_pad, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_style_module = { "std.style", style_funcs };
