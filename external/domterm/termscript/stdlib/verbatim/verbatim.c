/* verbatim.c -- std.verbatim: line-numbered code listings.
 *
 * render(source[, language[, first_line]]) renders literal source as a code
 * listing.  A non-empty language delegates colouring to std.syntax through
 * its shared C helper; "plain" and "text" intentionally leave it untouched.
 */
#include "verbatim/verbatim.h"

#include "common/ts_std_common.h"
#include "syntax/syntax.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static TS_Status
emit (TS_Value *ret, ts_sbuf_t *out, TS_Error *error)
{
  char *text = ts_sbuf_take (out);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = text;
  if (!text)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static size_t
line_count (const char *source)
{
  size_t count = 1;
  const char *p;
  for (p = source; *p; p++)
    if (*p == '\n' && p[1])
      count++;
  return count;
}

static size_t
decimal_width (uint64_t value)
{
  size_t width = 1;
  while (value >= 10)
    {
      value /= 10;
      width++;
    }
  return width;
}

static TS_Status
v_render (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *source;
  const char *language = "";
  int64_t first_line = 1;
  const char *listing;
  char *highlighted = NULL;
  ts_sbuf_t out;
  const char *p;
  size_t line;
  size_t number_width;
  TS_Status status;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 3, error, "render") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &source, error, "render") != TS_OK ||
      (argc >= 2 && ts_std_str (&argv[1], &language, error, "render") != TS_OK) ||
      (argc == 3 && ts_std_int (&argv[2], &first_line, error, "render") != TS_OK))
    return TS_ERR_INVAL;
  if (first_line < 1 || first_line > INT64_MAX - (int64_t) line_count (source))
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "render: bad first line");
      return TS_ERR_INVAL;
    }
  listing = source;
  if (language[0] && strcmp (language, "plain") != 0 &&
      strcmp (language, "text") != 0)
    {
      status = ts_std_syntax_highlight (language, source, &highlighted,
                                        error);
      if (status != TS_OK)
        return status;
      listing = highlighted;
    }
  line = (size_t) first_line;
  number_width = decimal_width ((uint64_t) first_line + line_count (source) - 1);
  ts_sbuf_init (&out);
  for (p = listing;;)
    {
      const char *newline = strchr (p, '\n');
      size_t length = newline ? (size_t) (newline - p) : strlen (p);
      if (ts_sbuf_printf (&out, "%*zu | ", (int) number_width, line) != 0 ||
          ts_sbuf_put (&out, p, length) != 0)
        goto oom;
      if (!newline)
        break;
      if (ts_sbuf_ch (&out, '\n') != 0)
        goto oom;
      line++;
      p = newline + 1;
      if (!*p)
        break; /* A source trailing newline does not create a phantom line. */
    }
  free (highlighted);
  return emit (ret, &out, error);
oom:
  free (highlighted);
  ts_sbuf_free (&out);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef verbatim_funcs[] = {
  { "render", v_render, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_verbatim_module = { "std.verbatim", verbatim_funcs };
