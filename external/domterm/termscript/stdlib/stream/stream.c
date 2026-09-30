/* stream.c -- std.stream: append-only byte buffers with line reads.
 *
 * The primitive for building output incrementally (report bodies,
 * generated configs) before a single write/commit elsewhere.
 */
#include "stream/stream.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  ts_sbuf_t buf;
  size_t cursor; /* Consumed prefix for getline. */
} stream_t;

static const char stream_tag_id = 0;

static void
stream_free (void *p)
{
  stream_t *s = p;
  if (!s)
    return;
  ts_sbuf_free (&s->buf);
  free (s);
}

static stream_t *
stream_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &stream_tag_id, error, what);
}

static TS_Status
stream_wrap (TS_Value *ret, stream_t *s, TS_Error *error)
{
  char desc[64];
  snprintf (desc, sizeof desc, "<stream len=%zu>", s->buf.len);
  if (ts_value_make_handle (ret, s, stream_free, desc,
                            &stream_tag_id) != TS_OK)
    {
      stream_free (s);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
s_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 1, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  s = calloc (1, sizeof *s);
  if (!s)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ts_sbuf_init (&s->buf);
  if (argc == 1)
    {
      const char *init;
      if (ts_std_str (&argv[0], &init, error, "new") != TS_OK ||
          ts_sbuf_str (&s->buf, init) != 0)
        {
          stream_free (s);
          if (!error || error->code == TS_OK)
            ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  return stream_wrap (ret, s, error);
}

static TS_Status
s_write (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  char *text = NULL;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 99, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  s = stream_unwrap (&argv[0], error, "write");
  if (!s)
    return TS_ERR_INVAL;
  for (i = 1; i < argc; i++)
    {
      text = ts_std_to_str (&argv[i], error);
      if (!text)
        return TS_ERR_NOMEM;
      if (ts_sbuf_str (&s->buf, text) != 0)
        {
          free (text);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      free (text);
    }
  ts_std_ret_int (ret, (int64_t) s->buf.len);
  return TS_OK;
}

static TS_Status
s_line (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  const char *text;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "line") != TS_OK)
    return TS_ERR_INVAL;
  s = stream_unwrap (&argv[0], error, "line");
  if (!s || ts_std_str (&argv[1], &text, error, "line") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_sbuf_str (&s->buf, text) != 0 || ts_sbuf_ch (&s->buf, '\n') != 0)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_contents (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "contents") != TS_OK)
    return TS_ERR_INVAL;
  s = stream_unwrap (&argv[0], error, "contents");
  if (!s)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, s->buf.data ? s->buf.data : "", error);
}

static TS_Status
s_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "len") != TS_OK)
    return TS_ERR_INVAL;
  s = stream_unwrap (&argv[0], error, "len");
  if (!s)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) (s->buf.len - s->cursor));
  return TS_OK;
}

static TS_Status
s_getline (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  const char *base;
  const char *nl;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "getline") != TS_OK)
    return TS_ERR_INVAL;
  s = stream_unwrap (&argv[0], error, "getline");
  if (!s)
    return TS_ERR_INVAL;
  base = (s->buf.data ? s->buf.data : "") + s->cursor;
  if (s->cursor >= s->buf.len)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  nl = strchr (base, '\n');
  if (!nl)
    {
      TS_Status st = ts_std_ret_str (ret, base, error);
      s->cursor = s->buf.len;
      return st;
    }
  {
    TS_Status st = ts_std_ret_strn (ret, base, (size_t) (nl - base), error);
    s->cursor = (size_t) (nl - s->buf.data) + 1;
    return st;
  }
}

static TS_Status
s_clear (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  stream_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "clear") != TS_OK)
    return TS_ERR_INVAL;
  s = stream_unwrap (&argv[0], error, "clear");
  if (!s)
    return TS_ERR_INVAL;
  ts_sbuf_clear (&s->buf);
  s->cursor = 0;
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef stream_funcs[] = {
  { "new", s_new, NULL },
  { "write", s_write, NULL },
  { "line", s_line, NULL },
  { "contents", s_contents, NULL },
  { "len", s_len, NULL },
  { "getline", s_getline, NULL },
  { "clear", s_clear, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_stream_module = { "std.stream", stream_funcs };
