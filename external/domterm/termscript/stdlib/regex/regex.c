/* regex.c -- std.regex: POSIX extended regular expressions.
 *
 * Thin, predictable wrapper over libc regcomp/regexec: search() tests
 * anywhere, full() anchors, find() returns the leftmost match, and
 * replace() expands & and \1..\9.
 */
#include "regex/regex.h"

#include "common/ts_std_common.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>

static TS_Status
rx_compile (regex_t *re, const char *pat, TS_Error *error, const char *fname)
{
  char msg[128];
  int rc = regcomp (re, pat, REG_EXTENDED);
  if (rc != 0)
    {
      regerror (rc, re, msg, sizeof msg);
      ts_error_set (error, TS_ERR_PARSE, 0, 0, "%s: %s", fname, msg);
      return TS_ERR_PARSE;
    }
  return TS_OK;
}

static TS_Status
r_search (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *pat, *s;
  regex_t re;
  int hit;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "search") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "search") != TS_OK ||
      ts_std_str (&argv[1], &s, error, "search") != TS_OK)
    return TS_ERR_INVAL;
  if (rx_compile (&re, pat, error, "search") != TS_OK)
    return TS_ERR_PARSE;
  hit = regexec (&re, s, 0, NULL, 0) == 0;
  regfree (&re);
  ts_std_ret_bool (ret, hit);
  return TS_OK;
}

static TS_Status
r_full (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *pat, *s;
  regex_t re;
  int hit;
  ts_sbuf_t anchored;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "full") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "full") != TS_OK ||
      ts_std_str (&argv[1], &s, error, "full") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&anchored);
  if (ts_sbuf_str (&anchored, "^(") != 0 ||
      ts_sbuf_str (&anchored, pat) != 0 ||
      ts_sbuf_str (&anchored, ")$") != 0)
    {
      ts_sbuf_free (&anchored);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (rx_compile (&re, anchored.data, error, "full") != TS_OK)
    {
      ts_sbuf_free (&anchored);
      return TS_ERR_PARSE;
    }
  ts_sbuf_free (&anchored);
  hit = regexec (&re, s, 0, NULL, 0) == 0;
  regfree (&re);
  ts_std_ret_bool (ret, hit);
  return TS_OK;
}

static TS_Status
r_find (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *pat, *s;
  regex_t re;
  regmatch_t m[1];
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "find") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "find") != TS_OK ||
      ts_std_str (&argv[1], &s, error, "find") != TS_OK)
    return TS_ERR_INVAL;
  if (rx_compile (&re, pat, error, "find") != TS_OK)
    return TS_ERR_PARSE;
  if (regexec (&re, s, 1, m, 0) != 0)
    {
      regfree (&re);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  regfree (&re);
  return ts_std_ret_strn (ret, s + m[0].rm_so,
                          (size_t) (m[0].rm_eo - m[0].rm_so), error);
}

static TS_Status
r_count (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *pat, *s;
  regex_t re;
  regmatch_t m[1];
  long n = 0;
  const char *cursor;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "count") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "count") != TS_OK ||
      ts_std_str (&argv[1], &s, error, "count") != TS_OK)
    return TS_ERR_INVAL;
  if (rx_compile (&re, pat, error, "count") != TS_OK)
    return TS_ERR_PARSE;
  cursor = s;
  while (regexec (&re, cursor, 1, m, 0) == 0)
    {
      n++;
      if ((size_t) m[0].rm_eo == 0)
        break; /* Empty match: count once, stop. */
      cursor += m[0].rm_eo;
      if (n > 1000000)
        break;
    }
  regfree (&re);
  ts_std_ret_int (ret, n);
  return TS_OK;
}

static TS_Status
r_replace (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *pat, *s, *rep;
  regex_t re;
  regmatch_t m[10];
  ts_sbuf_t b;
  const char *cursor;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "replace") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "replace") != TS_OK ||
      ts_std_str (&argv[1], &s, error, "replace") != TS_OK ||
      ts_std_str (&argv[2], &rep, error, "replace") != TS_OK)
    return TS_ERR_INVAL;
  if (rx_compile (&re, pat, error, "replace") != TS_OK)
    return TS_ERR_PARSE;
  ts_sbuf_init (&b);
  cursor = s;
  while (regexec (&re, cursor, 10, m, 0) == 0)
    {
      const char *q;
      if (ts_sbuf_put (&b, cursor, (size_t) m[0].rm_so) != 0)
        goto oom;
      for (q = rep; *q; q++)
        {
          if (*q == '&')
            {
              if (ts_sbuf_put (&b, cursor + m[0].rm_so,
                               (size_t) (m[0].rm_eo - m[0].rm_so)) != 0)
                goto oom;
            }
          else if (*q == '\\' && q[1] >= '1' && q[1] <= '9')
            {
              int g = q[1] - '0';
              q++;
              if (m[g].rm_so >= 0)
                if (ts_sbuf_put (&b, cursor + m[g].rm_so,
                                 (size_t) (m[g].rm_eo - m[g].rm_so)) !=
                    0)
                  goto oom;
            }
          else if (ts_sbuf_ch (&b, *q) != 0)
            goto oom;
        }
      if ((size_t) m[0].rm_eo == 0)
        break; /* Empty match: replace once, stop. */
      cursor += m[0].rm_eo;
    }
  regfree (&re);
  if (ts_sbuf_str (&b, cursor) != 0)
    goto oom;
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    goto oom;
  return TS_OK;
oom:
  regfree (&re);
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef regex_funcs[] = {
  { "search", r_search, NULL },
  { "full", r_full, NULL },
  { "find", r_find, NULL },
  { "count", r_count, NULL },
  { "replace", r_replace, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_regex_module = { "std.regex", regex_funcs };
