/* menu.c -- std.menu: numbered menus over strings.
 *
 * format() renders, pick() parses a 1-based answer, confirm()
 * accepts y/yes, select() reads a line from stdin itself.
 */
#include "menu/menu.h"

#include "common/ts_std_common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TS_Status
split_items (const char *items, const char *sep, ts_std_list_t *out,
             TS_Error *error)
{
  const char *p = items ? items : "";
  size_t slen = strlen (sep);
  if (!slen)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "menu: empty separator");
      return TS_ERR_INVAL;
    }
  for (;;)
    {
      const char *hit = strstr (p, sep);
      TS_Value v;
      v.type = TS_NIL;
      if (ts_std_ret_strn (&v, p, hit ? (size_t) (hit - p) : strlen (p),
                           error) != TS_OK ||
          ts_std_list_push (out, &v) != TS_OK)
        {
          ts_value_free (&v);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      ts_value_free (&v);
      if (!hit)
        break;
      p = hit + slen;
    }
  return TS_OK;
}

static TS_Status
m_format (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *items, *sep = "\n", *title = "";
  ts_std_list_t *l;
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 3, error, "format") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &items, error, "format") != TS_OK)
    return TS_ERR_INVAL;
  if (argc >= 2 && ts_std_str (&argv[1], &sep, error, "format") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_str (&argv[2], &title, error, "format") !=
      TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_new ();
  if (!l)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (split_items (items, sep, l, error) != TS_OK)
    {
      ts_std_list_free (l);
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  if (title[0] && ts_sbuf_printf (&b, "%s\n", title) != 0)
    goto oom;
  for (i = 0; i < ts_std_list_len (l); i++)
    {
      char *s = ts_std_to_str (ts_std_list_get (l, i), error);
      if (!s)
        {
          ts_std_list_free (l);
          ts_sbuf_free (&b);
          return TS_ERR_NOMEM;
        }
      if (ts_sbuf_printf (&b, "  %zu) %s\n", i + 1, s) != 0)
        {
          free (s);
          ts_std_list_free (l);
          goto oom;
        }
      free (s);
    }
  ts_std_list_free (l);
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
m_pick (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  int64_t count, choice;
  const char *answer;
  char *end = NULL;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "pick") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &count, error, "pick") != TS_OK ||
      ts_std_str (&argv[1], &answer, error, "pick") != TS_OK)
    return TS_ERR_INVAL;
  while (*answer && isspace ((unsigned char) *answer))
    answer++;
  choice = strtol (answer, &end, 10);
  if (end == answer || choice < 1 || choice > count)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  ts_std_ret_int (ret, choice - 1);
  return TS_OK;
}

static TS_Status
m_confirm (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *answer;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "confirm") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &answer, error, "confirm") != TS_OK)
    return TS_ERR_INVAL;
  while (*answer && isspace ((unsigned char) *answer))
    answer++;
  ts_std_ret_bool (ret, answer[0] == 'y' || answer[0] == 'Y');
  return TS_OK;
}

static TS_Status
m_select (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *prompt, *items, *sep = "\n";
  ts_std_list_t *l;
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  long choice;
  char *end = NULL;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "select") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &prompt, error, "select") != TS_OK ||
      ts_std_str (&argv[1], &items, error, "select") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_str (&argv[2], &sep, error, "select") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_new ();
  if (!l)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  if (split_items (items, sep, l, error) != TS_OK)
    {
      ts_std_list_free (l);
      return TS_ERR_INVAL;
    }
  fputs (prompt, stdout);
  fputs (" ", stdout);
  fflush (stdout);
  n = getline (&line, &cap, stdin);
  if (n < 0)
    {
      free (line);
      ts_std_list_free (l);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  choice = strtol (line, &end, 10);
  free (line);
  if (end == line || choice < 1 ||
      (size_t) choice > ts_std_list_len (l))
    {
      ts_std_list_free (l);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  ts_std_ret_int (ret, choice - 1);
  ts_std_list_free (l);
  return TS_OK;
}

static const TS_FuncDef menu_funcs[] = {
  { "format", m_format, NULL },
  { "pick", m_pick, NULL },
  { "confirm", m_confirm, NULL },
  { "select", m_select, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_menu_module = { "std.menu", menu_funcs };
