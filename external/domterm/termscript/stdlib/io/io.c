/* io.c -- std.io: whole-file and line I/O over libc.
 *
 * Errors read as nil (missing file) or false (failed write) so
 * scripts use `or` fallbacks instead of catching.
 */
#include "io/io.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *
io_slurp (FILE *f, TS_Error *error)
{
  ts_sbuf_t b;
  char chunk[8192];
  size_t r;
  ts_sbuf_init (&b);
  while ((r = fread (chunk, 1, sizeof chunk, f)) > 0)
    {
      if (ts_sbuf_put (&b, chunk, r) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return NULL;
        }
      if (b.len > (1u << 20))
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_LIMIT, 0, 0, "file too large");
          return NULL;
        }
    }
  return ts_sbuf_take (&b);
}

static TS_Status
i_read (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *path;
  FILE *f;
  char *text;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  f = fopen (path, "rb");
  if (!f)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  text = io_slurp (f, error);
  fclose (f);
  if (!text)
    return error ? error->code : TS_ERR_NOMEM;
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = text;
  return TS_OK;
}

static TS_Status
io_write_mode (TS_VM *vm, const TS_Value *argv, size_t argc,
               TS_Value *ret, TS_Error *error, const char *fname,
               const char *mode)
{
  const char *path, *text;
  FILE *f;
  size_t n;
  if (ts_std_argc (vm, argc, 2, 2, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, fname) != TS_OK ||
      ts_std_str (&argv[1], &text, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  f = fopen (path, mode);
  if (!f)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  n = strlen (text);
  if (n && fwrite (text, 1, n, f) != n)
    {
      fclose (f);
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, fclose (f) == 0);
  return TS_OK;
}

static TS_Status
i_write (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return io_write_mode (v, a, n, r, e, "write", "wb");
}

static TS_Status
i_append (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
          TS_Error *e)
{
  (void) ud;
  return io_write_mode (v, a, n, r, e, "append", "ab");
}

static TS_Status
i_exists (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *path;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "exists") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "exists") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, access (path, F_OK) == 0);
  return TS_OK;
}

static TS_Status
i_size (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *path;
  struct stat st;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "size") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "size") != TS_OK)
    return TS_ERR_INVAL;
  if (stat (path, &st) != 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  ts_std_ret_int (ret, (int64_t) st.st_size);
  return TS_OK;
}

static TS_Status
i_remove (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *path;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "remove") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "remove") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, remove (path) == 0);
  return TS_OK;
}

static TS_Status
i_rename (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *from, *to;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "rename") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &from, error, "rename") != TS_OK ||
      ts_std_str (&argv[1], &to, error, "rename") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, rename (from, to) == 0);
  return TS_OK;
}

static TS_Status
i_mkdir (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *path;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "mkdir") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "mkdir") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, mkdir (path, 0777) == 0 || errno == EEXIST);
  return TS_OK;
}

static TS_Status
i_lines (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *path;
  FILE *f;
  char *text;
  ts_std_list_t *l;
  const char *p;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "lines") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "lines") != TS_OK)
    return TS_ERR_INVAL;
  f = fopen (path, "rb");
  if (!f)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  text = io_slurp (f, error);
  fclose (f);
  if (!text)
    return error ? error->code : TS_ERR_NOMEM;
  l = ts_std_list_new ();
  if (!l)
    {
      free (text);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  for (p = text;;)
    {
      const char *nl = strchr (p, '\n');
      TS_Value v;
      v.type = TS_NIL;
      if (ts_std_ret_strn (&v, p, nl ? (size_t) (nl - p) : strlen (p),
                           error) != TS_OK ||
          ts_std_list_push (l, &v) != TS_OK)
        {
          ts_value_free (&v);
          free (text);
          ts_std_list_free (l);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      ts_value_free (&v);
      if (!nl)
        break;
      p = nl + 1;
    }
  free (text);
  return ts_std_list_wrap (ret, l, error);
}

static TS_Status
i_stderr (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *text;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "stderr") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "stderr") != TS_OK)
    return TS_ERR_INVAL;
  fputs (text, stderr);
  fputc ('\n', stderr);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef io_funcs[] = {
  { "read", i_read, NULL },
  { "write", i_write, NULL },
  { "append", i_append, NULL },
  { "exists", i_exists, NULL },
  { "size", i_size, NULL },
  { "remove", i_remove, NULL },
  { "rename", i_rename, NULL },
  { "mkdir", i_mkdir, NULL },
  { "lines", i_lines, NULL },
  { "stderr", i_stderr, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_io_module = { "std.io", io_funcs };
