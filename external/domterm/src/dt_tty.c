/* dt_tty.c -- TTY session management (self-contained, libc only). */
#include "dt_internal.h"

#include <sys/ioctl.h>
#include <termios.h>

struct DT_TTYSession
{
  int fd;
  bool own;
  bool restore;
  bool have_saved;
  struct termios saved;
  bool vtime_active; /* Last mode used VMIN=0/VTIME>0 (timed reads). */
};

void
dt_tty_options_init (DT_TTYOptions *options)
{
  if (!options)
    return;
  options->struct_size = (uint32_t) sizeof *options;
  options->fd = -1;
  options->take_ownership = false;
  options->no_restore = false;
  options->nonblocking = false;
  options->reserved = 0;
}

DT_TTYSession *
dt_tty_open (const DT_TTYOptions *options, DT_Error *error)
{
  DT_TTYSession *s;
  int flags;
  if (!options || options->fd < 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "invalid TTY options/fd");
      return NULL;
    }
  s = calloc (1, sizeof *s);
  if (!s)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  s->fd = options->fd;
  s->own = options->take_ownership;
  s->restore = !options->no_restore;
  if (options->nonblocking)
    {
      flags = fcntl (s->fd, F_GETFL);
      if (flags < 0 || fcntl (s->fd, F_SETFL, flags | O_NONBLOCK) < 0)
        {
          int e = errno;
          if (options->take_ownership)
            close (s->fd);
          free (s);
          dt_err_set (error, DT_ERR_SYSTEM, e, 0, "O_NONBLOCK failed");
          return NULL;
        }
    }
  if (tcgetattr (s->fd, &s->saved) == 0)
    s->have_saved = true;
  return s;
}

void
dt_tty_close (DT_TTYSession *session)
{
  if (!session)
    return;
  if (session->have_saved && session->restore)
    tcsetattr (session->fd, TCSANOW, &session->saved);
  if (session->own)
    close (session->fd);
  free (session);
}

int
dt_tty_fd (const DT_TTYSession *session)
{
  return session ? session->fd : -1;
}

int
dt_tty_pollfd (const DT_TTYSession *session)
{
  return session ? session->fd : -1;
}

/* Mode mapping (documented, portable termios subset):
   canonical <-> ICANON (+ ICANON off forces VMIN/VTIME byte mode)
   echo <-> ECHO|ECHOK
   signals <-> ISIG
   input_processing <-> BRKINT|ICRNL|IXON
   output_processing <-> OPOST
   read_timeout_ds <-> VTIME (capped at 255); noncanonical reads use
   VMIN=1/VTIME=0 when 0 (blocking) else VMIN=0/VTIME=n (timed).
   Canonical mode forces VMIN=1/VTIME=0. */
static void
dt_mode_to_termios (const DT_TTYMode *mode, struct termios *t,
                    bool *vtime_active)
{
  if (mode->canonical)
    {
      t->c_lflag |= ICANON;
      t->c_cc[VMIN] = 1;
      t->c_cc[VTIME] = 0;
      *vtime_active = false;
    }
  else
    {
      t->c_lflag &= (tcflag_t) ~ICANON;
      if (mode->read_timeout_ds == 0)
        {
          t->c_cc[VMIN] = 1;
          t->c_cc[VTIME] = 0;
          *vtime_active = false;
        }
      else
        {
          t->c_cc[VMIN] = 0;
          t->c_cc[VTIME] = mode->read_timeout_ds > 255 ?
                           255 : (cc_t) mode->read_timeout_ds;
          *vtime_active = true;
        }
    }
  if (mode->echo)
    t->c_lflag |= (tcflag_t) (ECHO | ECHOK);
  else
    t->c_lflag &= (tcflag_t) ~(ECHO | ECHOK | ECHONL);
  if (mode->signals)
    t->c_lflag |= ISIG;
  else
    t->c_lflag &= (tcflag_t) ~ISIG;
  if (mode->input_processing)
    t->c_iflag |= (tcflag_t) (BRKINT | ICRNL | IXON);
  else
    t->c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | IXON | INLCR | IGNCR);
  if (mode->output_processing)
    t->c_oflag |= OPOST;
  else
    t->c_oflag &= (tcflag_t) ~OPOST;
}

static void
dt_termios_to_mode (const struct termios *t, DT_TTYMode *mode)
{
  mode->canonical = (t->c_lflag & ICANON) != 0;
  mode->echo = (t->c_lflag & ECHO) != 0;
  mode->signals = (t->c_lflag & ISIG) != 0;
  mode->input_processing = (t->c_iflag & (BRKINT | ICRNL | IXON)) != 0;
  mode->output_processing = (t->c_oflag & OPOST) != 0;
  mode->read_timeout_ds = t->c_cc[VTIME];
}

DT_Status
dt_tty_get_mode (DT_TTYSession *session, DT_TTYMode *mode, DT_Error *error)
{
  struct termios t;
  if (!session || !mode)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (tcgetattr (session->fd, &t) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tcgetattr failed");
      return DT_ERR_SYSTEM;
    }
  dt_termios_to_mode (&t, mode);
  return DT_OK;
}

