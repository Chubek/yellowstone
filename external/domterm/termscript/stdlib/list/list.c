/* list.c -- std.list: ordered sequences of Termscript values.
 *
 * The Termscript side (list.tsc) binds the module and spells Lazily
 * batch idioms (folds via while loops live in scripts); this file owns
 * storage (the shared ts_std_list_t store) and the primitives.
 */
#include "list/list.h"

#include "common/ts_std_common.h"

#include <stdlib.h>

static TS_Status
l_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_new ();
  if (!l)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return ts_std_list_wrap (ret, l, error);
}

static TS_Status
l_push (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "push") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "push");
  if (!l)
    return TS_ERR_INVAL;
  if (ts_std_list_push (l, &argv[1]) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ts_std_ret_int (ret, (int64_t) ts_std_list_len (l));
  return TS_OK;
}

static TS_Status
l_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  int64_t i;
  const TS_Value *v;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "get");
  if (!l || ts_std_int (&argv[1], &i, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  v = i >= 0 ? ts_std_list_get (l, (size_t) i) : NULL;
  if (!v)
    {
      ts_std_ret_nil (ret); /* Out of range is nil, not an error. */
      return TS_OK;
    }
  if (ts_value_copy (ret, v) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
l_set (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  int64_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "set");
  if (!l || ts_std_int (&argv[1], &i, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  if (i < 0 || ts_std_list_set (l, (size_t) i, &argv[2]) != TS_OK)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
l_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "len") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "len");
  if (!l)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) ts_std_list_len (l));
  return TS_OK;
}

static TS_Status
l_pop (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  const TS_Value *v;
  size_t n;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "pop") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "pop");
  if (!l)
    return TS_ERR_INVAL;
  n = ts_std_list_len (l);
  if (!n)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  v = ts_std_list_get (l, n - 1);
  if (ts_value_copy (ret, v) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ts_std_list_pop (l);
  return TS_OK;
}

static TS_Status
l_join (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  const char *sep;
  ts_sbuf_t b;
  size_t i, n;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "join") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "join");
  if (!l || ts_std_str (&argv[1], &sep, error, "join") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  n = ts_std_list_len (l);
  for (i = 0; i < n; i++)
    {
      char *s = ts_std_to_str (ts_std_list_get (l, i), error);
      if (!s)
        {
          ts_sbuf_free (&b);
          return TS_ERR_NOMEM;
        }
      if (i && ts_sbuf_str (&b, sep) != 0)
        {
          free (s);
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      if (ts_sbuf_str (&b, s) != 0)
        {
          free (s);
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      free (s);
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out ? out : NULL;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
l_clear (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  ts_std_list_t *l;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "clear") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_unwrap (&argv[0], error, "clear");
  if (!l)
    return TS_ERR_INVAL;
  ts_std_list_clear (l);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef list_funcs[] = {
  { "new", l_new, NULL },
  { "push", l_push, NULL },
  { "get", l_get, NULL },
  { "set", l_set, NULL },
  { "len", l_len, NULL },
  { "pop", l_pop, NULL },
  { "join", l_join, NULL },
  { "clear", l_clear, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_list_module = { "std.list", list_funcs };
