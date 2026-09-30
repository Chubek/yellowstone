/* dt_pty.c -- PTY master management and child control (self-contained). */
#include "dt_internal.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

extern char **environ;

struct DT_PTYSession
{
  int master;
  pid_t child;
  bool child_alive;
  bool reaped;
  int raw_status;
  bool kill_on_close;
  int kill_signal;
};

void
dt_pty_options_init (DT_PTYOptions *options)
{
  if (!options)
    return;
  options->struct_size = (uint32_t) sizeof *options;
  options->terminal_name = NULL;
  options->working_directory = NULL;
  options->argv = NULL;
  options->envp = NULL;
  options->rows = 24;
  options->columns = 80;
  options->nonblocking = false;
  options->kill_on_close = true;
  options->kill_signal = SIGKILL;
  options->reserved = 0;
}

static void
dt_pty_destroy (DT_PTYSession *s)
{
  if (!s)
    return;
  if (s->master >= 0)
    close (s->master);
  free (s);
}

/* Reap helper: blocking ? wait : poll. Returns 1 reaped, 0 running,
   -1 error (errno set, ECHILD when no child). */
static int
dt_reap (DT_PTYSession *s, bool blocking, int *status)
{
  for (;;)
    {
      pid_t r = waitpid (s->child, status, blocking ? 0 : WNOHANG);
      if (r < 0)
        {
          if (errno == EINTR && blocking)
            continue;
          return -1;
        }
      if (r == 0)
        return 0; /* WNOHANG: still running. */
      return 1; /* Reaped. */
    }
}

DT_PTYSession *
dt_pty_spawn (const DT_PTYOptions *options, DT_Error *error)
{
  DT_PTYSession *s = NULL;
  int master = -1, slave = -1;
  int errpipe[2] = { -1, -1 };
  char slave_path[128];
  pid_t pid;
  struct winsize ws;

  if (!options || !options->argv || !options->argv[0])
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "argv with argv[0] is required");
      return NULL;
    }

  s = calloc (1, sizeof *s);
  if (!s)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  s->master = -1;
  s->child = -1;
  s->kill_on_close = options->kill_on_close;
  s->kill_signal = options->kill_signal > 0 ? options->kill_signal : SIGKILL;

  master = posix_openpt (O_RDWR | O_NOCTTY);
  if (master < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "posix_openpt failed");
      goto fail;
    }
  if (grantpt (master) < 0 || unlockpt (master) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "grantpt/unlockpt failed");
      goto fail;
    }
  if (ptsname_r (master, slave_path, sizeof slave_path) != 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "ptsname failed");
      goto fail;
    }
  slave = open (slave_path, O_RDWR | O_NOCTTY);
  if (slave < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "slave open failed");
      goto fail;
    }
  if (options->rows || options->columns)
    {
      memset (&ws, 0, sizeof ws);
      ws.ws_row = options->rows;
      ws.ws_col = options->columns;
      ioctl (slave, TIOCSWINSZ, &ws); /* Best effort pre-fork. */
    }
  if (pipe2 (errpipe, O_CLOEXEC) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "pipe failed");
      goto fail;
    }

  pid = fork ();
  if (pid < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "fork failed");
      goto fail;
    }
  if (pid == 0)
    {
      /* Child: become session leader with the slave as controlling tty. */
      int e;
      close (errpipe[0]);
      close (master);
      if (setsid () < 0)
        goto child_fail;
      if (ioctl (slave, TIOCSCTTY, 0) < 0)
        goto child_fail;
      if (slave != STDIN_FILENO)
        dup2 (slave, STDIN_FILENO);
      if (slave != STDOUT_FILENO)
        dup2 (slave, STDOUT_FILENO);
      if (slave != STDERR_FILENO)
        dup2 (slave, STDERR_FILENO);
      if (slave > STDERR_FILENO)
        close (slave);
      if (options->working_directory &&
          chdir (options->working_directory) < 0)
        goto child_fail;
      if (options->terminal_name)
        {
          setenv ("TERM", options->terminal_name, 1);
        }
      if (options->envp)
        execvpe (options->argv[0], options->argv, options->envp);
      else
        execvp (options->argv[0], options->argv);
    child_fail:
      e = errno;
      (void) write (errpipe[1], &e, sizeof e);
      _exit (127);
    }

  /* Parent. */
  close (slave);
  slave = -1;
  close (errpipe[1]);
  errpipe[1] = -1;
  {
    int child_errno = 0;
    size_t got = 0;
    ssize_t r;
    /* errpipe[1] is CLOEXEC: EOF means exec succeeded. */
    while (got < sizeof child_errno)
      {
        r = read (errpipe[0], ((char *) &child_errno) + got,
                  sizeof child_errno - got);
        if (r < 0)
          {
            if (errno == EINTR)
              continue;
            break;
          }
        if (r == 0)
          break;
        got += (size_t) r;
      }
    close (errpipe[0]);
    errpipe[0] = -1;
    if (got == sizeof child_errno)
      {
        int st = 0;
        waitpid (pid, &st, 0); /* Reap the failed child. */
        close (master);
        free (s);
        dt_err_set (error, DT_ERR_SYSTEM, child_errno,
                    0, "child exec failed");
        return NULL;
      }
  }
  if (options->nonblocking)
    {
      int flags = fcntl (master, F_GETFL);
      if (flags >= 0)
        fcntl (master, F_SETFL, flags | O_NONBLOCK);
    }
  s->master = master;
  s->child = pid;
  s->child_alive = true;
  return s;

