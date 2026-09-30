/* csv.c -- std.csv: delimiter-separated values.
 *
 * parse implements the double-quote dialect (quoted fields may span
 * the separator and embed doubled quotes); join quotes fields that
 * need it.  Rows are std.list handles of strings.
 */
#include "csv/csv.h"

#include "common/ts_std_common.h"

#include <stdlib.h>
#include <string.h>

static TS_Status
csv_split_line (const char *s, char sep, ts_std_list_t *out, TS_Error *error)
{
  ts_sbuf_t field;
  bool in_quotes = false;
  const char *p = s ? s : "";
  ts_sbuf_init (&field);
  if (!*p)
    return TS_OK; /* Empty line: zero fields. */
  for (;;)
    {
      char c = *p;
      if (in_quotes)
        {
          if (c == '"')
            {
              if (p[1] == '"')
                {
                  if (ts_sbuf_ch (&field, '"') != 0)
                    goto oom;
                  p += 2;
                }
              else
                {
                  in_quotes = false;
                  p++;
                }
            }
          else if (c == '\0')
            {
              ts_sbuf_free (&field);
              ts_error_set (error, TS_ERR_PARSE, 0, 0,
                            "csv: unterminated quote");
              return TS_ERR_PARSE;
            }
          else
            {
              if (ts_sbuf_ch (&field, c) != 0)
                goto oom;
              p++;
            }
        }
      else if (c == '"')
        {
          in_quotes = true;
          p++;
        }
      else if (c == sep || c == '\0')
        {
          char *taken = ts_sbuf_take (&field);
          TS_Status st;
          if (!taken)
            goto oom;
          st = ts_std_list_push_str (out, taken);
          free (taken);
          if (st != TS_OK)
            {
              ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
          if (c == '\0')
            break;
          p++;
        }
      else
        {
          if (ts_sbuf_ch (&field, c) != 0)
            goto oom;
          p++;
        }
    }
  return TS_OK;
oom:
  ts_sbuf_free (&field);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
csv_sep (const TS_Value *v, char *sep, TS_Error *error)
{
  const char *s;
  if (ts_std_str (v, &s, error, "csv") != TS_OK)
    return TS_ERR_INVAL;
  if (!s[0] || s[1])
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "csv: separator must be one char");
      return TS_ERR_INVAL;
    }
  *sep = s[0];
  return TS_OK;
}

static TS_Status
c_parse (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  char sep = ',';
  ts_std_list_t *rows;
  const char *p;
  ts_sbuf_t logical;
  bool in_quotes = false;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && csv_sep (&argv[1], &sep, error) != TS_OK)
    return TS_ERR_INVAL;
  rows = ts_std_list_new ();
  if (!rows)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  /* Split physical lines, honouring quotes that span newlines. */
  ts_sbuf_init (&logical);
  {
    for (p = text;; p++)
      {
        char c = *p;
        if (c == '"')
          in_quotes = !in_quotes;
        if ((c == '\n' || c == '\0') && !in_quotes)
          {
            ts_std_list_t *fields = ts_std_list_new ();
            TS_Value row;
            if (!fields)
              goto fail;
            if (csv_split_line (logical.data ? logical.data : "",
                                sep, fields, error) != TS_OK)
              {
                ts_std_list_free (fields);
                goto fail;
              }
            ts_sbuf_clear (&logical);
            row.type = TS_NIL;
            if (ts_std_list_wrap (&row, fields, error) != TS_OK)
              {
                ts_std_list_free (fields);
                goto fail;
              }
            if (ts_std_list_push (rows, &row) != TS_OK)
              {
                ts_value_free (&row);
                goto fail;
              }
            ts_value_free (&row);
            if (c == '\0')
              break;
          }
        else
          {
            if (c == '\r' && (p[1] == '\n' || p[1] == '\0') && !in_quotes)
              continue;
            if (ts_sbuf_ch (&logical, c) != 0)
              goto fail;
          }
        if (c == '\0')
          break;
      }
    ts_sbuf_free (&logical);
  }
  return ts_std_list_wrap (ret, rows, error);
fail:
  ts_sbuf_free (&logical);
  ts_std_list_free (rows);
  if (!error || error->code == TS_OK)
    ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
