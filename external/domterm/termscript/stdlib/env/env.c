/* env.c -- std.env: process environment access.
 *
 * get() reads nil for missing keys; list() renders K=V lines for
 * scripts to scan with std.codec:split.
 */
#include "env/env.h"

#include "common/ts_std_common.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

static TS_Status
e_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *key, *val;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &key, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  val = getenv (key);
  if (!val)
    {
      if (argc == 2)
        return ts_value_copy (ret, &argv[1]) == TS_OK ?
                 TS_OK : TS_ERR_NOMEM;
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return ts_std_ret_str (ret, val, error);
}

static TS_Status
e_set (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *key, *val;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &key, error, "set") != TS_OK ||
      ts_std_str (&argv[1], &val, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, setenv (key, val, 1) == 0);
  return TS_OK;
}

static TS_Status
e_unset (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *key;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "unset") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &key, error, "unset") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, unsetenv (key) == 0);
  return TS_OK;
}

static TS_Status
e_has (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *key;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "has") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &key, error, "has") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, getenv (key) != NULL);
  return TS_OK;
}

static TS_Status
e_list (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *sep = "\n";
  ts_sbuf_t b;
  char **e;
  char *out;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 1, error, "list") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &sep, error, "list") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (e = environ; e && *e; e++)
    {
      if (e != environ && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, *e) != 0)
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
e_home (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *h;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "home") != TS_OK)
    return TS_ERR_INVAL;
  h = getenv ("HOME");
  if (!h)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return ts_std_ret_str (ret, h, error);
}

static TS_Status
e_pid (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "pid") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) getpid ());
  return TS_OK;
}

static const TS_FuncDef env_funcs[] = {
  { "get", e_get, NULL },
  { "set", e_set, NULL },
  { "unset", e_unset, NULL },
  { "has", e_has, NULL },
  { "list", e_list, NULL },
  { "home", e_home, NULL },
  { "pid", e_pid, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_env_module = { "std.env", env_funcs };
