/* exec.c -- std.exec: child processes via the shell.
 *
 * system() reports the exit code (127 + signal encoding matches the
 * shell convention), capture() returns stdout, status() the code of
 * any command.  Quoting belongs to sh_escape().
 */
#include "exec/exec.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static int
decode_status (int rc)
{
  if (WIFEXITED (rc))
    return WEXITSTATUS (rc);
  if (WIFSIGNALED (rc))
    return 128 + WTERMSIG (rc);
  return 127;
}

static TS_Status
e_system (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  int rc;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "system") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "system") != TS_OK)
    return TS_ERR_INVAL;
  rc = system (cmd);
  if (rc == -1)
    {
      ts_error_set (error, TS_ERR_SYSTEM, 0, 0, "system: fork failed");
      return TS_ERR_SYSTEM;
    }
  ts_std_ret_int (ret, (int64_t) decode_status (rc));
  return TS_OK;
}

static TS_Status
e_capture (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  FILE *f;
  ts_sbuf_t b;
  char chunk[4096];
  size_t r;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "capture") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "capture") != TS_OK)
    return TS_ERR_INVAL;
  f = popen (cmd, "r");
  if (!f)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  ts_sbuf_init (&b);
  while ((r = fread (chunk, 1, sizeof chunk, f)) > 0)
    if (ts_sbuf_put (&b, chunk, r) != 0)
      {
        pclose (f);
        ts_sbuf_free (&b);
        ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
        return TS_ERR_NOMEM;
      }
  pclose (f);
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
e_ok (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
      TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  int rc;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "ok") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "ok") != TS_OK)
    return TS_ERR_INVAL;
  rc = system (cmd);
  if (rc == -1)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, decode_status (rc) == 0);
  return TS_OK;
}

static TS_Status
e_sh_escape (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
             TS_Value *ret, TS_Error *error)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "sh_escape") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "sh_escape") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (ts_sbuf_ch (&b, '\'') != 0)
    goto oom;
  for (; *s; s++)
    {
      if (*s == '\'')
        {
          if (ts_sbuf_str (&b, "'\\''") != 0)
            goto oom;
        }
      else if (ts_sbuf_ch (&b, *s) != 0)
        goto oom;
    }
  if (ts_sbuf_ch (&b, '\'') != 0)
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

static const TS_FuncDef exec_funcs[] = {
  { "system", e_system, NULL },
  { "capture", e_capture, NULL },
  { "ok", e_ok, NULL },
  { "sh_escape", e_sh_escape, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_exec_module = { "std.exec", exec_funcs };
