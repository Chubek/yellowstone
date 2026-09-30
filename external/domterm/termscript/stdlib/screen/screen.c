/* screen.c -- std.screen: an in-memory character grid.
 *
 * put() writes text at a cell, render()/text() dump the grid;
 * std.vterm feeds escape sequences into the same shape of buffer.
 */
#include "screen/screen.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  char *cells;
  size_t rows, cols;
} screen_t;

static const char screen_tag_id = 0;

static void
screen_free (void *p)
{
  screen_t *s = p;
  if (!s)
    return;
  free (s->cells);
  free (s);
}

static screen_t *
screen_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &screen_tag_id, error, what);
}

static TS_Status
s_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  int64_t rows = 24, cols = 80;
  screen_t *s;
  char desc[64];
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 2, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc >= 1 && ts_std_int (&argv[0], &rows, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_int (&argv[1], &cols, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (rows < 1 || rows > 500 || cols < 1 || cols > 500)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "new: bad size");
      return TS_ERR_INVAL;
    }
  s = calloc (1, sizeof *s);
  if (!s)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  s->cells = malloc ((size_t) rows * (size_t) cols);
  if (!s->cells)
    {
      free (s);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  memset (s->cells, ' ', (size_t) rows * (size_t) cols);
  s->rows = (size_t) rows;
  s->cols = (size_t) cols;
  snprintf (desc, sizeof desc, "<screen %lldx%lld>", (long long) rows,
            (long long) cols);
  if (ts_value_make_handle (ret, s, screen_free, desc,
                            &screen_tag_id) != TS_OK)
    {
      screen_free (s);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
s_put (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  int64_t r, c;
  const char *text;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 4, 4, error, "put") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "put");
  if (!s || ts_std_int (&argv[1], &r, error, "put") != TS_OK ||
      ts_std_int (&argv[2], &c, error, "put") != TS_OK ||
      ts_std_str (&argv[3], &text, error, "put") != TS_OK)
    return TS_ERR_INVAL;
  if (r < 0 || (size_t) r >= s->rows || c < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  for (i = 0; text[i] && (size_t) c + i < s->cols; i++)
    s->cells[(size_t) r * s->cols + (size_t) c + i] = text[i];
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
s_clear (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "clear") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "clear");
  if (!s)
    return TS_ERR_INVAL;
  memset (s->cells, ' ', s->rows * s->cols);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_fill (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  const char *ch;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "fill") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "fill");
  if (!s || ts_std_str (&argv[1], &ch, error, "fill") != TS_OK)
    return TS_ERR_INVAL;
  if (!ch[0])
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "fill: empty char");
      return TS_ERR_INVAL;
    }
  memset (s->cells, ch[0], s->rows * s->cols);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_text (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  ts_sbuf_t b;
  size_t r;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "text") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "text");
  if (!s)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (r = 0; r < s->rows; r++)
    {
      /* Trim trailing blanks per row. */
      size_t end = s->cols;
      while (end > 0 && s->cells[r * s->cols + end - 1] == ' ')
        end--;
      if (ts_sbuf_put (&b, s->cells + r * s->cols, end) != 0 ||
          ts_sbuf_ch (&b, '\n') != 0)
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
s_line (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  int64_t r;
  size_t end;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "line") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "line");
  if (!s || ts_std_int (&argv[1], &r, error, "line") != TS_OK)
    return TS_ERR_INVAL;
  if (r < 0 || (size_t) r >= s->rows)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  end = s->cols;
  while (end > 0 && s->cells[(size_t) r * s->cols + end - 1] == ' ')
    end--;
  return ts_std_ret_strn (ret, s->cells + (size_t) r * s->cols, end,
                          error);
}

static TS_Status
s_rows (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "rows") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "rows");
  if (!s)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) s->rows);
  return TS_OK;
}

static TS_Status
s_cols (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  screen_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "cols") != TS_OK)
    return TS_ERR_INVAL;
  s = screen_unwrap (&argv[0], error, "cols");
  if (!s)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) s->cols);
  return TS_OK;
}

static const TS_FuncDef screen_funcs[] = {
  { "new", s_new, NULL },
  { "put", s_put, NULL },
  { "clear", s_clear, NULL },
  { "fill", s_fill, NULL },
  { "text", s_text, NULL },
  { "line", s_line, NULL },
  { "rows", s_rows, NULL },
  { "cols", s_cols, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_screen_module = { "std.screen", screen_funcs };
