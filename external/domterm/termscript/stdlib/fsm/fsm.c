/* fsm.c -- std.fsm: table-driven finite state machines.
 *
 * new() binds the start state; rule() adds from/event/to rows;
 * send() fires the first matching row (nil when no row matches);
 * reset() jumps back explicitly.
 */
#include "fsm/fsm.h"

#include "common/ts_std_common.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
  char *from;
  char *event;
  char *to;
} rule_t;

typedef struct
{
  char *state;
  char *initial;
  rule_t *rules;
  size_t n, cap;
} fsm_t;

static const char fsm_tag_id = 0;

static void
fsm_free (void *p)
{
  fsm_t *f = p;
  size_t i;
  if (!f)
    return;
  free (f->state);
  free (f->initial);
  for (i = 0; i < f->n; i++)
    {
      free (f->rules[i].from);
      free (f->rules[i].event);
      free (f->rules[i].to);
    }
  free (f->rules);
  free (f);
}

static fsm_t *
fsm_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &fsm_tag_id, error, what);
}

static TS_Status
f_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *initial;
  fsm_t *f;
  char desc[160];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &initial, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  f = calloc (1, sizeof *f);
  if (!f)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  f->state = strdup (initial);
  f->initial = strdup (initial);
  if (!f->state || !f->initial)
    {
      fsm_free (f);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  snprintf (desc, sizeof desc, "<fsm %s>", f->state);
  if (ts_value_make_handle (ret, f, fsm_free, desc, &fsm_tag_id) !=
      TS_OK)
    {
      fsm_free (f);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
f_rule (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  fsm_t *f;
  const char *from, *event, *to;
  rule_t *nv;
  (void) ud;
  if (ts_std_argc (vm, argc, 4, 4, error, "rule") != TS_OK)
    return TS_ERR_INVAL;
  f = fsm_unwrap (&argv[0], error, "rule");
  if (!f || ts_std_str (&argv[1], &from, error, "rule") != TS_OK ||
      ts_std_str (&argv[2], &event, error, "rule") != TS_OK ||
      ts_std_str (&argv[3], &to, error, "rule") != TS_OK)
    return TS_ERR_INVAL;
  if (f->n == f->cap)
    {
      size_t nc = f->cap ? f->cap * 2 : 8;
      nv = realloc (f->rules, nc * sizeof *nv);
      if (!nv)
        {
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      f->rules = nv;
      f->cap = nc;
    }
  f->rules[f->n].from = strdup (from);
  f->rules[f->n].event = strdup (event);
  f->rules[f->n].to = strdup (to);
  if (!f->rules[f->n].from || !f->rules[f->n].event ||
      !f->rules[f->n].to)
    {
      free (f->rules[f->n].from);
      free (f->rules[f->n].event);
      free (f->rules[f->n].to);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  f->n++;
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
f_send (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  fsm_t *f;
  const char *event;
  size_t i;
  char *next;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  f = fsm_unwrap (&argv[0], error, "send");
  if (!f || ts_std_str (&argv[1], &event, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; i < f->n; i++)
    if (strcmp (f->rules[i].from, f->state) == 0 &&
        strcmp (f->rules[i].event, event) == 0)
      {
        next = strdup (f->rules[i].to);
        if (!next)
          {
            ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
            return TS_ERR_NOMEM;
          }
        free (f->state);
        f->state = next;
        return ts_std_ret_str (ret, f->state, error);
      }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
f_state (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  fsm_t *f;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "state") != TS_OK)
    return TS_ERR_INVAL;
  f = fsm_unwrap (&argv[0], error, "state");
  if (!f)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, f->state, error);
}

static TS_Status
f_reset (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  fsm_t *f;
  const char *to = NULL;
  char *next;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "reset") != TS_OK)
    return TS_ERR_INVAL;
  f = fsm_unwrap (&argv[0], error, "reset");
  if (!f)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_str (&argv[1], &to, error, "reset") != TS_OK)
    return TS_ERR_INVAL;
  next = strdup (to ? to : f->initial);
  if (!next)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  free (f->state);
  f->state = next;
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef fsm_funcs[] = {
  { "new", f_new, NULL },
  { "rule", f_rule, NULL },
  { "send", f_send, NULL },
  { "state", f_state, NULL },
  { "reset", f_reset, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_fsm_module = { "std.fsm", fsm_funcs };
