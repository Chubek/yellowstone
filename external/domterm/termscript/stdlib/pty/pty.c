/* pty.c -- std.pty: child processes under a pseudoterminal.
 *
 * A standalone-safe minipty (no libdomterm): spawn() forks /bin/sh -c
 * under a fresh pty pair; read()/write() move bytes; wait() reaps
 * (nil while running); close() kills and reaps.
 */
#include "pty/pty.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct
{
  int master;
  pid_t pid;
  bool reaped;
  int exit_code;
} pty_t;

static const char pty_tag_id = 0;

static void
pty_free (void *p)
{
  pty_t *h = p;
  int st;
  if (!h)
    return;
  if (!h->reaped && h->pid > 0)
    {
      kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
    }
  if (h->master >= 0)
    close (h->master);
  free (h);
}

static pty_t *
pty_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &pty_tag_id, error, what);
}

static TS_Status
p_spawn (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  int master, slave;
  char *slave_name;
  pid_t pid;
  pty_t *h;
  char desc[96];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "spawn") != TS_OK)
    return TS_ERR_INVAL;
  master = posix_openpt (O_RDWR | O_NOCTTY);
  if (master < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "spawn: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (grantpt (master) != 0 || unlockpt (master) != 0)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  slave_name = ptsname (master);
  if (!slave_name)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  slave = open (slave_name, O_RDWR | O_NOCTTY);
  if (slave < 0)
    {
      int e = errno;
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  pid = fork ();
  if (pid < 0)
    {
      int e = errno;
      close (slave);
      close (master);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "spawn: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  if (pid == 0)
    {
      /* Child: new session, controlling terminal, stdio on slave. */
      setsid ();
      ioctl (slave, TIOCSCTTY, 0);
      dup2 (slave, 0);
      dup2 (slave, 1);
      dup2 (slave, 2);
      if (slave > 2)
        close (slave);
      close (master);
      execl ("/bin/sh", "sh", "-c", cmd, (char *) NULL);
      _exit (127);
    }
  close (slave);
  h = calloc (1, sizeof *h);
  if (!h)
    {
      int st;
      kill (pid, SIGKILL);
      while (waitpid (pid, &st, 0) < 0 && errno == EINTR)
        ;
      close (master);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  h->master = master;
  h->pid = pid;
  snprintf (desc, sizeof desc, "<pty pid=%d>", (int) pid);
  if (ts_value_make_handle (ret, h, pty_free, desc, &pty_tag_id) !=
      TS_OK)
    {
      pty_free (h);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
p_read (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int64_t max = 65536;
  char *buf;
  ssize_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "read");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  if (argc == 2 && ts_std_int (&argv[1], &max, error, "read") != TS_OK)
    return TS_ERR_INVAL;
  if (max < 1 || max > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "read: bad size");
      return TS_ERR_INVAL;
    }
  buf = malloc ((size_t) max + 1);
  if (!buf)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  for (;;)
    {
      r = read (h->master, buf, (size_t) max);
      if (r < 0 && errno == EINTR)
        continue;
      break;
    }
  if (r <= 0)
    {
      free (buf);
      ts_std_ret_nil (ret); /* EOF or EIO after child exit. */
      return TS_OK;
    }
  buf[r] = '\0';
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = buf;
  return TS_OK;
}

static TS_Status
p_write (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  const char *text;
  size_t n, sent = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "write");
  if (!h)
    return TS_ERR_INVAL;
  if (h->master < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  if (ts_std_str (&argv[1], &text, error, "write") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (text);
  while (sent < n)
    {
      ssize_t r = write (h->master, text + sent, n - sent);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          ts_std_ret_bool (ret, false);
          return TS_OK;
        }
      sent += (size_t) r;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
p_wait (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int st;
  pid_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "wait") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "wait");
  if (!h)
    return TS_ERR_INVAL;
  if (h->reaped)
    {
      ts_std_ret_int (ret, h->exit_code);
      return TS_OK;
    }
  r = waitpid (h->pid, &st, WNOHANG);
  if (r < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "wait: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (!r)
    {
      ts_std_ret_nil (ret); /* Still running. */
      return TS_OK;
    }
  h->reaped = true;
  h->exit_code = WIFEXITED (st) ? WEXITSTATUS (st) :
                 WIFSIGNALED (st) ? 128 + WTERMSIG (st) : 127;
  ts_std_ret_int (ret, h->exit_code);
  return TS_OK;
}

static TS_Status
p_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  pty_t *h;
  int st;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  h = pty_unwrap (&argv[0], error, "close");
  if (!h)
    return TS_ERR_INVAL;
  if (!h->reaped && h->pid > 0)
    {
      kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
      h->reaped = true;
    }
  if (h->master >= 0)
    {
      close (h->master);
      h->master = -1;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef pty_funcs[] = {
  { "spawn", p_spawn, NULL },
  { "read", p_read, NULL },
  { "write", p_write, NULL },
  { "wait", p_wait, NULL },
  { "close", p_close, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_pty_module = { "std.pty", pty_funcs };
