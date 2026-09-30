/* log.c -- std.log: leveled logging to files or stderr.
 *
 * Levels 0=debug < 1=info < 2=warn < 3=error; messages below the
 * handle's threshold are dropped.  Lines carry a wall-clock stamp.
 */
#include "log/log.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
  FILE *f;
  bool own;
  int level;
} log_t;

static const char log_tag_id = 0;

static void
log_free (void *p)
{
  log_t *l = p;
  if (!l)
    return;
  if (l->f && l->own)
    fclose (l->f);
  free (l);
}

static const char *
level_name (int lv)
{
  switch (lv)
    {
    case 0: return "DEBUG";
    case 1: return "INFO";
    case 2: return "WARN";
    default: return "ERROR";
    }
}

static void
log_emit (log_t *l, int lv, const char *msg)
{
  char stamp[32];
  time_t now = time (NULL);
  struct tm tm;
  localtime_r (&now, &tm);
  strftime (stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%S", &tm);
  fprintf (l->f, "%s %-5s %s\n", stamp, level_name (lv), msg);
  fflush (l->f);
}

static TS_Status
l_open (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *path;
  log_t *l;
  char desc[256];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "open") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "open") != TS_OK)
    return TS_ERR_INVAL;
  l = calloc (1, sizeof *l);
  if (!l)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  l->f = fopen (path, "a");
  if (!l->f)
    {
      free (l);
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  l->own = true;
  l->level = 1;
  snprintf (desc, sizeof desc, "<log %s>", path);
  if (ts_value_make_handle (ret, l, log_free, desc, &log_tag_id) !=
      TS_OK)
    {
      log_free (l);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
l_level (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  log_t *l;
  int64_t lv;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "level") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_handle (&argv[0], &log_tag_id, error, "level");
  if (!l || ts_std_int (&argv[1], &lv, error, "level") != TS_OK)
    return TS_ERR_INVAL;
  if (lv < 0 || lv > 3)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "level: 0..3 required");
      return TS_ERR_INVAL;
    }
  l->level = (int) lv;
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
log_at (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error, const char *fname, int lv)
{
  log_t *l;
  const char *msg;
  if (ts_std_argc (vm, argc, 2, 2, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_handle (&argv[0], &log_tag_id, error, fname);
  if (!l || ts_std_str (&argv[1], &msg, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (lv >= l->level)
    log_emit (l, lv, msg);
  ts_std_ret_bool (ret, lv >= l->level);
  return TS_OK;
}

static TS_Status
l_debug (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return log_at (v, a, n, r, e, "debug", 0);
}

static TS_Status
l_info (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void) ud;
  return log_at (v, a, n, r, e, "info", 1);
}

static TS_Status
l_warn (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void) ud;
  return log_at (v, a, n, r, e, "warn", 2);
}

static TS_Status
l_error (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return log_at (v, a, n, r, e, "error", 3);
}

static TS_Status
l_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  log_t *l;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_handle (&argv[0], &log_tag_id, error, "close");
  if (!l)
    return TS_ERR_INVAL;
  if (l->f && l->own)
    {
      fclose (l->f);
      l->f = NULL;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
stderr_at (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
           TS_Error *error, const char *fname, int lv)
{
  const char *msg;
  log_t tmp;
  if (ts_std_argc (vm, argc, 1, 1, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &msg, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  tmp.f = stderr;
  tmp.own = false;
  tmp.level = 0;
  log_emit (&tmp, lv, msg);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
l_sinfo (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return stderr_at (v, a, n, r, e, "sinfo", 1);
}

static TS_Status
l_swarn (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return stderr_at (v, a, n, r, e, "swarn", 2);
}

static TS_Status
l_serror (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
          TS_Error *e)
{
  (void) ud;
  return stderr_at (v, a, n, r, e, "serror", 3);
}

static const TS_FuncDef log_funcs[] = {
  { "open", l_open, NULL },
  { "level", l_level, NULL },
  { "debug", l_debug, NULL },
  { "info", l_info, NULL },
  { "warn", l_warn, NULL },
  { "error", l_error, NULL },
  { "close", l_close, NULL },
  { "sinfo", l_sinfo, NULL },
  { "swarn", l_swarn, NULL },
  { "serror", l_serror, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_log_module = { "std.log", log_funcs };
