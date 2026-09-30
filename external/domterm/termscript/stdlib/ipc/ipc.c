/* ipc.c -- std.ipc: file locks and FIFOs.
 *
 * lock() takes an exclusive flock(2) (released by unlock() or when
 * the handle dies); fifo() creates a named pipe for use with
 * std.io:read/write from cooperating processes.
 */
#include "ipc/ipc.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
  int fd;
} lock_t;

static const char lock_tag_id = 0;

static void
lock_free (void *p)
{
  lock_t *l = p;
  if (!l)
    return;
  if (l->fd >= 0)
    {
      flock (l->fd, LOCK_UN);
      close (l->fd);
    }
  free (l);
}

static TS_Status
i_lock (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *path;
  int64_t wait_ms = 0;
  lock_t *l;
  int fd;
  char desc[256];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "lock") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "lock") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_int (&argv[1], &wait_ms, error, "lock") !=
      TS_OK)
    return TS_ERR_INVAL;
  fd = open (path, O_RDWR | O_CREAT, 0666);
  if (fd < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "lock: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (wait_ms <= 0)
    {
      if (flock (fd, LOCK_EX | LOCK_NB) != 0)
        {
          close (fd);
          ts_std_ret_nil (ret); /* Held elsewhere: nil. */
          return TS_OK;
        }
    }
  else
    {
      int64_t waited = 0;
      for (;;)
        {
          if (flock (fd, LOCK_EX | LOCK_NB) == 0)
            break;
          if (errno != EWOULDBLOCK || waited >= wait_ms)
            {
              close (fd);
              ts_std_ret_nil (ret);
              return TS_OK;
            }
          {
            struct timespec rq = { 0, 5000000L };
            nanosleep (&rq, NULL);
          }
          waited += 5;
        }
    }
  l = malloc (sizeof *l);
  if (!l)
    {
      flock (fd, LOCK_UN);
      close (fd);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  l->fd = fd;
  snprintf (desc, sizeof desc, "<lock %s>", path);
  if (ts_value_make_handle (ret, l, lock_free, desc, &lock_tag_id) !=
      TS_OK)
    {
      lock_free (l);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
i_unlock (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  lock_t *l;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "unlock") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_handle (&argv[0], &lock_tag_id, error, "unlock");
  if (!l)
    return TS_ERR_INVAL;
  if (l->fd >= 0)
    {
      flock (l->fd, LOCK_UN);
      close (l->fd);
      l->fd = -1;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
i_fifo (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *path;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "fifo") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &path, error, "fifo") != TS_OK)
    return TS_ERR_INVAL;
  if (mkfifo (path, 0666) != 0 && errno != EEXIST)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "fifo: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static const TS_FuncDef ipc_funcs[] = {
  { "lock", i_lock, NULL },
  { "unlock", i_unlock, NULL },
  { "fifo", i_fifo, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_ipc_module = { "std.ipc", ipc_funcs };
