/* socket.c -- std.socket: blocking TCP clients and tiny servers.
 *
 * connect() dials, send()/recv() move bytes (recv nil means EOF),
 * listen()/accept() serve one connection at a time.  Timeouts are
 * per-call milliseconds via set_timeout(); without it calls block.
 */
#include "socket/socket.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct
{
  int fd;
} sock_t;

static const char sock_tag_id = 0;

static void
sock_free (void *p)
{
  sock_t *s = p;
  if (!s)
    return;
  if (s->fd >= 0)
    close (s->fd);
  free (s);
}

static sock_t *
sock_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  sock_t *s = ts_std_handle (v, &sock_tag_id, error, what);
  if (s && s->fd < 0)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: closed socket",
                    what);
      return NULL;
    }
  return s;
}

static TS_Status
sock_wrap (TS_Value *ret, int fd, const char *label, TS_Error *error)
{
  sock_t *s = malloc (sizeof *s);
  char desc[64];
  if (!s)
    {
      close (fd);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  s->fd = fd;
  snprintf (desc, sizeof desc, "<socket %s>", label);
  if (ts_value_make_handle (ret, s, sock_free, desc, &sock_tag_id) !=
      TS_OK)
    {
      sock_free (s);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
s_connect (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *host;
  int64_t port;
  struct addrinfo hints, *list = NULL, *ai;
  char ports[16];
  int fd = -1;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "connect") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &host, error, "connect") != TS_OK ||
      ts_std_int (&argv[1], &port, error, "connect") != TS_OK)
    return TS_ERR_INVAL;
  if (port < 1 || port > 65535)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "connect: bad port");
      return TS_ERR_INVAL;
    }
  snprintf (ports, sizeof ports, "%lld", (long long) port);
  memset (&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo (host, ports, &hints, &list) != 0 || !list)
    {
      ts_std_ret_nil (ret); /* Unresolvable: nil, not an error. */
      return TS_OK;
    }
  for (ai = list; ai; ai = ai->ai_next)
    {
      fd = socket (ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
        continue;
      if (connect (fd, ai->ai_addr, ai->ai_addrlen) == 0)
        break;
      close (fd);
      fd = -1;
    }
  freeaddrinfo (list);
  if (fd < 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return sock_wrap (ret, fd, "conn", error);
}

static TS_Status
s_send (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  sock_t *s;
  const char *text;
  size_t n, sent = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  s = sock_unwrap (&argv[0], error, "send");
  if (!s || ts_std_str (&argv[1], &text, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (text);
  while (sent < n)
    {
      ssize_t r = send (s->fd, text + sent, n - sent, MSG_NOSIGNAL);
      if (r < 0)
        {
          if (errno == EINTR)
            continue;
          ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "send: %s",
                        strerror (errno));
          return TS_ERR_SYSTEM;
        }
      sent += (size_t) r;
    }
  ts_std_ret_int (ret, (int64_t) sent);
  return TS_OK;
}

static TS_Status
s_recv (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  sock_t *s;
  int64_t max = 65536;
  char *buf;
  ssize_t r;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 2, error, "recv") != TS_OK)
    return TS_ERR_INVAL;
  s = sock_unwrap (&argv[0], error, "recv");
  if (!s)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_int (&argv[1], &max, error, "recv") != TS_OK)
    return TS_ERR_INVAL;
  if (max < 1 || max > 1000000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "recv: bad size");
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
      r = recv (s->fd, buf, (size_t) max, 0);
      if (r < 0 && errno == EINTR)
        continue;
      break;
    }
  if (r < 0)
    {
      free (buf);
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "recv: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  if (!r)
    {
      free (buf);
      ts_std_ret_nil (ret); /* Orderly shutdown. */
      return TS_OK;
    }
  buf[r] = '\0';
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = buf;
  return TS_OK;
}

static TS_Status
s_listen (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  int64_t port;
  int fd, one = 1;
  struct sockaddr_in addr;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "listen") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &port, error, "listen") != TS_OK)
    return TS_ERR_INVAL;
  if (port < 1 || port > 65535)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "listen: bad port");
      return TS_ERR_INVAL;
    }
  fd = socket (AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "socket: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  memset (&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
  addr.sin_port = htons ((uint16_t) port);
  if (bind (fd, (struct sockaddr *) &addr, sizeof addr) != 0 ||
      listen (fd, 4) != 0)
    {
      int e = errno;
      close (fd);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "listen: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  return sock_wrap (ret, fd, "listener", error);
}

static TS_Status
s_accept (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  sock_t *s;
  int fd;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "accept") != TS_OK)
    return TS_ERR_INVAL;
  s = sock_unwrap (&argv[0], error, "accept");
  if (!s)
    return TS_ERR_INVAL;
  for (;;)
    {
      fd = accept (s->fd, NULL, NULL);
      if (fd < 0 && errno == EINTR)
        continue;
      break;
    }
  if (fd < 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "accept: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  return sock_wrap (ret, fd, "conn", error);
}

static TS_Status
s_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  sock_t *s;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  s = ts_std_handle (&argv[0], &sock_tag_id, error, "close");
  if (!s)
    return TS_ERR_INVAL;
  if (s->fd >= 0)
    {
      close (s->fd);
      s->fd = -1;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef socket_funcs[] = {
  { "connect", s_connect, NULL },
  { "send", s_send, NULL },
  { "recv", s_recv, NULL },
  { "listen", s_listen, NULL },
  { "accept", s_accept, NULL },
  { "close", s_close, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_socket_module = { "std.socket", socket_funcs };
