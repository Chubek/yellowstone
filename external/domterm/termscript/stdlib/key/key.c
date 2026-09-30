/* key.c -- std.key: terminal input sequence names.
 *
 * parse() maps escape sequences and control bytes to symbolic names
 * ("up", "c-a", "f5", ...); encode() inverts the table.  Unknown
 * input parses to nil rather than erroring.
 */
#include "key/key.h"

#include "common/ts_std_common.h"

#include <string.h>

typedef struct
{
  const char *seq;
  const char *name;
} key_entry_t;

/* Ordered longest-first so prefix scans prefer full sequences. */
static const key_entry_t key_table[] = {
  { "\x1b[A", "up" }, { "\x1b[B", "down" }, { "\x1b[C", "right" },
  { "\x1b[D", "left" }, { "\x1b[H", "home" }, { "\x1b[F", "end" },
  { "\x1b[1~", "home" }, { "\x1b[2~", "insert" }, { "\x1b[3~", "delete" },
  { "\x1b[4~", "end" }, { "\x1b[5~", "pgup" }, { "\x1b[6~", "pgdn" },
  { "\x1b[7~", "home" }, { "\x1b[8~", "end" },
  { "\x1bOP", "f1" }, { "\x1bOQ", "f2" }, { "\x1bOR", "f3" },
  { "\x1bOS", "f4" }, { "\x1b[11~", "f1" }, { "\x1b[12~", "f2" },
  { "\x1b[13~", "f3" }, { "\x1b[14~", "f4" }, { "\x1b[15~", "f5" },
  { "\x1b[17~", "f6" }, { "\x1b[18~", "f7" }, { "\x1b[19~", "f8" },
  { "\x1b[20~", "f9" }, { "\x1b[21~", "f10" }, { "\x1b[23~", "f11" },
  { "\x1b[24~", "f12" },
  { "\x1b[1;5A", "c-up" }, { "\x1b[1;5B", "c-down" },
  { "\x1b[1;5C", "c-right" }, { "\x1b[1;5D", "c-left" },
  { "\r", "enter" }, { "\n", "enter" }, { "\t", "tab" },
  { "\x7f", "backspace" }, { "\x1b", "escape" }, { " ", "space" },
  { NULL, NULL }
};

static TS_Status
k_parse (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *s;
  size_t i;
  char single[8];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "parse") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; key_table[i].seq; i++)
    if (strcmp (s, key_table[i].seq) == 0)
      return ts_std_ret_str (ret, key_table[i].name, error);
  /* Printable singletons name themselves; C0 controls become c-x. */
  if (s[0] && !s[1])
    {
      unsigned char c = (unsigned char) s[0];
      if (c >= 0x20 && c < 0x7F)
        {
          single[0] = (char) c;
          single[1] = '\0';
          return ts_std_ret_str (ret, single, error);
        }
      if (c < 0x20)
        {
          single[0] = 'c';
          single[1] = '-';
          single[2] = (char) (c + 'a' - 1);
          single[3] = '\0';
          return ts_std_ret_str (ret, single, error);
        }
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
k_encode (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *name;
  size_t i;
  char single[2];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "encode") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &name, error, "encode") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; key_table[i].seq; i++)
    if (strcmp (name, key_table[i].name) == 0)
      return ts_std_ret_str (ret, key_table[i].seq, error);
  if (name[0] && !name[1])
    {
      single[0] = name[0];
      single[1] = '\0';
      return ts_std_ret_str (ret, single, error);
    }
  if (name[0] == 'c' && name[1] == '-' && name[2] && !name[3] &&
      name[2] >= 'a' && name[2] <= 'z')
    {
      single[0] = (char) (name[2] - 'a' + 1);
      single[1] = '\0';
      return ts_std_ret_str (ret, single, error);
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
k_is_esc (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "is_esc") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "is_esc") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, s[0] == '\x1b');
  return TS_OK;
}

static const TS_FuncDef key_funcs[] = {
  { "parse", k_parse, NULL },
  { "encode", k_encode, NULL },
  { "is_esc", k_is_esc, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_key_module = { "std.key", key_funcs };