c_parse_line (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
              TS_Value *ret, TS_Error *error)
{
  const char *text;
  char sep = ',';
  ts_std_list_t *fields;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "parse_line") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "parse_line") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && csv_sep (&argv[1], &sep, error) != TS_OK)
    return TS_ERR_INVAL;
  fields = ts_std_list_new ();
  if (!fields)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (csv_split_line (text, sep, fields, error) != TS_OK)
    {
      ts_std_list_free (fields);
      return error ? error->code : TS_ERR_PARSE;
    }
  return ts_std_list_wrap (ret, fields, error);
}

static bool
csv_needs_quote (const char *s, char sep)
{
  for (; *s; s++)
    if (*s == sep || *s == '"' || *s == '\n' || *s == '\r')
      return true;
  return false;
}

static TS_Status
c_join (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  char sep = ',';
  size_t first = 0;
  ts_sbuf_t b;
  size_t i;
  char *out;
  const TS_Value *vals = argv;
  size_t nvals = argc;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 99, error, "join") != TS_OK)
    return TS_ERR_INVAL;
  /* join(list) / join(sep, values...) / join(list, sep)?  Resolve:
   * a single list handle joins with ','; a leading string is the
   * separator. */
  if (argc >= 1 && argv[0].type == TS_STRING)
    {
      if (csv_sep (&argv[0], &sep, error) != TS_OK)
        return TS_ERR_INVAL;
      first = 1;
    }
  if (argc - first == 1 && argv[first].type == TS_HANDLE)
    {
      ts_std_list_t *l = ts_std_list_unwrap (&argv[first], error, "join");
      ts_sbuf_init (&b);
      if (!l)
        return TS_ERR_INVAL;
      for (i = 0; i < ts_std_list_len (l); i++)
        {
          char *s = ts_std_to_str (ts_std_list_get (l, i), error);
          if (!s)
            {
              ts_sbuf_free (&b);
              return TS_ERR_NOMEM;
            }
          if (i && ts_sbuf_ch (&b, sep) != 0)
            goto oom_s;
          if (csv_needs_quote (s, sep))
            {
              const char *q;
              if (ts_sbuf_ch (&b, '"') != 0)
                goto oom_s;
              for (q = s; *q; q++)
                {
                  if (*q == '"' && ts_sbuf_ch (&b, '"') != 0)
                    goto oom_s;
                  if (ts_sbuf_ch (&b, *q) != 0)
                    goto oom_s;
                }
              if (ts_sbuf_ch (&b, '"') != 0)
                goto oom_s;
            }
          else if (ts_sbuf_str (&b, s) != 0)
            {
            oom_s:
              free (s);
              ts_sbuf_free (&b);
              ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
          free (s);
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
  vals = argv + first;
  nvals = argc - first;
  ts_sbuf_init (&b);
  for (i = 0; i < nvals; i++)
    {
      char *s = ts_std_to_str (&vals[i], error);
      if (!s)
        {
          ts_sbuf_free (&b);
          return TS_ERR_NOMEM;
        }
      if (i && ts_sbuf_ch (&b, sep) != 0)
        {
          free (s);
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      if (ts_sbuf_str (&b, s) != 0)
        {
          free (s);
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      free (s);
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
c_field (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  int64_t idx;
  char sep = ',';
  ts_std_list_t *fields = NULL;
  const TS_Value *v;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "field") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "field") != TS_OK ||
      ts_std_int (&argv[1], &idx, error, "field") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && csv_sep (&argv[2], &sep, error) != TS_OK)
    return TS_ERR_INVAL;
  fields = ts_std_list_new ();
  if (!fields)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (csv_split_line (text, sep, fields, error) != TS_OK)
    {
      st = error ? error->code : TS_ERR_PARSE;
      ts_std_list_free (fields);
      return st;
    }
  v = idx >= 0 ? ts_std_list_get (fields, (size_t) idx) : NULL;
  if (!v)
    {
      ts_std_list_free (fields);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  st = ts_value_copy (ret, v);
  ts_std_list_free (fields);
  if (st != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static const TS_FuncDef csv_funcs[] = {
  { "parse", c_parse, NULL },
  { "parse_line", c_parse_line, NULL },
  { "join", c_join, NULL },
  { "field", c_field, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_csv_module = { "std.csv", csv_funcs };
