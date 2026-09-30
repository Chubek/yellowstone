/* terminfo.c -- std.tinfo: standalone-safe terminal strings.
 *
 * libtermscript_standalone cannot link libdomterm, so this is NOT the
 * full terminfo database (that is std.terminfo in the DT adapter).  It
 * is a curated capability table for common xterm-style strings plus
 * live window-size queries and small builders (cup/sgr/clear).
 */
#include "terminfo/terminfo.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct
{
  const char *name;
  const char *value;
} cap_entry_t;

static const cap_entry_t cap_table[] = {
  { "clear", "\x1b[H\x1b[2J" },
  { "home", "\x1b[H" },
  { "smcup", "\x1b[?1049h" },
  { "rmcup", "\x1b[?1049l" },
  { "bold", "\x1b[1m" },
  { "sgr0", "\x1b[0m" },
  { "rev", "\x1b[7m" },
  { "cud1", "\x1b[B" },
  { "cuf1", "\x1b[C" },
  { "cub1", "\x1b[D" },
  { "cuu1", "\x1b[A" },
  { "el", "\x1b[K" },
  { "ed", "\x1b[J" },
  { "civis", "\x1b[?25l" },
  { "cnorm", "\x1b[?25h" },
  { "bel", "\a" },
  { NULL, NULL }
};

static TS_Status
t_cap (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *name;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "cap") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &name, error, "cap") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; cap_table[i].name; i++)
    if (strcmp (name, cap_table[i].name) == 0)
      return ts_std_ret_str (ret, cap_table[i].value, error);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
t_has (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *name;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "has") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &name, error, "has") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; cap_table[i].name; i++)
    if (strcmp (name, cap_table[i].name) == 0)
      {
        ts_std_ret_bool (ret, true);
        return TS_OK;
      }
  ts_std_ret_bool (ret, false);
  return TS_OK;
}

static TS_Status
t_cup (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  int64_t r, c;
  char seq[48];
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "cup") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &r, error, "cup") != TS_OK ||
      ts_std_int (&argv[1], &c, error, "cup") != TS_OK)
    return TS_ERR_INVAL;
  if (r < 0 || c < 0 || r > 9999 || c > 9999)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "cup: bad position");
      return TS_ERR_INVAL;
    }
  snprintf (seq, sizeof seq, "\x1b[%lld;%lldH", (long long) r + 1,
            (long long) c + 1);
  return ts_std_ret_str (ret, seq, error);
}

static TS_Status
t_names (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *sep = ",";
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 1, error, "names") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &sep, error, "names") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (i = 0; cap_table[i].name; i++)
    {
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, cap_table[i].name) != 0)
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
winsize_query (TS_VM *vm, size_t argc, TS_Value *ret, TS_Error *error,
               const char *fname, bool rows)
{
  struct winsize ws;
  if (ts_std_argc (vm, argc, 0, 0, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ioctl (STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || !ws.ws_col ||
      !ws.ws_row)
    {
      /* Not a terminal (pipes, tests): documented 80x24 fallback. */
      ts_std_ret_int (ret, rows ? 24 : 80);
      return TS_OK;
    }
  ts_std_ret_int (ret, rows ? ws.ws_row : ws.ws_col);
  return TS_OK;
}

static TS_Status
t_rows (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void) ud;
  (void) a;
  return winsize_query (v, n, r, e, "rows", true);
}

static TS_Status
t_cols (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void) ud;
  (void) a;
  return winsize_query (v, n, r, e, "cols", false);
}

static const TS_FuncDef terminfo_funcs[] = {
  { "cap", t_cap, NULL },
  { "has", t_has, NULL },
  { "cup", t_cup, NULL },
  { "names", t_names, NULL },
  { "rows", t_rows, NULL },
  { "cols", t_cols, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_terminfo_module = { "std.tinfo", terminfo_funcs };
