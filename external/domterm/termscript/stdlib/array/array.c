/* array.c -- std.array: dense int64 vectors.
 *
 * Termscript ints are the element type, so no cross-type machinery is
 * needed; the Termscript side (array.tsc) adds reductions written as
 * plain while loops over get/len.
 */
#include "array/array.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  int64_t *data;
  size_t len, cap;
} array_t;

static const char array_tag_id = 0;

static void
array_free (void *p)
{
  array_t *a = p;
  if (!a)
    return;
  free (a->data);
  free (a);
}

static array_t *
array_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &array_tag_id, error, what);
}

static TS_Status
array_wrap (TS_Value *ret, array_t *a, TS_Error *error)
{
  char desc[64];
  snprintf (desc, sizeof desc, "<array len=%zu>", a->len);
  if (ts_value_make_handle (ret, a, array_free, desc,
                            &array_tag_id) != TS_OK)
    {
      array_free (a);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static int
array_grow (array_t *a, size_t want)
{
  if (want > a->cap)
    {
      size_t nc = a->cap ? a->cap * 2 : 8;
      int64_t *nv;
      while (nc < want)
        nc *= 2;
      nv = realloc (a->data, nc * sizeof *nv);
      if (!nv)
        return -1;
      a->data = nv;
      a->cap = nc;
    }
  return 0;
}

static TS_Status
a_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  array_t *a;
  int64_t n = 0, fill = 0;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 2, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc >= 1 && ts_std_int (&argv[0], &n, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc >= 2 && ts_std_int (&argv[1], &fill, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (n < 0 || n > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "new: bad size");
      return TS_ERR_INVAL;
    }
  a = calloc (1, sizeof *a);
  if (!a)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (array_grow (a, (size_t) n) != 0)
    {
      array_free (a);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  for (i = 0; i < (size_t) n; i++)
    a->data[i] = fill;
  a->len = (size_t) n;
  return array_wrap (ret, a, error);
}

static TS_Status
a_push (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  array_t *a;
  int64_t v;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "push") != TS_OK)
    return TS_ERR_INVAL;
  a = array_unwrap (&argv[0], error, "push");
  if (!a || ts_std_int (&argv[1], &v, error, "push") != TS_OK)
    return TS_ERR_INVAL;
  if (array_grow (a, a->len + 1) != 0)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  a->data[a->len++] = v;
  ts_std_ret_int (ret, (int64_t) a->len);
  return TS_OK;
}

static TS_Status
a_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  array_t *a;
  int64_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  a = array_unwrap (&argv[0], error, "get");
  if (!a || ts_std_int (&argv[1], &i, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  if (i < 0 || (size_t) i >= a->len)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  ts_std_ret_int (ret, a->data[i]);
  return TS_OK;
}

static TS_Status
a_set (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  array_t *a;
  int64_t i, v;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  a = array_unwrap (&argv[0], error, "set");
  if (!a || ts_std_int (&argv[1], &i, error, "set") != TS_OK ||
      ts_std_int (&argv[2], &v, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  if (i < 0 || (size_t) i >= a->len)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  a->data[i] = v;
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
a_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  array_t *a;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "len") != TS_OK)
    return TS_ERR_INVAL;
  a = array_unwrap (&argv[0], error, "len");
  if (!a)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) a->len);
  return TS_OK;
}

static TS_Status
a_sum (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  array_t *a;
  size_t i;
  int64_t total = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "sum") != TS_OK)
    return TS_ERR_INVAL;
  a = array_unwrap (&argv[0], error, "sum");
  if (!a)
    return TS_ERR_INVAL;
  for (i = 0; i < a->len; i++)
    total += a->data[i];
  ts_std_ret_int (ret, total);
  return TS_OK;
}

static TS_Status
a_fill (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  array_t *a;
  int64_t v;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "fill") != TS_OK)
    return TS_ERR_INVAL;
  a = array_unwrap (&argv[0], error, "fill");
  if (!a || ts_std_int (&argv[1], &v, error, "fill") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; i < a->len; i++)
    a->data[i] = v;
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef array_funcs[] = {
  { "new", a_new, NULL },
  { "push", a_push, NULL },
  { "get", a_get, NULL },
  { "set", a_set, NULL },
  { "len", a_len, NULL },
  { "sum", a_sum, NULL },
  { "fill", a_fill, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_array_module = { "std.array", array_funcs };
