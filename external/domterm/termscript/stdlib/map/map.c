/* map.c -- std.map: string-keyed value tables.
 *
 * A linear store (documented O(n)) is plenty for configuration-style
 * maps; ordering is insertion order, which keeps keys() stable.
 */
#include "map/map.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  char **keys;
  TS_Value *vals;
  size_t n, cap;
} map_t;

static const char map_tag_id = 0;

static void
map_free (void *p)
{
  map_t *m = p;
  size_t i;
  if (!m)
    return;
  for (i = 0; i < m->n; i++)
    {
      free (m->keys[i]);
      ts_value_free (&m->vals[i]);
    }
  free (m->keys);
  free (m->vals);
  free (m);
}

static map_t *
map_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &map_tag_id, error, what);
}

static TS_Status
map_wrap (TS_Value *ret, map_t *m, TS_Error *error)
{
  char desc[64];
  snprintf (desc, sizeof desc, "<map len=%zu>", m->n);
  if (ts_value_make_handle (ret, m, map_free, desc, &map_tag_id) != TS_OK)
    {
      map_free (m);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static long
map_find (const map_t *m, const char *key)
{
  size_t i;
  for (i = 0; i < m->n; i++)
    if (strcmp (m->keys[i], key) == 0)
      return (long) i;
  return -1;
}

static TS_Status
m_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  map_t *m;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  m = calloc (1, sizeof *m);
  if (!m)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return map_wrap (ret, m, error);
}

static TS_Status
m_set (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  map_t *m;
  const char *key;
  long at;
  TS_Value tmp;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  m = map_unwrap (&argv[0], error, "set");
  if (!m || ts_std_str (&argv[1], &key, error, "set") != TS_OK)
    return TS_ERR_INVAL;
  at = map_find (m, key);
  tmp.type = TS_NIL;
  if (ts_value_copy (&tmp, &argv[2]) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (at >= 0)
    {
      ts_value_free (&m->vals[at]);
      m->vals[at] = tmp;
      ts_std_ret_bool (ret, true);
      return TS_OK;
    }
  if (m->n == m->cap)
    {
      size_t nc = m->cap ? m->cap * 2 : 8;
      char **nk = realloc (m->keys, nc * sizeof *nk);
      TS_Value *nv = realloc (m->vals, nc * sizeof *nv);
      if (!nk || !nv)
        {
          free (nk);
          free (nv);
          ts_value_free (&tmp);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      m->keys = nk;
      m->vals = nv;
      m->cap = nc;
    }
  m->keys[m->n] = strdup (key);
  if (!m->keys[m->n])
    {
      ts_value_free (&tmp);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  m->vals[m->n] = tmp;
  m->n++;
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
m_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  map_t *m;
  const char *key;
  long at;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  m = map_unwrap (&argv[0], error, "get");
  if (!m || ts_std_str (&argv[1], &key, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  at = map_find (m, key);
  if (at < 0)
    {
      /* Optional third argument is the default. */
      if (argc == 3)
        {
          if (ts_value_copy (ret, &argv[2]) != TS_OK)
            {
              ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
          return TS_OK;
        }
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  if (ts_value_copy (ret, &m->vals[at]) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
m_has (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  map_t *m;
  const char *key;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "has") != TS_OK)
    return TS_ERR_INVAL;
  m = map_unwrap (&argv[0], error, "has");
  if (!m || ts_std_str (&argv[1], &key, error, "has") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, map_find (m, key) >= 0);
  return TS_OK;
}

static TS_Status
m_del (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  map_t *m;
  const char *key;
  long at;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "del") != TS_OK)
    return TS_ERR_INVAL;
  m = map_unwrap (&argv[0], error, "del");
  if (!m || ts_std_str (&argv[1], &key, error, "del") != TS_OK)
    return TS_ERR_INVAL;
  at = map_find (m, key);
  if (at < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  free (m->keys[at]);
  ts_value_free (&m->vals[at]);
  m->keys[at] = m->keys[m->n - 1];
  m->vals[at] = m->vals[m->n - 1];
  m->n--;
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
m_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  map_t *m;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "len") != TS_OK)
    return TS_ERR_INVAL;
  m = map_unwrap (&argv[0], error, "len");
  if (!m)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) m->n);
  return TS_OK;
}

static TS_Status
m_keys (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  map_t *m;
  const char *sep = ",";
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "keys") != TS_OK)
    return TS_ERR_INVAL;
  m = map_unwrap (&argv[0], error, "keys");
  if (!m)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_str (&argv[1], &sep, error, "keys") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (i = 0; i < m->n; i++)
    {
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, m->keys[i]) != 0)
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

static const TS_FuncDef map_funcs[] = {
  { "new", m_new, NULL },
  { "set", m_set, NULL },
  { "get", m_get, NULL },
  { "has", m_has, NULL },
  { "del", m_del, NULL },
  { "len", m_len, NULL },
  { "keys", m_keys, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_map_module = { "std.map", map_funcs };
