/* json.c -- std.json: JSON documents as handles.
 *
 * Parsing and the document model live in common/dom.c; this module is
 * the Termscript surface (parse/get/type/len/keys/stringify).  The
 * Termscript companion (json.tsc) binds the module and spells the
 * load-or-die idiom once.
 */
#include "json/json.h"

#include "common/ts_std_common.h"

static TS_Status
j_parse (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  ts_dom_t *dom = NULL;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_dom_parse_json (text, &dom, error) != TS_OK)
    {
      /* Malformed input is nil so `or` fallbacks work. */
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return ts_dom_wrap (ret, dom, error);
}

static TS_Status
j_type (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  ts_dom_t *d;
  const char *t;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "type") != TS_OK)
    return TS_ERR_INVAL;
  /* Scalars arrive as plain values; only containers are handles. */
  if (argv[0].type != TS_HANDLE)
    {
      switch (argv[0].type)
        {
        case TS_NIL: t = "null"; break;
        case TS_BOOL: t = "bool"; break;
        case TS_INT: t = "int"; break;
        default: t = "string"; break;
        }
      return ts_std_ret_str (ret, t, error);
    }
  d = ts_dom_unwrap (&argv[0], error, "type");
  if (!d)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, ts_dom_kind_str (d), error);
}

static TS_Status
j_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_dom_t *d;
  const char *path;
  const ts_dom_t *hit;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  d = ts_dom_unwrap (&argv[0], error, "get");
  if (!d || ts_std_str (&argv[1], &path, error, "get") != TS_OK)
    return TS_ERR_INVAL;
  hit = ts_dom_path (d, path);
  if (!hit)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return ts_dom_to_value (hit, ret, error);
}

static TS_Status
j_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  ts_dom_t *d;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "len") != TS_OK)
    return TS_ERR_INVAL;
  d = ts_dom_unwrap (&argv[0], error, "len");
  if (!d)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) ts_dom_len (d));
  return TS_OK;
}

static TS_Status
j_stringify (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
             TS_Value *ret, TS_Error *error)
{
  ts_dom_t *d;
  char *out = NULL;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "stringify") != TS_OK)
    return TS_ERR_INVAL;
  d = ts_dom_unwrap (&argv[0], error, "stringify");
  if (!d)
    return TS_ERR_INVAL;
  if (ts_dom_stringify (d, &out) != TS_OK)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  return TS_OK;
}

static const TS_FuncDef json_funcs[] = {
  { "parse", j_parse, NULL },
  { "type", j_type, NULL },
  { "get", j_get, NULL },
  { "len", j_len, NULL },
  { "stringify", j_stringify, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_json_module = { "std.json", json_funcs };
