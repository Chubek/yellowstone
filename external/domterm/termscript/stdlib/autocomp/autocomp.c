/* autocomp.c -- std.autocomp: prefix completion over strings.
 *
 * Candidates travel as one separator-joined string (usually from
 * std.codec:split or std.io:lines joins); results come back joined
 * the same way, so no handles cross the boundary.
 */
#include "autocomp/autocomp.h"

#include "common/ts_std_common.h"
#include "gen/ts_strvec.h"

#include <stdlib.h>
#include <string.h>

static TS_Status
split_owned (const char *text, const char *sep, ts_strvec_t *out,
             TS_Error *error, const char *fname)
{
  const char *p = text ? text : "";
  size_t slen = strlen (sep);
  if (!slen)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: empty separator",
                    fname);
      return TS_ERR_INVAL;
    }
  for (;;)
    {
      const char *hit = strstr (p, sep);
      size_t n = hit ? (size_t) (hit - p) : strlen (p);
      char **slot = ts_strvec_add (out);
      if (!slot)
        {
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      *slot = strndup (p, n);
      if (!*slot)
        {
          out->len--;
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      if (!hit)
        break;
      p = hit + slen;
    }
  return TS_OK;
}

static TS_Status
join_vec (ts_strvec_t *v, const char *sep, TS_Value *ret, TS_Error *error)
{
  ts_sbuf_t b;
  size_t i;
  char *out;
  ts_sbuf_init (&b);
  for (i = 0; i < ts_strvec_len (v); i++)
    {
      char **s = ts_strvec_get (v, i);
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, *s ? *s : "") != 0)
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
a_complete (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  const char *prefix, *cands, *sep = "\n";
  ts_strvec_t all, hits;
  size_t i;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "complete") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &prefix, error, "complete") != TS_OK ||
      ts_std_str (&argv[1], &cands, error, "complete") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_str (&argv[2], &sep, error, "complete") !=
      TS_OK)
    return TS_ERR_INVAL;
  ts_strvec_init (&all);
  ts_strvec_init (&hits);
  if (split_owned (cands, sep, &all, error, "complete") != TS_OK)
    {
      ts_strvec_free (&all);
      ts_strvec_free (&hits);
      return TS_ERR_INVAL;
    }
  {
    size_t plen = strlen (prefix);
    for (i = 0; i < ts_strvec_len (&all); i++)
      {
        char **s = ts_strvec_get (&all, i);
        if (strncmp (*s ? *s : "", prefix, plen) == 0)
          {
            char **slot = ts_strvec_add (&hits);
            if (!slot)
              goto oom;
            /* Transfer ownership: all must not free it. */
            *slot = *s;
            *s = NULL;
          }
      }
  }
  st = join_vec (&hits, sep, ret, error);
  ts_strvec_free (&all);
  ts_strvec_free (&hits);
  return st;
oom:
  ts_strvec_free (&all);
  ts_strvec_free (&hits);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
a_common (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *cands, *sep = "\n";
  ts_strvec_t all;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "common") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cands, error, "common") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_str (&argv[1], &sep, error, "common") != TS_OK)
    return TS_ERR_INVAL;
  ts_strvec_init (&all);
  if (split_owned (cands, sep, &all, error, "common") != TS_OK)
    {
      ts_strvec_free (&all);
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  if (ts_strvec_len (&all))
    {
      const char *first = *ts_strvec_get (&all, 0);
      size_t n = 0;
      if (!first)
        first = "";
      for (;;)
        {
          size_t i;
          char c = first[n];
          if (!c)
            break;
          for (i = 1; i < ts_strvec_len (&all); i++)
            {
              const char *s = *ts_strvec_get (&all, i);
              if (!s || s[n] != c)
                break;
            }
          if (i < ts_strvec_len (&all))
            break;
          if (ts_sbuf_ch (&b, c) != 0)
            {
              ts_strvec_free (&all);
              ts_sbuf_free (&b);
              ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
          n++;
        }
    }
  ts_strvec_free (&all);
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
a_rank (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *prefix, *cands, *sep = "\n";
  ts_strvec_t all, hits;
  size_t i;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "rank") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &prefix, error, "rank") != TS_OK ||
      ts_std_str (&argv[1], &cands, error, "rank") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_str (&argv[2], &sep, error, "rank") != TS_OK)
    return TS_ERR_INVAL;
  ts_strvec_init (&all);
  ts_strvec_init (&hits);
  if (split_owned (cands, sep, &all, error, "rank") != TS_OK)
    {
      ts_strvec_free (&all);
      ts_strvec_free (&hits);
      return TS_ERR_INVAL;
    }
  {
    /* Stable two-pass: prefix matches first, then substring matches. */
    int pass;
    size_t plen = strlen (prefix);
    for (pass = 0; pass < 2; pass++)
      for (i = 0; i < ts_strvec_len (&all); i++)
        {
          char **s = ts_strvec_get (&all, i);
          const char *text = *s ? *s : "";
          bool is_prefix = strncmp (text, prefix, plen) == 0;
          bool want = pass == 0 ? is_prefix :
                                  (!is_prefix &&
                                   strstr (text, prefix) != NULL);
          if (want)
            {
              char **slot = ts_strvec_add (&hits);
              char *copy;
              if (!slot)
                goto oom;
              copy = strdup (text);
              if (!copy)
                {
                  hits.len--;
                  goto oom;
                }
              *slot = copy;
            }
        }
  }
  st = join_vec (&hits, sep, ret, error);
  ts_strvec_free (&all);
  ts_strvec_free (&hits);
  return st;
oom:
  ts_strvec_free (&all);
  ts_strvec_free (&hits);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef autocomp_funcs[] = {
  { "complete", a_complete, NULL },
  { "common", a_common, NULL },
  { "rank", a_rank, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_autocomp_module = { "std.autocomp",
                                           autocomp_funcs };
