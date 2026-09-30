/* toml.c -- std.toml: TOML documents via vendored tomlc99.
 *
 * Parsing delegates to third_party/tomlc99 (compiled into
 * libtermscript_stdlib); the result is converted into the shared DOM
 * (common/dom.c) so get/type/len/stringify behave like std.json.
 */
#include "toml/toml.h"

#include "common/ts_std_common.h"

static TS_Status
t_parse (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  ts_dom_t *dom = NULL;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_dom_parse_toml (text, &dom, error) != TS_OK)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return ts_dom_wrap (ret, dom, error);
}

static TS_Status
t_type (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  ts_dom_t *d;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "type") != TS_OK)
    return TS_ERR_INVAL;
  if (argv[0].type != TS_HANDLE)
    {
      switch (argv[0].type)
        {
        case TS_NIL: return ts_std_ret_str (ret, "null", error);
        case TS_BOOL: return ts_std_ret_str (ret, "bool", error);
        case TS_INT: return ts_std_ret_str (ret, "int", error);
        default: return ts_std_ret_str (ret, "string", error);
        }
    }
  d = ts_dom_unwrap (&argv[0], error, "type");
  if (!d)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, ts_dom_kind_str (d), error);
}

static TS_Status
t_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
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
t_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
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
t_stringify (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
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
  /* Rendered as JSON (documented): a lossless-enough interchange view
   * of the TOML table. */
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

static const TS_FuncDef toml_funcs[] = {
  { "parse", t_parse, NULL },
  { "type", t_type, NULL },
  { "get", t_get, NULL },
  { "len", t_len, NULL },
  { "stringify", t_stringify, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_toml_module = { "std.toml", toml_funcs };
