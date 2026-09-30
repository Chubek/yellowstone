/* readline.c -- std.readline: interactive line input.
 *
 * When built with TS_STD_HAVE_ISOCLINE (third_party/isocline, single
 * umbrella source compiled into libtermscript_stdlib) line() offers
 * full editing and history; otherwise it falls back to plain getline
 * on stdin.  Either way the contract is identical: nil at EOF.
 */
#include "readline/readline.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TS_STD_HAVE_ISOCLINE
#include "isocline.h"
#endif

static TS_Status
r_line (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *prompt = "";
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 1, error, "line") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &prompt, error, "line") !=
      TS_OK)
    return TS_ERR_INVAL;
#ifdef TS_STD_HAVE_ISOCLINE
  {
    char *in = ic_readline (prompt);
    TS_Status st;
    if (!in)
      {
        ts_std_ret_nil (ret);
        return TS_OK;
      }
    st = ts_std_ret_str (ret, in, error);
    ic_free (in);
    return st;
  }
#else
  {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    fputs (prompt, stdout);
    fflush (stdout);
    n = getline (&line, &cap, stdin);
    if (n < 0)
      {
        free (line);
        ts_std_ret_nil (ret);
        return TS_OK;
      }
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
      line[--n] = '\0';
    ts_value_free (ret);
    ret->type = TS_STRING;
    ret->as.string = line;
    return TS_OK;
  }
#endif
}

static TS_Status
r_history (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *path;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "history") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "history") != TS_OK)
    return TS_ERR_INVAL;
#ifdef TS_STD_HAVE_ISOCLINE
  ic_set_history (path, 200);
  ts_std_ret_bool (ret, true);
#else
  (void) path;
  /* No persistent history without isocline (documented no-op). */
  ts_std_ret_bool (ret, false);
#endif
  return TS_OK;
}

static TS_Status
r_add_history (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
               TS_Value *ret, TS_Error *error)
{
  const char *entry;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "add_history") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &entry, error, "add_history") != TS_OK)
    return TS_ERR_INVAL;
#ifdef TS_STD_HAVE_ISOCLINE
  ic_history_add (entry);
  ts_std_ret_bool (ret, true);
#else
  (void) entry;
  ts_std_ret_bool (ret, false);
#endif
  return TS_OK;
}

static const TS_FuncDef readline_funcs[] = {
  { "line", r_line, NULL },
  { "history", r_history, NULL },
  { "add_history", r_add_history, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_readline_module = { "std.readline",
                                           readline_funcs };
