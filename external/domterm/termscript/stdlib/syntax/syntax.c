/* syntax.c -- std.syntax: naive keyword highlighting.
 *
 * Word-boundary keyword scans per language with string/comment
 * elision; output wraps hits in yellow SGR.  A teaching-size
 * highlighter, not a parser — documented as such.
 */
#include "syntax/syntax.h"

#include "common/ts_std_common.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *const kw_c[] = {
  "auto", "break", "case", "char", "const", "continue", "default", "do",
  "double", "else", "enum", "extern", "float", "for", "goto", "if",
  "inline", "int", "long", "register", "restrict", "return", "short",
  "signed", "sizeof", "static", "struct", "switch", "typedef", "union",
  "unsigned", "void", "volatile", "while", "_Bool", NULL
};

static const char *const kw_ts[] = {
  "const", "while", "if", "do", "end", "else", "or", "true", "false",
  "nil", NULL
};

static const char *const kw_sh[] = {
  "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
  "done", "case", "esac", "in", "function", "return", "exit", "export",
  "local", "readonly", "shift", "break", "continue", "exec", NULL
};

typedef struct
{
  const char *lang;
  const char *const *words;
  char line_comment; /* 0, '#', or '/' for // */
} lang_def_t;

static const lang_def_t lang_defs[] = {
  { "c", kw_c, '/' },
  { "termscript", kw_ts, '#' },
  { "sh", kw_sh, '#' },
  { NULL, NULL, 0 }
};

static bool
is_keyword (const char *const *words, const char *w, size_t n)
{
  size_t i;
  for (i = 0; words[i]; i++)
    if (strlen (words[i]) == n && memcmp (words[i], w, n) == 0)
      return true;
  return false;
}

static bool
is_wordch (char c)
{
  return isalnum ((unsigned char) c) || c == '_';
}

static TS_Status
s_highlight (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
             TS_Value *ret, TS_Error *error)
{
  const char *lang, *src;
  const lang_def_t *def = NULL;
  size_t i;
  ts_sbuf_t b;
  char *out;
  const char *p;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "highlight") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &lang, error, "highlight") != TS_OK ||
      ts_std_str (&argv[1], &src, error, "highlight") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; lang_defs[i].lang; i++)
    if (strcmp (lang, lang_defs[i].lang) == 0)
      def = &lang_defs[i];
  if (!def)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "highlight: unknown language '%s'", lang);
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  p = src;
  while (*p)
    {
      /* Elide double-quoted strings verbatim. */
      if (*p == '"')
        {
          const char *q = p + 1;
          if (ts_sbuf_ch (&b, *p++) != 0)
            goto oom;
          while (*q && *q != '"' && *q != '\n')
            {
              if (*q == '\\' && q[1])
                {
                  if (ts_sbuf_ch (&b, *q++) != 0)
                    goto oom;
                }
              if (ts_sbuf_ch (&b, *q++) != 0)
                goto oom;
            }
          if (*q == '"' && ts_sbuf_ch (&b, *q++) != 0)
            goto oom;
          p = q;
          continue;
        }
      /* Elide line comments verbatim. */
      if ((def->line_comment == '#' && *p == '#') ||
          (def->line_comment == '/' && p[0] == '/' && p[1] == '/'))
        {
          while (*p && *p != '\n')
            if (ts_sbuf_ch (&b, *p++) != 0)
              goto oom;
          continue;
        }
      if (is_wordch (*p) && (p == src || !is_wordch (p[-1])))
        {
          const char *w = p;
          while (is_wordch (*p))
            p++;
          if (is_keyword (def->words, w, (size_t) (p - w)))
            {
              if (ts_sbuf_str (&b, "\x1b[33m") != 0 ||
                  ts_sbuf_put (&b, w, (size_t) (p - w)) != 0 ||
                  ts_sbuf_str (&b, "\x1b[0m") != 0)
                goto oom;
              continue;
            }
          if (ts_sbuf_put (&b, w, (size_t) (p - w)) != 0)
            goto oom;
          continue;
        }
      if (ts_sbuf_ch (&b, *p++) != 0)
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
s_langs (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *sep = ",";
  ts_sbuf_t b;
  size_t i;
  char *out;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 1, error, "langs") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 1 && ts_std_str (&argv[0], &sep, error, "langs") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (i = 0; lang_defs[i].lang; i++)
    {
      if (i && ts_sbuf_str (&b, sep) != 0)
        goto oom;
      if (ts_sbuf_str (&b, lang_defs[i].lang) != 0)
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

TS_Status
ts_std_syntax_highlight (const char *language, const char *source,
                         char **out, TS_Error *error)
{
  TS_Value argv[2];
  TS_Value ret;
  TS_Status status;
  if (!out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "highlight: bad output");
      return TS_ERR_INVAL;
    }
  *out = NULL;
  argv[0].type = TS_STRING;
  argv[0].as.string = (char *) (language ? language : "");
  argv[1].type = TS_STRING;
  argv[1].as.string = (char *) (source ? source : "");
  ret.type = TS_NIL;
  status = s_highlight (NULL, NULL, argv, 2, &ret, error);
  if (status != TS_OK)
    return status;
  *out = ret.as.string;
  ret.type = TS_NIL; /* Transfer the string to the C caller. */
  return TS_OK;
}

static TS_Status
s_keyword (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *lang, *word;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "keyword") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &lang, error, "keyword") != TS_OK ||
      ts_std_str (&argv[1], &word, error, "keyword") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; lang_defs[i].lang; i++)
    if (strcmp (lang, lang_defs[i].lang) == 0)
      {
        ts_std_ret_bool (ret,
                         is_keyword (lang_defs[i].words, word,
                                     strlen (word)));
        return TS_OK;
      }
  ts_error_set (error, TS_ERR_INVAL, 0, 0,
                "keyword: unknown language '%s'", lang);
  return TS_ERR_INVAL;
}

static const TS_FuncDef syntax_funcs[] = {
  { "highlight", s_highlight, NULL },
  { "langs", s_langs, NULL },
  { "keyword", s_keyword, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_syntax_module = { "std.syntax", syntax_funcs };
