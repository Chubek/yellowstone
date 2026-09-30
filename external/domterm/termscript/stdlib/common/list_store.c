/* list_store.c -- shared TS_Value sequence for the stdlib.
 *
 * Storage is a Q-generated ts_valvec_t (see q/tsvec.q, gen/ts_valvec.h):
 * domqlib's Q supplies the growable-array machinery, this file adds
 * Termscript value semantics (copy on push/set, shared handle tag).
 */
#include "common/ts_std_common.h"
#include "gen/ts_valvec.h"

#include <stdio.h>
#include <stdlib.h>

struct ts_std_list
{
  ts_valvec_t vec;
};

static const char list_tag_id = 0;

const void *
ts_std_list_tag (void)
{
  return &list_tag_id;
}

ts_std_list_t *
ts_std_list_new (void)
{
  ts_std_list_t *l = calloc (1, sizeof *l);
  if (l)
    ts_valvec_init (&l->vec);
  return l;
}

void
ts_std_list_free (void *p)
{
  ts_std_list_t *l = p;
  if (!l)
    return;
  ts_valvec_free (&l->vec);
  free (l);
}

TS_Status
ts_std_list_push (ts_std_list_t *l, const TS_Value *v)
{
  TS_Value *slot;
  if (!l || !v)
    return TS_ERR_INVAL;
  slot = ts_valvec_add (&l->vec);
  if (!slot)
    return TS_ERR_NOMEM;
  if (ts_value_copy (slot, v) != TS_OK)
    {
      l->vec.len--;
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

TS_Status
ts_std_list_push_str (ts_std_list_t *l, const char *s)
{
  TS_Value v;
  TS_Status st;
  v.type = TS_NIL;
  if (ts_value_make_string (&v, s ? s : "") != TS_OK)
    return TS_ERR_NOMEM;
  st = ts_std_list_push (l, &v);
  ts_value_free (&v);
  return st;
}

TS_Status
ts_std_list_push_int (ts_std_list_t *l, int64_t i)
{
  TS_Value v;
  v.type = TS_INT;
  v.as.integer = i;
  return ts_std_list_push (l, &v);
}

size_t
ts_std_list_len (const ts_std_list_t *l)
{
  return l ? ts_valvec_len (&((ts_std_list_t *) l)->vec) : 0;
}

const TS_Value *
ts_std_list_get (const ts_std_list_t *l, size_t i)
{
  if (!l)
    return NULL;
  return ts_valvec_get (&((ts_std_list_t *) l)->vec, i);
}

TS_Status
ts_std_list_set (ts_std_list_t *l, size_t i, const TS_Value *v)
{
  TS_Value *slot;
  TS_Value tmp;
  if (!l || !v)
    return TS_ERR_INVAL;
  slot = ts_valvec_get (&l->vec, i);
  if (!slot)
    return TS_ERR_NOTFOUND;
  tmp.type = TS_NIL;
  if (ts_value_copy (&tmp, v) != TS_OK)
    return TS_ERR_NOMEM;
  ts_value_free (slot);
  *slot = tmp;
  return TS_OK;
}

void
ts_std_list_pop (ts_std_list_t *l)
{
  if (l)
    ts_valvec_pop (&l->vec);
}

void
ts_std_list_clear (ts_std_list_t *l)
{
  if (l)
    ts_valvec_clear (&l->vec);
}

TS_Status
ts_std_list_wrap (TS_Value *ret, ts_std_list_t *l, TS_Error *error)
{
  char desc[64];
  if (!l)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad list");
      return TS_ERR_INVAL;
    }
  snprintf (desc, sizeof desc, "<list len=%zu>", ts_std_list_len (l));
  if (ts_value_make_handle (ret, l, ts_std_list_free, desc,
                            &list_tag_id) != TS_OK)
    {
      ts_std_list_free (l);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

ts_std_list_t *
ts_std_list_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &list_tag_id, error,
                        what ? what : "list handle");
}
