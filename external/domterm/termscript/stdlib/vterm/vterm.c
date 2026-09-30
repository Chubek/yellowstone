/* vterm.c -- std.vterm: a small VT parser over a character grid.
 *
 * feed() consumes bytes (printables, C0 controls, and the CSI
 * sequences CUP/CUU/CUD/CUF/CUB/ED/EL/SGR plus RI/IND); text()
 * dumps the grid; cursor() reports "row,col".  Attributes are
 * accepted and ignored (documented): the grid stores characters.
 */
#include "vterm/vterm.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  char *cells;
  size_t rows, cols;
  size_t cur_r, cur_c;
} vterm_t;

static const char vterm_tag_id = 0;

static void
vterm_free (void *p)
{
  vterm_t *v = p;
  if (!v)
    return;
  free (v->cells);
  free (v);
}

static vterm_t *
vterm_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &vterm_tag_id, error, what);
}

static void
vterm_scroll (vterm_t *v)
{
  memmove (v->cells, v->cells + v->cols, (v->rows - 1) * v->cols);
  memset (v->cells + (v->rows - 1) * v->cols, ' ', v->cols);
  if (v->cur_r > 0)
    v->cur_r--;
}

static void
vterm_putc (vterm_t *v, char c)
{
  if (v->cur_c >= v->cols)
    {
      v->cur_c = 0;
      v->cur_r++;
    }
  if (v->cur_r >= v->rows)
    vterm_scroll (v);
  v->cells[v->cur_r * v->cols + v->cur_c++] = c;
}

static void
vterm_newline (vterm_t *v)
{
  v->cur_c = 0;
  v->cur_r++;
  if (v->cur_r >= v->rows)
    vterm_scroll (v);
}

static long
csi_param (long *params, size_t n, size_t i, long dflt)
{
  if (i >= n || params[i] <= 0)
    return dflt;
  return params[i];
}

static void
vterm_csi (vterm_t *v, char final, long *params, size_t nparams)
{
  size_t r, c, k;
  long a, b;
  switch (final)
    {
    case 'A': /* CUU */
      a = csi_param (params, nparams, 0, 1);
      v->cur_r = a > (long) v->cur_r ? 0 : v->cur_r - (size_t) a;
      break;
    case 'B': /* CUD */
    case 'e':
      a = csi_param (params, nparams, 0, 1);
      v->cur_r += (size_t) a;
      if (v->cur_r >= v->rows)
        v->cur_r = v->rows - 1;
      break;
    case 'C': /* CUF */
    case 'a':
      a = csi_param (params, nparams, 0, 1);
      v->cur_c += (size_t) a;
      if (v->cur_c >= v->cols)
        v->cur_c = v->cols - 1;
      break;
    case 'D': /* CUB */
      a = csi_param (params, nparams, 0, 1);
      v->cur_c = a > (long) v->cur_c ? 0 : v->cur_c - (size_t) a;
      break;
    case 'E':
      a = csi_param (params, nparams, 0, 1);
      v->cur_c = 0;
      v->cur_r += (size_t) a;
      if (v->cur_r >= v->rows)
        v->cur_r = v->rows - 1;
      break;
    case 'F':
      a = csi_param (params, nparams, 0, 1);
      v->cur_c = 0;
      v->cur_r = a > (long) v->cur_r ? 0 : v->cur_r - (size_t) a;
      break;
    case 'G':
      a = csi_param (params, nparams, 0, 1);
      v->cur_c = a < 1 ? 0 : (size_t) (a - 1);
      if (v->cur_c >= v->cols)
        v->cur_c = v->cols - 1;
      break;
    case 'H': /* CUP */
    case 'f':
      a = csi_param (params, nparams, 0, 1);
      b = csi_param (params, nparams, 1, 1);
      v->cur_r = a < 1 ? 0 : (size_t) (a - 1);
      v->cur_c = b < 1 ? 0 : (size_t) (b - 1);
      if (v->cur_r >= v->rows)
        v->cur_r = v->rows - 1;
      if (v->cur_c >= v->cols)
        v->cur_c = v->cols - 1;
      break;
    case 'J': /* ED */
      a = csi_param (params, nparams, 0, 0);
      if (a == 2)
        {
          memset (v->cells, ' ', v->rows * v->cols);
          v->cur_r = 0;
          v->cur_c = 0;
        }
      else if (a == 0)
        {
          size_t at = v->cur_r * v->cols + v->cur_c;
          memset (v->cells + at, ' ', v->rows * v->cols - at);
        }
      else if (a == 1)
        {
          size_t at = v->cur_r * v->cols + v->cur_c + 1;
          memset (v->cells, ' ', at);
        }
      break;
    case 'K': /* EL */
      a = csi_param (params, nparams, 0, 0);
      if (a == 2 || a == 1)
        {
          size_t row = v->cur_r * v->cols;
          size_t end = a == 2 ? v->cols : v->cur_c + 1;
          if (end > v->cols)
            end = v->cols;
          memset (v->cells + row, ' ', end);
        }
      else
        {
          size_t row = v->cur_r * v->cols;
          memset (v->cells + row + v->cur_c, ' ',
                  v->cols - v->cur_c);
        }
      break;
    case 'd':
      a = csi_param (params, nparams, 0, 1);
      v->cur_r = a < 1 ? 0 : (size_t) (a - 1);
      if (v->cur_r >= v->rows)
        v->cur_r = v->rows - 1;
      break;
    case 'm': /* SGR: accepted, attributes not stored. */
      break;
    case 'L':
      a = csi_param (params, nparams, 0, 1);
      for (k = 0; k < (size_t) a && v->cur_r < v->rows; k++)
        {
          memmove (v->cells + (v->cur_r + 1) * v->cols,
                   v->cells + v->cur_r * v->cols,
                   (v->rows - v->cur_r - 1) * v->cols);
          memset (v->cells + v->cur_r * v->cols, ' ', v->cols);
        }
      break;
    case 'M':
      a = csi_param (params, nparams, 0, 1);
      for (k = 0; k < (size_t) a && v->cur_r < v->rows; k++)
        {
          memmove (v->cells + v->cur_r * v->cols,
                   v->cells + (v->cur_r + 1) * v->cols,
                   (v->rows - v->cur_r - 1) * v->cols);
          memset (v->cells + (v->rows - 1) * v->cols, ' ', v->cols);
        }
      break;
    default:
      break;
    }
  (void) r;
  (void) c;
}

