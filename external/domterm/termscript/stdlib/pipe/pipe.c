/* pipe.c -- std.pipe: popen command streams as handles.
 *
 * open() starts the command, read() drains (nil at EOF), write()
 * feeds stdin, close() reaps and reports the exit code.
 */
#include "pipe/pipe.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

typedef struct
{
  FILE *f;
  bool writable;
} pipe_t;

static const char pipe_tag_id = 0;

static void
pipe_free (void *p)
{
  pipe_t *h = p;
  if (!h)
    return;
  if (h->f)
    pclose (h->f);
  free (h);
}

static pipe_t *
pipe_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  pipe_t *h = ts_std_handle (v, &pipe_tag_id, error, what);
  if (h && !h->f)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: closed pipe", what);
      return NULL;
    }
  return h;
}

static TS_Status
p_open (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *cmd, *mode = "r";
  pipe_t *h;
  char desc[96];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "open") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "open") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_str (&argv[1], &mode, error, "open") != TS_OK)
    return TS_ERR_INVAL;
  if (strcmp (mode, "r") != 0 && strcmp (mode, "w") != 0)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "open: mode must be r or w");
      return TS_ERR_INVAL;
    }
  h = calloc (1, sizeof *h);
  if (!h)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  h->f = popen (cmd, mode);
  if (!h->f)
    {
      free (h);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  h->writable = mode[0] == 'w';
  snprintf (desc, sizeof desc, "<pipe %s>", mode);
  if (ts_value_make_handle (ret, h, pipe_free, desc, &pipe_tag_id) !=
      TS_OK)
    {
      pipe_free (h);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
p_read (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pipe_t *h;
  int64_t max = 65536;
  char *buf;
  size_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  h = pipe_unwrap (&argv[0], error, "read");
  if (!h)
    return TS_ERR_INVAL;
  if (h->writable)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "read: write-mode pipe");
      return TS_ERR_INVAL;
    }
  if (argc == 2 && ts_std_int (&argv[1], &max, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  if (max < 1 || max > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "read: bad size");
      return TS_ERR_INVAL;
    }
  buf = malloc ((size_t) max + 1);
  if (!buf)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  r = fread (buf, 1, (size_t) max, h->f);
  if (!r)
    {
      free (buf);
      ts_std_ret_nil (ret); /* EOF or error: nil. */
      return TS_OK;
    }
  buf[r] = '\0';
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = buf;
  return TS_OK;
}

static TS_Status
p_write (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pipe_t *h;
  const char *text;
  size_t n;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  h = pipe_unwrap (&argv[0], error, "write");
  if (!h)
    return TS_ERR_INVAL;
  if (!h->writable)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "write: read-mode pipe");
      return TS_ERR_INVAL;
    }
  if (ts_std_str (&argv[1], &text, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (text);
  ts_std_ret_bool (ret, !n || fwrite (text, 1, n, h->f) == n);
  return TS_OK;
}

static int
pipe_decode (int rc)
{
  if (WIFEXITED (rc))
    return WEXITSTATUS (rc);
  if (WIFSIGNALED (rc))
    return 128 + WTERMSIG (rc);
  return 127;
}

static TS_Status
p_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pipe_t *h;
  int rc;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  h = ts_std_handle (&argv[0], &pipe_tag_id, error, "close");
  if (!h || !h->f)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "close: bad handle");
      return TS_ERR_INVAL;
    }
  rc = pclose (h->f);
  h->f = NULL;
  ts_std_ret_int (ret, (int64_t) pipe_decode (rc));
  return TS_OK;
}

static const TS_FuncDef pipe_funcs[] = {
  { "open", p_open, NULL },
  { "read", p_read, NULL },
  { "write", p_write, NULL },
  { "close", p_close, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_pipe_module = { "std.pipe", pipe_funcs };
