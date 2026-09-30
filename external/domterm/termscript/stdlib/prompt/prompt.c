/* prompt.c -- std.prompt: reusable primary and continuation prompts.
 *
 * Prompt strings may contain ANSI SGR sequences or Readline's \001/\002
 * invisible delimiters.  width() ignores both, so callers can keep cursor
 * layout correct before handing the rendered primary string to std.readline.
 */
#include "prompt/prompt.h"

#include "common/ts_std_common.h"

#include <stdlib.h>
#include <string.h>

typedef struct
{
  char *primary;
  char *continuation;
} prompt_t;

static const char prompt_tag;

static TS_Status
oom (TS_Error *error)
{
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static void
prompt_free (void *payload)
{
  prompt_t *prompt = payload;
  if (!prompt)
    return;
  free (prompt->primary);
  free (prompt->continuation);
  free (prompt);
}

static prompt_t *
get_prompt (const TS_Value *value, TS_Error *error, const char *name)
{
  return ts_std_handle (value, &prompt_tag, error, name);
}

static size_t
visible_width (const char *text)
{
  size_t width = 0;
  const unsigned char *p = (const unsigned char *) text;
  while (*p)
    {
      if (*p == '\001' || *p == '\002')
        {
          p++;
          continue;
        }
      if (*p == 0x1b && p[1])
        {
          p++;
          if (*p == '[')
            {
              p++;
              while (*p && !(*p >= '@' && *p <= '~'))
                p++;
              if (*p)
                p++;
            }
          else
            p++;
          continue;
        }
      width++;
      p++;
    }
  return width;
}

static TS_Status
p_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *primary;
  const char *continuation = "> ";
  prompt_t *prompt;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &primary, error, "new") != TS_OK ||
      (argc == 2 && ts_std_str (&argv[1], &continuation, error, "new") != TS_OK))
    return TS_ERR_INVAL;
  prompt = calloc (1, sizeof *prompt);
  if (!prompt)
    return oom (error);
  prompt->primary = strdup (primary);
  prompt->continuation = strdup (continuation);
  if (!prompt->primary || !prompt->continuation)
    {
      prompt_free (prompt);
      return oom (error);
    }
  if (ts_value_make_handle (ret, prompt, prompt_free, "<prompt>",
                            &prompt_tag) != TS_OK)
    {
      prompt_free (prompt);
      return oom (error);
    }
  return TS_OK;
}

static TS_Status
p_render (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  prompt_t *prompt;
  bool continuation = false;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "render") != TS_OK)
    return TS_ERR_INVAL;
  prompt = get_prompt (&argv[0], error, "render");
  if (!prompt)
    return TS_ERR_INVAL;
  if (argc == 2)
    {
      if (argv[1].type != TS_BOOL)
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0,
                        "render: bool required");
          return TS_ERR_INVAL;
        }
      continuation = argv[1].as.boolean;
    }
  return ts_std_ret_str (ret, continuation ? prompt->continuation : prompt->primary,
                         error);
}

static TS_Status
p_set (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  prompt_t *prompt;
  const char *text;
  char *copy;
  bool continuation = ud != NULL;
  const char *name = continuation ? "set_continuation" : "set_primary";
  if (ts_std_argc (vm, argc, 2, 2, error, name) != TS_OK)
    return TS_ERR_INVAL;
  prompt = get_prompt (&argv[0], error, name);
  if (!prompt || ts_std_str (&argv[1], &text, error, name) != TS_OK)
    return TS_ERR_INVAL;
  copy = strdup (text);
  if (!copy)
    return oom (error);
  if (continuation)
    {
      free (prompt->continuation);
      prompt->continuation = copy;
    }
  else
    {
      free (prompt->primary);
      prompt->primary = copy;
    }
  return ts_std_ret_str (ret, copy, error);
}

static TS_Status
p_width (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *text;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "width") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &text, error, "width") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) visible_width (text));
  return TS_OK;
}

static const TS_FuncDef prompt_funcs[] = {
  { "new", p_new, NULL },
  { "render", p_render, NULL },
  { "set_primary", p_set, NULL },
  { "set_continuation", p_set, (void *) 1 },
  { "width", p_width, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_prompt_module = { "std.prompt", prompt_funcs };
