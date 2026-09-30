/* test.c -- std.test: counting checks for Termscript test scripts.
 *
 * new() opens a group, check()/eq() record outcomes, summary()
 * renders "name: pass=X fail=Y", done() is true when nothing failed.
 * Failures never abort: the script decides via done().
 */
#include "test/test.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  char name[128];
  long pass;
  long fail;
} test_t;

static const char test_tag_id = 0;

static test_t *
test_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &test_tag_id, error, what);
}

static TS_Status
t_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *name = "test";
  test_t *t;
  char desc[160];
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 1, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &name, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  t = calloc (1, sizeof *t);
  if (!t)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  snprintf (t->name, sizeof t->name, "%s", name);
  snprintf (desc, sizeof desc, "<test %s>", t->name);
  if (ts_value_make_handle (ret, t, free, desc, &test_tag_id) != TS_OK)
    {
      free (t);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
t_check (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  test_t *t;
  const char *msg = "check";
  bool cond;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "check") != TS_OK)
    return TS_ERR_INVAL;
  t = test_unwrap (&argv[0], error, "check");
  if (!t)
    return TS_ERR_INVAL;
  cond = ts_value_truthy (&argv[1]);
  if (argc == 3 && ts_std_str (&argv[2], &msg, error, "check") != TS_OK)
    return TS_ERR_INVAL;
  if (cond)
    t->pass++;
  else
    {
      t->fail++;
      fprintf (stderr, "FAIL [%s]: %s\n", t->name, msg);
    }
  ts_std_ret_bool (ret, cond);
  return TS_OK;
}

static bool
test_eq (const TS_Value *a, const TS_Value *b)
{
  if (a->type != b->type)
    return false;
  switch (a->type)
    {
    case TS_NIL:
      return true;
    case TS_BOOL:
      return a->as.boolean == b->as.boolean;
    case TS_INT:
      return a->as.integer == b->as.integer;
    case TS_STRING:
      return strcmp (a->as.string ? a->as.string : "",
                     b->as.string ? b->as.string : "") == 0;
    case TS_HANDLE:
      return a->as.handle == b->as.handle;
    }
  return false;
}

static TS_Status
t_eq (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
      TS_Value *ret, TS_Error *error)
{
  test_t *t;
  const char *msg = "eq";
  bool ok;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 4, error, "eq") != TS_OK)
    return TS_ERR_INVAL;
  t = test_unwrap (&argv[0], error, "eq");
  if (!t)
    return TS_ERR_INVAL;
  if (argc == 4 && ts_std_str (&argv[3], &msg, error, "eq") != TS_OK)
    return TS_ERR_INVAL;
  ok = test_eq (&argv[1], &argv[2]);
  if (ok)
    t->pass++;
  else
    {
      t->fail++;
      fprintf (stderr, "FAIL [%s]: %s\n", t->name, msg);
    }
  ts_std_ret_bool (ret, ok);
  return TS_OK;
}

static TS_Status
t_summary (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  test_t *t;
  char buf[320];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "summary") != TS_OK)
    return TS_ERR_INVAL;
  t = test_unwrap (&argv[0], error, "summary");
  if (!t)
    return TS_ERR_INVAL;
  snprintf (buf, sizeof buf, "%s: pass=%ld fail=%ld", t->name, t->pass,
            t->fail);
  return ts_std_ret_str (ret, buf, error);
}

static TS_Status
t_done (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  test_t *t;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "done") != TS_OK)
    return TS_ERR_INVAL;
  t = test_unwrap (&argv[0], error, "done");
  if (!t)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, t->fail == 0);
  return TS_OK;
}

static const TS_FuncDef test_funcs[] = {
  { "new", t_new, NULL },
  { "check", t_check, NULL },
  { "eq", t_eq, NULL },
  { "summary", t_summary, NULL },
  { "done", t_done, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_test_module = { "std.test", test_funcs };