fail:
  if (errpipe[0] >= 0)
    close (errpipe[0]);
  if (errpipe[1] >= 0)
    close (errpipe[1]);
  if (slave >= 0)
    close (slave);
  if (master >= 0)
    close (master);
  free (s);
  return NULL;
}

void
dt_pty_close (DT_PTYSession *session)
{
  int st;
  if (!session)
    return;
  if (session->child_alive && !session->reaped)
    {
      if (session->kill_on_close)
        {
          kill (session->child, session->kill_signal);
          /* SIGKILL cannot be caught: blocking reap terminates. */
          while (waitpid (session->child, &st, 0) < 0 && errno == EINTR)
            ;
          session->raw_status = st;
          session->reaped = true;
        }
      else
        {
          /* Best effort: reap if already exited, else leave running. */
          pid_t r = waitpid (session->child, &st, WNOHANG);
          if (r == session->child)
            {
              session->raw_status = st;
              session->reaped = true;
            }
        }
      session->child_alive = false;
    }
  dt_pty_destroy (session);
}

DT_Status
dt_pty_get_info (const DT_PTYSession *session, DT_PTYInfo *info,
                 DT_Error *error)
{
  if (!session || !info)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  info->master_fd = session->master;
  info->child_pid = (int) session->child;
  return DT_OK;
}

int
dt_pty_pollfd (const DT_PTYSession *session)
{
  return session ? session->master : -1;
}

DT_Status
dt_pty_set_nonblocking (DT_PTYSession *session, bool nonblocking,
                         DT_Error *error)
{
  int flags;
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  flags = fcntl (session->master, F_GETFL);
  if (flags < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "fcntl failed");
      return DT_ERR_SYSTEM;
    }
  if (nonblocking)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
  if (fcntl (session->master, F_SETFL, flags) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "fcntl failed");
      return DT_ERR_SYSTEM;
    }
  return DT_OK;
}

DT_Status
dt_pty_read (DT_PTYSession *session, void *buffer, size_t capacity,
             size_t *bytes_read, DT_Error *error)
{
  ssize_t r;
  if (!session || !buffer || !bytes_read || capacity == 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  for (;;)
    {
      r = read (session->master, buffer, capacity);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              dt_err_set (error, DT_ERR_WOULD_BLOCK, errno, 0,
                          "pty read would block");
              return DT_ERR_WOULD_BLOCK;
            }
          if (errno == EIO)
            {
              dt_err_set (error, DT_ERR_EOF, 0, 0,
                          "pty slave closed (EOF)");
              return DT_ERR_EOF;
            }
          dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "pty read failed");
          return DT_ERR_SYSTEM;
        }
      if (r == 0)
        {
          dt_err_set (error, DT_ERR_EOF, 0, 0, "pty EOF");
          return DT_ERR_EOF;
        }
      *bytes_read = (size_t) r;
      return DT_OK;
    }
}

