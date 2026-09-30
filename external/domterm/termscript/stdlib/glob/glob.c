/* glob.c -- std.glob: shell pattern matching over libc fnmatch.
 *
 * match() is pure matching; glob() lists directory entries whose
 * basename matches (no recursive ** by design).
 */
#include "glob/glob.h"

#include "common/ts_std_common.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

static TS_Status
g_match (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *pat, *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "match") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "match") != TS_OK ||
      ts_std_str (&argv[1], &s, error, "match") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_bool (ret, fnmatch (pat, s, 0) == 0);
  return TS_OK;
}

static TS_Status
g_glob (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *pat;
  const char *slash;
  char dir[1024], base[1024];
  DIR *dp;
  struct dirent *de;
  ts_std_list_t *l;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "glob") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &pat, error, "glob") != TS_OK)
    return TS_ERR_INVAL;
  slash = strrchr (pat, '/');
  if (!slash)
    {
      snprintf (dir, sizeof dir, ".");
      snprintf (base, sizeof base, "%s", pat);
    }
  else
    {
      size_t n = (size_t) (slash - pat);
      if (!n)
        snprintf (dir, sizeof dir, "/");
      else if (n >= sizeof dir)
        {
          ts_error_set (error, TS_ERR_INVAL, 0, 0, "glob: path too long");
          return TS_ERR_INVAL;
        }
      else
        {
          memcpy (dir, pat, n);
          dir[n] = '\0';
        }
      snprintf (base, sizeof base, "%s", slash + 1);
    }
  l = ts_std_list_new ();
  if (!l)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  dp = opendir (dir);
  if (!dp)
    {
      /* Unreadable directory: empty result, not an error. */
      return ts_std_list_wrap (ret, l, error);
    }
  while ((de = readdir (dp)) != NULL)
    {
      if (strcmp (de->d_name, ".") == 0 || strcmp (de->d_name, "..") == 0)
        continue;
      if (de->d_name[0] == '.' && base[0] != '.')
        continue;
      if (fnmatch (base, de->d_name, 0) == 0)
        {
          char full[2048];
          if (strcmp (dir, ".") == 0)
            snprintf (full, sizeof full, "%s", de->d_name);
          else
            snprintf (full, sizeof full, "%s/%s", dir, de->d_name);
          if (ts_std_list_push_str (l, full) != TS_OK)
            {
              closedir (dp);
              ts_std_list_free (l);
              ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
        }
    }
  closedir (dp);
  return ts_std_list_wrap (ret, l, error);
}

static TS_Status
g_quote (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "quote") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "quote") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      if (*s == '*' || *s == '?' || *s == '[' || *s == '\\')
        if (ts_sbuf_ch (&b, '\\') != 0)
          goto oom;
      if (ts_sbuf_ch (&b, *s) != 0)
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

static const TS_FuncDef glob_funcs[] = {
  { "match", g_match, NULL },
  { "glob", g_glob, NULL },
  { "quote", g_quote, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_glob_module = { "std.glob", glob_funcs };