static void
vterm_feed (vterm_t *v, const char *s)
{
  enum { GROUND, ESC, CSI } state = GROUND;
  long params[16];
  size_t nparams = 0;
  bool priv = false;
  for (; *s; s++)
    {
      unsigned char c = (unsigned char) *s;
      switch (state)
        {
        case GROUND:
          if (c == 0x1B)
            state = ESC;
          else if (c == '\n' || c == '\v' || c == '\f')
            vterm_newline (v);
          else if (c == '\r')
            v->cur_c = 0;
          else if (c == '\b')
            {
              if (v->cur_c > 0)
                v->cur_c--;
            }
          else if (c == '\t')
            v->cur_c = (v->cur_c + 8) & ~(size_t) 7;
          else if (c >= 0x20 && c < 0x7F)
            vterm_putc (v, (char) c);
          break;
        case ESC:
          if (c == '[')
            {
              state = CSI;
              nparams = 0;
              params[0] = 0;
              priv = false;
            }
          else if (c == 'c')
            {
              memset (v->cells, ' ', v->rows * v->cols);
              v->cur_r = 0;
              v->cur_c = 0;
              state = GROUND;
            }
          else if (c == 'M')
            {
              if (v->cur_r == 0)
                {
                  memmove (v->cells + v->cols, v->cells,
                           (v->rows - 1) * v->cols);
                  memset (v->cells, ' ', v->cols);
                }
              else
                v->cur_r--;
              state = GROUND;
            }
          else if (c == 'D' || c == 'E')
            {
              vterm_newline (v);
              state = GROUND;
            }
          else
            state = GROUND;
          break;
        case CSI:
          if (c == '?' && nparams == 0 && params[0] == 0)
            priv = true;
          else if (c >= '0' && c <= '9')
            params[nparams] = params[nparams] * 10 + (c - '0');
          else if (c == ';')
            {
              if (nparams + 1 < 16)
                params[++nparams] = 0;
            }
          else if (c >= '@' && c <= '~')
            {
              (void) priv;
              vterm_csi (v, (char) c, params, nparams + 1);
              state = GROUND;
            }
          else
            state = GROUND; /* Malformed: drop. */
          break;
        }
    }
}

static TS_Status
vt_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  int64_t rows = 24, cols = 80;
  vterm_t *v;
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
  v = calloc (1, sizeof *v);
  if (!v)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  v->cells = malloc ((size_t) rows * (size_t) cols);
  if (!v->cells)
    {
      free (v);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  memset (v->cells, ' ', (size_t) rows * (size_t) cols);
  v->rows = (size_t) rows;
  v->cols = (size_t) cols;
  snprintf (desc, sizeof desc, "<vterm %lldx%lld>", (long long) rows,
            (long long) cols);
  if (ts_value_make_handle (ret, v, vterm_free, desc,
                            &vterm_tag_id) != TS_OK)
    {
      vterm_free (v);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
vt_feed (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  const char *text;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "feed") != TS_OK)
    return TS_ERR_INVAL;
  v = vterm_unwrap (&argv[0], error, "feed");
  if (!v || ts_std_str (&argv[1], &text, error, "feed") != TS_OK)
    return TS_ERR_INVAL;
  vterm_feed (v, text);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
vt_text (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  ts_sbuf_t b;
  size_t r;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "text") != TS_OK)
    return TS_ERR_INVAL;
  v = vterm_unwrap (&argv[0], error, "text");
  if (!v)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (r = 0; r < v->rows; r++)
    {
      size_t end = v->cols;
      while (end > 0 && v->cells[r * v->cols + end - 1] == ' ')
        end--;
      if (ts_sbuf_put (&b, v->cells + r * v->cols, end) != 0 ||
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
vt_cursor (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  char buf[64];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "cursor") != TS_OK)
    return TS_ERR_INVAL;
  v = vterm_unwrap (&argv[0], error, "cursor");
  if (!v)
    return TS_ERR_INVAL;
  snprintf (buf, sizeof buf, "%zu,%zu", v->cur_r, v->cur_c);
  return ts_std_ret_str (ret, buf, error);
}

static TS_Status
vt_reset (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "reset") != TS_OK)
    return TS_ERR_INVAL;
  v = vterm_unwrap (&argv[0], error, "reset");
  if (!v)
    return TS_ERR_INVAL;
  memset (v->cells, ' ', v->rows * v->cols);
  v->cur_r = 0;
  v->cur_c = 0;
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef vterm_funcs[] = {
  { "new", vt_new, NULL },
  { "feed", vt_feed, NULL },
  { "text", vt_text, NULL },
  { "cursor", vt_cursor, NULL },
  { "reset", vt_reset, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_vterm_module = { "std.vterm", vterm_funcs };
