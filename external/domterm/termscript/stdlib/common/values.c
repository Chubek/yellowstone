/* values.c -- argument getters and return makers for std.* modules. */
#include "common/ts_std_common.h"

#include <stdlib.h>
#include <string.h>

TS_Status
ts_std_str (const TS_Value *v, const char **out, TS_Error *error,
            const char *what)
{
  if (!v || v->type != TS_STRING)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: string required",
                    what ? what : "arg");
      return TS_ERR_INVAL;
    }
  *out = v->as.string ? v->as.string : "";
  return TS_OK;
}

TS_Status
ts_std_int (const TS_Value *v, int64_t *out, TS_Error *error,
            const char *what)
{
  if (!v || v->type != TS_INT)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: int required",
                    what ? what : "arg");
      return TS_ERR_INVAL;
    }
  *out = v->as.integer;
  return TS_OK;
}

void *
ts_std_handle (const TS_Value *v, const void *tag, TS_Error *error,
               const char *what)
{
  void *p;
  if (!v || v->type != TS_HANDLE)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: handle required",
                    what ? what : "arg");
      return NULL;
    }
  p = ts_value_handle (v, tag);
  if (!p)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: wrong handle type",
                    what ? what : "arg");
      return NULL;
    }
  return p;
}

TS_Status
ts_std_ret_str (TS_Value *ret, const char *s, TS_Error *error)
{
  if (ts_value_make_string (ret, s ? s : "") != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

TS_Status
ts_std_ret_strn (TS_Value *ret, const char *s, size_t n, TS_Error *error)
{
  char *copy = malloc (n + 1);
  if (!copy)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  memcpy (copy, s ? s : "", n);
  copy[n] = '\0';
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = copy;
  return TS_OK;
}

void
ts_std_ret_int (TS_Value *ret, int64_t i)
{
  ts_value_make_int (ret, i);
}

void
ts_std_ret_bool (TS_Value *ret, bool b)
{
  ts_value_make_bool (ret, b);
}

void
ts_std_ret_nil (TS_Value *ret)
{
  ts_value_make_nil (ret);
}

TS_Status
ts_std_argc (TS_VM *vm, size_t argc, size_t lo, size_t hi, TS_Error *error,
             const char *fname)
{
  (void) vm;
  if (argc < lo || argc > hi)
    {
      if (lo == hi)
        ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s wants %zu args",
                      fname, lo);
      else
        ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s wants %zu..%zu args",
                      fname, lo, hi);
      return TS_ERR_INVAL;
    }
  return TS_OK;
}

char *
ts_std_to_str (const TS_Value *v, TS_Error *error)
{
  char *out = NULL;
  if (ts_value_to_string (v, &out, error) != TS_OK)
    return NULL;
  return out;
}

void
ts_std_free_cstr (char **p)
{
  if (p)
    free (*p);
}