DT_Status
dt_pty_write (DT_PTYSession *session, const void *buffer, size_t size,
              size_t *bytes_written, DT_Error *error)
{
  const uint8_t *p;
  size_t done = 0;
  if (!session || !buffer || !bytes_written || size == 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  p = (const uint8_t *) buffer;
  while (done < size)
    {
      ssize_t r = write (session->master, p + done, size - done);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              if (done)
                {
                  *bytes_written = done;
                  return DT_OK;
                }
              dt_err_set (error, DT_ERR_WOULD_BLOCK, errno, 0,
                          "pty write would block");
              return DT_ERR_WOULD_BLOCK;
            }
          if (errno == EPIPE || errno == EIO)
            {
              dt_err_set (error, DT_ERR_EOF, errno, 0, "pty peer closed");
              return DT_ERR_EOF;
            }
          dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "pty write failed");
          return DT_ERR_SYSTEM;
        }
      done += (size_t) r;
    }
  *bytes_written = done;
  return DT_OK;
}

DT_Status
dt_pty_resize (DT_PTYSession *session, unsigned short rows,
               unsigned short columns, DT_Error *error)
{
  struct winsize ws;
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  memset (&ws, 0, sizeof ws);
  ws.ws_row = rows;
  ws.ws_col = columns;
  if (ioctl (session->master, TIOCSWINSZ, &ws) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "TIOCSWINSZ failed");
      return DT_ERR_SYSTEM;
    }
  return DT_OK;
}

DT_Status
dt_pty_get_winsize (const DT_PTYSession *session, DT_Winsize *size,
                    DT_Error *error)
{
  struct winsize ws;
  if (!session || !size)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (ioctl (session->master, TIOCGWINSZ, &ws) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "TIOCGWINSZ failed");
      return DT_ERR_SYSTEM;
    }
  size->rows = ws.ws_row;
  size->columns = ws.ws_col;
  size->xpixel = ws.ws_xpixel;
  size->ypixel = ws.ws_ypixel;
  return DT_OK;
}

static void
dt_decode_status (int st, int *exit_code, bool *exited_normally)
{
  if (WIFEXITED (st))
    {
      *exit_code = WEXITSTATUS (st);
      *exited_normally = true;
    }
  else if (WIFSIGNALED (st))
    {
      *exit_code = 128 + WTERMSIG (st);
      *exited_normally = false;
    }
  else /* Stopped/other: report distinctly, not as an exit. */
    {
      *exit_code = 128 + (WIFSTOPPED (st) ? WSTOPSIG (st) : 0);
      *exited_normally = false;
    }
}

DT_Status
dt_pty_wait (DT_PTYSession *session, int *exit_code, bool *exited_normally,
             DT_Error *error)
{
  int st = 0;
  int r;
  if (!session || !exit_code || !exited_normally)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (session->reaped)
    {
      dt_decode_status (session->raw_status, exit_code, exited_normally);
      return DT_OK;
    }
  if (!session->child_alive || session->child < 0)
    {
      dt_err_set (error, DT_ERR_CHILD_EXITED, 0, 0, "no child attached");
      return DT_ERR_CHILD_EXITED;
    }
  r = dt_reap (session, true, &st);
  if (r != 1)
    {
      dt_err_set (error, DT_ERR_CHILD_EXITED, errno, 0,
                  "child already reaped");
      session->child_alive = false;
      return DT_ERR_CHILD_EXITED;
    }
  session->raw_status = st;
  session->reaped = true;
  session->child_alive = false;
  dt_decode_status (st, exit_code, exited_normally);
  return DT_OK;
}

