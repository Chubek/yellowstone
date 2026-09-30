/* dt_internal.h -- shared DomTERM implementation helpers (not installed). */
#ifndef DT_INTERNAL_H
#define DT_INTERNAL_H

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "domterm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/types.h>

/* Fill error with truncation-safe printf formatting. */
static inline void
dt_err_set (DT_Error *e, DT_Status code, int errn, size_t off,
            const char *fmt, ...)
{
  va_list ap;
  if (!e)
    return;
  e->code = code;
  e->system_errno = errn;
  e->offset = off;
  if (!fmt)
    {
      e->message[0] = '\0';
      return;
    }
  va_start (ap, fmt);
  vsnprintf (e->message, sizeof e->message, fmt, ap);
  va_end (ap);
}

static inline void *
dt_xmalloc (size_t n)
{
  void *p = malloc (n ? n : 1);
  return p;
}

/* CRC32-IEEE (polynomial 0xEDB88320), used by recording and framing. */
uint32_t dt_crc32 (const void *data, size_t len);
uint32_t dt_crc32_update (uint32_t crc, const void *data, size_t len);

/* Growable text buffer helpers (defined in dt_record.c). */
void dt_buf_put (char **buf, size_t *len, size_t *cap, const char *s,
                 size_t n);
void dt_buf_str (char **buf, size_t *len, size_t *cap, const char *s);

/* Little-endian codecs. */
static inline void dt_put_u16le (uint8_t *p, uint16_t v)
{ p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); }
static inline void dt_put_u32le (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8);
  p[2] = (uint8_t) (v >> 16); p[3] = (uint8_t) (v >> 24);
}
static inline void dt_put_u64le (uint8_t *p, uint64_t v)
{
  dt_put_u32le (p, (uint32_t) v);
  dt_put_u32le (p + 4, (uint32_t) (v >> 32));
}
static inline uint16_t dt_get_u16le (const uint8_t *p)
{ return (uint16_t) ((uint16_t) p[0] | ((uint16_t) p[1] << 8)); }
static inline uint32_t dt_get_u32le (const uint8_t *p)
{
  return ((uint32_t) p[0] | ((uint32_t) p[1] << 8) |
          ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24));
}
static inline uint64_t dt_get_u64le (const uint8_t *p)
{
  return ((uint64_t) dt_get_u32le (p) |
          ((uint64_t) dt_get_u32le (p + 4) << 32));
}
static inline void dt_put_u32be (uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16);
  p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v;
}
static inline uint32_t dt_get_u32be (const uint8_t *p)
{
  return (((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
          ((uint32_t) p[2] << 8) | (uint32_t) p[3]);
}
static inline void dt_put_u64be (uint8_t *p, uint64_t v)
{
  dt_put_u32be (p, (uint32_t) (v >> 32));
  dt_put_u32be (p + 4, (uint32_t) v);
}
static inline uint64_t dt_get_u64be (const uint8_t *p)
{
  return (((uint64_t) dt_get_u32be (p) << 32) | dt_get_u32be (p + 4));
}

/* Wait for fd readiness. events = POLLIN/POLLOUT. timeout_ms: <0 forever,
   0 poll, >0 wait. Returns 1 ready, 0 timeout, -1 error (errno set).
   EINTR is retried internally. */
int dt_poll_wait (int fd, short events, int timeout_ms);

/* Transfer exactly `len` bytes with a poll-based timeout; handles EINTR
   internally and partial transfers. timeout_ms: <0 forever, 0 nonblocking
   single attempt, >0 wait.  Returns 0 on success, DT_ERR_* style codes:
   DT_ERR_EOF (read side closed), DT_ERR_TIMEOUT, DT_ERR_WOULD_BLOCK
   (timeout_ms == 0 without progress), DT_ERR_SYSTEM (errno preserved in
   *errn when non-NULL), DT_ERR_CANCELLED (when cancelled flag set). */
int dt_stream_read_full (int fd, void *buf, size_t len, int timeout_ms,
                         const volatile int *cancelled, int *errn);
int dt_stream_write_full (int fd, const void *buf, size_t len,
                          int timeout_ms, const volatile int *cancelled,
                          int *errn);

/* Read one ASCII line (up to cap bytes incl. NUL) with timeout. */
DT_Status dt_stream_read_line (int fd, char *buf, size_t cap,
                               int timeout_ms,
                               const volatile int *cancelled,
                               DT_Error *error);

#endif /* DT_INTERNAL_H */
