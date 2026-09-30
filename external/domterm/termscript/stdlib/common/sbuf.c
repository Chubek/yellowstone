/* sbuf.c -- growable string buffer for the Termscript stdlib. */
#include "common/ts_std_common.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
ts_sbuf_init (ts_sbuf_t *b)
{
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void
ts_sbuf_free (ts_sbuf_t *b)
{
  if (!b)
    return;
  free (b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

void
ts_sbuf_clear (ts_sbuf_t *b)
{
  if (!b)
    return;
  b->len = 0;
  if (b->data)
    b->data[0] = '\0';
}

int
ts_sbuf_put (ts_sbuf_t *b, const char *s, size_t n)
{
  char *nb;
  size_t nc;
  if (!b || (!s && n))
    return -1;
  if (b->len + n + 1 > b->cap)
    {
      nc = b->cap ? b->cap * 2 : 64;
      while (nc < b->len + n + 1)
        nc *= 2;
      nb = realloc (b->data, nc);
      if (!nb)
        return -1;
      b->data = nb;
      b->cap = nc;
    }
  if (n)
    memcpy (b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return 0;
}

int
ts_sbuf_str (ts_sbuf_t *b, const char *s)
{
  return ts_sbuf_put (b, s ? s : "", s ? strlen (s) : 0);
}

int
ts_sbuf_ch (ts_sbuf_t *b, char c)
{
  return ts_sbuf_put (b, &c, 1);
}

int
ts_sbuf_printf (ts_sbuf_t *b, const char *fmt, ...)
{
  va_list ap;
  char tmp[256];
  int n;
  va_start (ap, fmt);
  n = vsnprintf (tmp, sizeof tmp, fmt, ap);
  va_end (ap);
  if (n < 0)
    return -1;
  if ((size_t) n < sizeof tmp)
    return ts_sbuf_put (b, tmp, (size_t) n);
  {
    /* Long rendering: size exactly, then print again. */
    char *big = malloc ((size_t) n + 1);
    if (!big)
      return -1;
    va_start (ap, fmt);
    vsnprintf (big, (size_t) n + 1, fmt, ap);
    va_end (ap);
    n = ts_sbuf_put (b, big, (size_t) n);
    free (big);
    return n;
  }
}

char *
ts_sbuf_take (ts_sbuf_t *b)
{
  char *out;
  if (!b || !b->data)
    {
      out = malloc (1);
      if (out)
        out[0] = '\0';
      if (b)
        {
          b->len = 0;
        }
      return out;
    }
  out = b->data;
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  return out;
}