DT_Status
dt_pty_poll (DT_PTYSession *session, bool *exited, int *exit_code,
             bool *exited_normally, DT_Error *error)
{
  int st = 0;
  int r;
  if (!session || !exited || !exit_code || !exited_normally)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (session->reaped)
    {
      *exited = true;
      dt_decode_status (session->raw_status, exit_code, exited_normally);
      return DT_OK;
    }
  if (!session->child_alive || session->child < 0)
    {
      dt_err_set (error, DT_ERR_CHILD_EXITED, 0, 0, "no child attached");
      return DT_ERR_CHILD_EXITED;
    }
  r = dt_reap (session, false, &st);
  if (r < 0)
    {
      if (errno == ECHILD)
        {
          dt_err_set (error, DT_ERR_CHILD_EXITED, 0, 0, "no child");
          session->child_alive = false;
          return DT_ERR_CHILD_EXITED;
        }
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "waitpid failed");
      return DT_ERR_SYSTEM;
    }
  if (r == 0)
    {
      *exited = false;
      dt_err_set (error, DT_ERR_WOULD_BLOCK, 0, 0, "child still running");
      return DT_ERR_WOULD_BLOCK;
    }
  session->raw_status = st;
  session->reaped = true;
  session->child_alive = false;
  *exited = true;
  dt_decode_status (st, exit_code, exited_normally);
  return DT_OK;
}

DT_Status
dt_pty_wait_status (DT_PTYSession *session, int *raw_status,
                    bool wait_blocking, DT_Error *error)
{
  int st = 0;
  int r;
  if (!session || !raw_status)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (session->reaped)
    {
      *raw_status = session->raw_status;
      return DT_OK;
    }
  if (!session->child_alive || session->child < 0)
    {
      dt_err_set (error, DT_ERR_CHILD_EXITED, 0, 0, "no child attached");
      return DT_ERR_CHILD_EXITED;
    }
  r = dt_reap (session, wait_blocking, &st);
  if (r < 0 || (r == 0 && !wait_blocking))
    {
      if (r == 0)
        {
          dt_err_set (error, DT_ERR_WOULD_BLOCK, 0, 0,
                      "child still running");
          return DT_ERR_WOULD_BLOCK;
        }
      dt_err_set (error, DT_ERR_CHILD_EXITED, errno, 0, "waitpid failed");
      session->child_alive = false;
      return DT_ERR_CHILD_EXITED;
    }
  session->raw_status = st;
  session->reaped = true;
  session->child_alive = false;
  *raw_status = st;
  return DT_OK;
}

bool
dt_pty_status_exited (int raw_status, int *exit_code)
{
  if (!WIFEXITED (raw_status))
    return false;
  if (exit_code)
    *exit_code = WEXITSTATUS (raw_status);
  return true;
}

bool
dt_pty_status_signaled (int raw_status, int *signal_number)
{
  if (!WIFSIGNALED (raw_status))
    return false;
  if (signal_number)
    *signal_number = WTERMSIG (raw_status);
  return true;
}

bool
dt_pty_status_stopped (int raw_status, int *stop_signal)
{
  if (!WIFSTOPPED (raw_status))
    return false;
  if (stop_signal)
    *stop_signal = WSTOPSIG (raw_status);
  return true;
}

DT_Status
dt_pty_terminate (DT_PTYSession *session, int signal_number, DT_Error *error)
{
  if (!session || signal_number <= 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (!session->child_alive || session->child < 0)
    {
      dt_err_set (error, DT_ERR_CHILD_EXITED, 0, 0, "no live child");
      return DT_ERR_CHILD_EXITED;
    }
  if (kill (session->child, signal_number) < 0)
    {
      if (errno == ESRCH)
        {
          session->child_alive = false;
          dt_err_set (error, DT_ERR_CHILD_EXITED, 0, 0, "child is gone");
          return DT_ERR_CHILD_EXITED;
        }
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "kill failed");
      return DT_ERR_SYSTEM;
    }
  return DT_OK;
}