DT_Status
dt_tty_set_mode (DT_TTYSession *session, const DT_TTYMode *mode,
                 DT_Error *error)
{
  struct termios t;
  bool vtime = false;
  if (!session || !mode)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (tcgetattr (session->fd, &t) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tcgetattr failed");
      return DT_ERR_SYSTEM;
    }
  dt_mode_to_termios (mode, &t, &vtime);
  if (tcsetattr (session->fd, TCSANOW, &t) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tcsetattr failed");
      return DT_ERR_SYSTEM;
    }
  session->vtime_active = vtime;
  return DT_OK;
}

DT_Status
dt_tty_set_raw (DT_TTYSession *session, DT_Error *error)
{
  DT_TTYMode m;
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  m.canonical = false;
  m.echo = false;
  m.signals = false;
  m.input_processing = false;
  m.output_processing = false;
  m.read_timeout_ds = 0;
  return dt_tty_set_mode (session, &m, error);
}

DT_Status
dt_tty_set_cooked (DT_TTYSession *session, DT_Error *error)
{
  DT_TTYMode m;
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  m.canonical = true;
  m.echo = true;
  m.signals = true;
  m.input_processing = true;
  m.output_processing = true;
  m.read_timeout_ds = 0;
  return dt_tty_set_mode (session, &m, error);
}

DT_Status
dt_tty_save (DT_TTYSession *session, DT_Error *error)
{
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (tcgetattr (session->fd, &session->saved) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tcgetattr failed");
      return DT_ERR_SYSTEM;
    }
  session->have_saved = true;
  return DT_OK;
}

DT_Status
dt_tty_restore (DT_TTYSession *session, DT_Error *error)
{
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (!session->have_saved)
    {
      dt_err_set (error, DT_ERR_NOT_FOUND, 0, 0, "no saved TTY state");
      return DT_ERR_NOT_FOUND;
    }
  if (tcsetattr (session->fd, TCSANOW, &session->saved) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tcsetattr failed");
      return DT_ERR_SYSTEM;
    }
  return DT_OK;
}

DT_Status
dt_tty_get_winsize (DT_TTYSession *session, DT_Winsize *size, DT_Error *error)
{
  struct winsize ws;
  if (!session || !size)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (ioctl (session->fd, TIOCGWINSZ, &ws) < 0)
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

DT_Status
dt_tty_set_winsize (DT_TTYSession *session, const DT_Winsize *size,
                    DT_Error *error)
{
  struct winsize ws;
  if (!session || !size)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  ws.ws_row = size->rows;
  ws.ws_col = size->columns;
  ws.ws_xpixel = size->xpixel;
  ws.ws_ypixel = size->ypixel;
  if (ioctl (session->fd, TIOCSWINSZ, &ws) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "TIOCSWINSZ failed");
      return DT_ERR_SYSTEM;
    }
  return DT_OK;
}

DT_Status
dt_tty_set_nonblocking (DT_TTYSession *session, bool nonblocking,
                         DT_Error *error)
{
  int flags;
  if (!session)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL session");
      return DT_ERR_INVALID_ARGUMENT;
    }
  flags = fcntl (session->fd, F_GETFL);
  if (flags < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "fcntl failed");
      return DT_ERR_SYSTEM;
    }
  if (nonblocking)
    flags |= O_NONBLOCK;
  else
    flags &= ~O_NONBLOCK;
  if (fcntl (session->fd, F_SETFL, flags) < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "fcntl failed");
      return DT_ERR_SYSTEM;
    }
  return DT_OK;
}

DT_Status
dt_tty_read (DT_TTYSession *session, void *buffer, size_t capacity,
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
      r = read (session->fd, buffer, capacity);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              dt_err_set (error, DT_ERR_WOULD_BLOCK, errno, 0,
                          "tty read would block");
              return DT_ERR_WOULD_BLOCK;
            }
          if (errno == EIO)
            {
              dt_err_set (error, DT_ERR_EOF, 0, 0, "tty hangup");
              return DT_ERR_EOF;
            }
          dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tty read failed");
          return DT_ERR_SYSTEM;
        }
      if (r == 0)
        {
          if (session->vtime_active)
            {
              dt_err_set (error, DT_ERR_TIMEOUT, 0, 0, "tty read timeout");
              return DT_ERR_TIMEOUT;
            }
          dt_err_set (error, DT_ERR_EOF, 0, 0, "tty EOF");
          return DT_ERR_EOF;
        }
      *bytes_read = (size_t) r;
      return DT_OK;
    }
}

DT_Status
dt_tty_write (DT_TTYSession *session, const void *buffer, size_t size,
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
      ssize_t r = write (session->fd, p + done, size - done);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              if (done)
                {
                  *bytes_written = done;
                  return DT_OK; /* Partial write is normal. */
                }
              dt_err_set (error, DT_ERR_WOULD_BLOCK, errno, 0,
                          "tty write would block");
              return DT_ERR_WOULD_BLOCK;
            }
          if (errno == EPIPE || errno == EIO)
            {
              dt_err_set (error, DT_ERR_EOF, errno, 0, "tty peer closed");
              return DT_ERR_EOF;
            }
          dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "tty write failed");
          return DT_ERR_SYSTEM;
        }
      done += (size_t) r;
    }
  *bytes_written = done;
  return DT_OK;
}
