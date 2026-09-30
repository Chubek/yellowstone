/* dt_util.c -- CRC32, poll and exact-transfer stream helpers. */
#include "dt_internal.h"

static uint32_t dt_crc_table[256];
static int dt_crc_ready = 0;

static void
dt_crc_init (void)
{
  uint32_t i, j, c;
  for (i = 0; i < 256; i++)
    {
      c = i;
      for (j = 0; j < 8; j++)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      dt_crc_table[i] = c;
    }
  dt_crc_ready = 1;
}

uint32_t
dt_crc32_update (uint32_t crc, const void *data, size_t len)
{
  const uint8_t *p = (const uint8_t *) data;
  size_t i;
  if (!dt_crc_ready)
    dt_crc_init ();
  for (i = 0; i < len; i++)
    crc = dt_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return crc;
}

uint32_t
dt_crc32 (const void *data, size_t len)
{
  return dt_crc32_update (0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

int
dt_poll_wait (int fd, short events, int timeout_ms)
{
  struct pollfd pfd;
  int r;
  if (fd < 0)
    {
      errno = EBADF;
      return -1;
    }
  pfd.fd = fd;
  pfd.events = events;
  pfd.revents = 0;
  for (;;)
    {
      r = poll (&pfd, 1, timeout_ms);
      if (r < 0 && errno == EINTR)
        continue;
      return r; /* 1 ready, 0 timeout, -1 error. */
    }
}

static int
dt_cancel_check (const volatile int *cancelled)
{
  return cancelled && *cancelled;
}

int
dt_stream_read_full (int fd, void *buf, size_t len, int timeout_ms,
                     const volatile int *cancelled, int *errn)
{
  uint8_t *p = (uint8_t *) buf;
  size_t got = 0;
  if (errn)
    *errn = 0;
  if (len == 0)
    return 0;
  while (got < len)
    {
      ssize_t r;
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
      if (timeout_ms == 0)
        {
          int pr = dt_poll_wait (fd, POLLIN, 0);
          if (pr == 0)
            return got ? DT_ERR_WOULD_BLOCK : DT_ERR_WOULD_BLOCK;
          if (pr < 0)
            {
              if (errn)
                *errn = errno;
              return DT_ERR_SYSTEM;
            }
        }
      else
        {
          int pr = dt_poll_wait (fd, POLLIN, timeout_ms);
          if (pr == 0)
            return DT_ERR_TIMEOUT;
          if (pr < 0)
            {
              if (errn)
                *errn = errno;
              return DT_ERR_SYSTEM;
            }
        }
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
      r = read (fd, p + got, len - got);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              if (timeout_ms == 0)
                return DT_ERR_WOULD_BLOCK;
              continue; /* poll again under the timeout budget. */
            }
          if (errn)
            *errn = errno;
          return DT_ERR_SYSTEM;
        }
      if (r == 0)
        return DT_ERR_EOF;
      got += (size_t) r;
    }
  return 0;
}

int
dt_stream_write_full (int fd, const void *buf, size_t len,
                      int timeout_ms, const volatile int *cancelled,
                      int *errn)
{
  const uint8_t *p = (const uint8_t *) buf;
  size_t done = 0;
  if (errn)
    *errn = 0;
  if (len == 0)
    return 0;
  while (done < len)
    {
      ssize_t r;
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
      if (timeout_ms == 0)
        {
          int pr = dt_poll_wait (fd, POLLOUT, 0);
          if (pr == 0)
            return DT_ERR_WOULD_BLOCK;
          if (pr < 0)
            {
              if (errn)
                *errn = errno;
              return DT_ERR_SYSTEM;
            }
        }
      else
        {
          int pr = dt_poll_wait (fd, POLLOUT, timeout_ms);
          if (pr == 0)
            return DT_ERR_TIMEOUT;
          if (pr < 0)
            {
              if (errn)
                *errn = errno;
              return DT_ERR_SYSTEM;
            }
        }
      if (dt_cancel_check (cancelled))
        return DT_ERR_CANCELLED;
      r = write (fd, p + done, len - done);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              if (timeout_ms == 0)
                return done ? DT_ERR_WOULD_BLOCK : DT_ERR_WOULD_BLOCK;
              continue;
            }
          if (errno == EPIPE)
            return DT_ERR_EOF;
          if (errn)
            *errn = errno;
          return DT_ERR_SYSTEM;
        }
      done += (size_t) r;
    }
  return 0;
}

DT_Status
dt_stream_read_line (int fd, char *buf, size_t cap,
                     int timeout_ms,
                     const volatile int *cancelled,
                     DT_Error *error)
{
  size_t n = 0;
  if (!buf || cap == 0)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad line buffer");
      return DT_ERR_INVALID_ARGUMENT;
    }
  while (n + 1 < cap)
    {
      char c;
      int rc, errn = 0;
      rc = dt_stream_read_full (fd, &c, 1, timeout_ms, cancelled, &errn);
      if (rc == DT_ERR_EOF)
        {
          if (n == 0)
            {
              dt_err_set (error, DT_ERR_EOF, 0, 0, "eof in handshake");
              return DT_ERR_EOF;
            }
          break;
        }
      if (rc != 0)
        {
          if (rc == DT_ERR_TIMEOUT)
            dt_err_set (error, DT_ERR_TIMEOUT, 0, 0, "handshake timeout");
          else if (rc == DT_ERR_CANCELLED)
            dt_err_set (error, DT_ERR_CANCELLED, 0, 0, "cancelled");
          else
            dt_err_set (error, DT_ERR_SYSTEM, errn, 0,
                        "handshake read failed");
          return (DT_Status) rc;
        }
      buf[n++] = c;
      if (c == '\n')
        break;
    }
  buf[n] = '\0';
  return DT_OK;
}
