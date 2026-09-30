/* notify.c -- std.notify: attention signals.
 *
 * bell() and title() return escape strings for scripts to print;
 * flash() wraps text in blink; toast() tries notify-send(1) and
 * reports whether a daemon accepted the message.
 */
#include "notify/notify.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static TS_Status
n_bell (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "bell") != TS_OK)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, "\a", error);
}

static TS_Status
n_title (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "title") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "title") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (ts_sbuf_str (&b, "\x1b]0;") != 0 || ts_sbuf_str (&b, text) != 0 ||
      ts_sbuf_ch (&b, '\a') != 0)
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

static TS_Status
n_flash (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "flash") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "flash") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  if (ts_sbuf_str (&b, "\x1b[5m") != 0 || ts_sbuf_str (&b, text) != 0 ||
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

static TS_Status
n_toast (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *title, *msg;
  ts_sbuf_t cmd;
  int rc;
  bool ok;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "toast") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &title, error, "toast") != TS_OK ||
      ts_std_str (&argv[1], &msg, error, "toast") != TS_OK)
    return TS_ERR_INVAL;
  /* Quote both arguments for the shell (single-quote style). */
  ts_sbuf_init (&cmd);
  if (ts_sbuf_str (&cmd, "notify-send '") != 0)
    goto oom;
  for (; *title; title++)
    {
      if (*title == '\'')
        {
          if (ts_sbuf_str (&cmd, "'\\''") != 0)
            goto oom;
        }
      else if (ts_sbuf_ch (&cmd, *title) != 0)
        goto oom;
    }
  if (ts_sbuf_str (&cmd, "' '") != 0)
    goto oom;
  for (; *msg; msg++)
    {
      if (*msg == '\'')
        {
          if (ts_sbuf_str (&cmd, "'\\''") != 0)
            goto oom;
        }
      else if (ts_sbuf_ch (&cmd, *msg) != 0)
        goto oom;
    }
  if (ts_sbuf_str (&cmd, "' >/dev/null 2>&1") != 0)
    goto oom;
  rc = system (cmd.data ? cmd.data : "false");
  ts_sbuf_free (&cmd);
  ok = rc != -1 && WIFEXITED (rc) && WEXITSTATUS (rc) == 0;
  ts_std_ret_bool (ret, ok);
  return TS_OK;
oom:
  ts_sbuf_free (&cmd);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static const TS_FuncDef notify_funcs[] = {
  { "bell", n_bell, NULL },
  { "title", n_title, NULL },
  { "flash", n_flash, NULL },
  { "toast", n_toast, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_notify_module = { "std.notify", notify_funcs };
