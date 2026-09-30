/* http.c -- std.http: minimal HTTP/1.0 client.
 *
 * Plain-socket GET/POST with a small response splitter (status,
 * headers, body).  No TLS: https URLs fail with INVALID_ARGUMENT
 * rather than silently downgrading (same policy as DomTERM's remote
 * connections).
 */
#include "http/http.h"

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
  char host[256];
  char port[16];
  char path[2048];
} url_t;

static TS_Status
url_parse (const char *url, url_t *out, TS_Error *error)
{
  const char *p, *slash, *colon;
  size_t n;
  memset (out, 0, sizeof *out);
  if (strncmp (url, "http://", 7) == 0)
    {
      strcpy (out->port, "80");
      p = url + 7;
    }
  else
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0,
                    "http: only http:// URLs (no TLS here)");
      return TS_ERR_INVAL;
    }
  slash = strchr (p, '/');
  colon = strchr (p, ':');
  if (colon && (!slash || colon < slash))
    {
      n = (size_t) (colon - p);
      if (!n || n >= sizeof out->host)
        goto bad;
      memcpy (out->host, p, n);
      out->host[n] = '\0';
      p = colon + 1;
      n = slash ? (size_t) (slash - p) : strlen (p);
      if (!n || n >= sizeof out->port)
        goto bad;
      memcpy (out->port, p, n);
      out->port[n] = '\0';
    }
  else
    {
      n = slash ? (size_t) (slash - p) : strlen (p);
      if (!n || n >= sizeof out->host)
        goto bad;
      memcpy (out->host, p, n);
      out->host[n] = '\0';
    }
  if (slash)
    {
      if (strlen (slash) >= sizeof out->path)
        goto bad;
      strcpy (out->path, slash);
    }
  else
    strcpy (out->path, "/");
  return TS_OK;
bad:
  ts_error_set (error, TS_ERR_INVAL, 0, 0, "http: bad URL");
  return TS_ERR_INVAL;
}

static int
http_dial (const url_t *u)
{
  struct addrinfo hints, *list = NULL, *ai;
  int fd = -1;
  memset (&hints, 0, sizeof hints);
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo (u->host, u->port, &hints, &list) != 0 || !list)
    return -1;
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
  return fd;
}

static void
send_all (int fd, const char *s, size_t n)
{
  while (n)
    {
      ssize_t r = send (fd, s, n, MSG_NOSIGNAL);
      if (r <= 0)
        {
          if (r < 0 && errno == EINTR)
            continue;
          break;
        }
      s += r;
      n -= (size_t) r;
    }
}

/* Fetch; returns malloc'd raw response (caller frees) or NULL. */
static char *
http_fetch (const url_t *u, const char *method, const char *body,
            const char *ctype, TS_Error *error)
{
  int fd = http_dial (u);
  ts_sbuf_t req, resp;
  char chunk[8192];
  ssize_t r;
  char *out;
  if (fd < 0)
    {
      ts_error_set (error, TS_ERR_NOTFOUND, 0, 0, "http: no route");
      return NULL;
    }
  ts_sbuf_init (&req);
  ts_sbuf_printf (&req, "%s %s HTTP/1.0\r\nHost: %s\r\n"
                        "User-Agent: termscript-stdlib/0.1\r\n"
                        "Connection: close\r\n",
                  method, u->path, u->host);
  if (body)
    {
      ts_sbuf_printf (&req, "Content-Length: %zu\r\n"
                            "Content-Type: %s\r\n",
                      strlen (body), ctype ? ctype : "text/plain");
    }
  ts_sbuf_str (&req, "\r\n");
  if (body)
    ts_sbuf_str (&req, body);
  send_all (fd, req.data ? req.data : "", req.len);
  ts_sbuf_free (&req);
  ts_sbuf_init (&resp);
  while ((r = recv (fd, chunk, sizeof chunk, 0)) > 0)
    if (ts_sbuf_put (&resp, chunk, (size_t) r) != 0)
      break;
  close (fd);
  out = ts_sbuf_take (&resp);
  if (!out)
    ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
  return out;
}

static int
resp_status (const char *resp)
{
  /* "HTTP/1.0 200 ..." */
  const char *sp = strchr (resp, ' ');
  if (!sp)
    return -1;
  return atoi (sp + 1);
}

static const char *
resp_body (const char *resp)
{
  const char *sep = strstr (resp, "\r\n\r\n");
  if (sep)
    return sep + 4;
  sep = strstr (resp, "\n\n");
  return sep ? sep + 2 : NULL;
}

static TS_Status
h_do (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
      TS_Error *error, const char *fname, const char *method,
      bool want_status, bool want_headers)
{
  const char *url, *body = NULL, *ctype = "text/plain";
  url_t u;
  char *resp;
  size_t lo = want_status || want_headers ? 1 : 1;
  size_t hi = method[0] == 'P' ? 3 : 1;
  (void) vm;
  if (ts_std_argc (vm, argc, lo, hi, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &url, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (method[0] == 'P')
    {
      if (argc >= 2 && ts_std_str (&argv[1], &body, error, fname) !=
          TS_OK)
        return TS_ERR_INVAL;
      if (argc == 3 && ts_std_str (&argv[2], &ctype, error, fname) !=
          TS_OK)
        return TS_ERR_INVAL;
      if (!body)
        body = "";
    }
  if (url_parse (url, &u, error) != TS_OK)
    return TS_ERR_INVAL;
  resp = http_fetch (&u, method, body, ctype, error);
  if (!resp)
    {
      if (!error || error->code == TS_OK)
        {
          ts_std_ret_nil (ret);
          return TS_OK;
        }
      return error->code;
    }
  if (want_status)
    {
      int st = resp_status (resp);
      free (resp);
      ts_std_ret_int (ret, (int64_t) st);
      return TS_OK;
    }
  if (want_headers)
    {
      const char *b = resp_body (resp);
      TS_Status st = b ? ts_std_ret_strn (ret, resp,
                                          (size_t) (b - resp), error) :
                         ts_std_ret_str (ret, resp, error);
      free (resp);
      return st;
    }
  {
    const char *b = resp_body (resp);
    TS_Status st = ts_std_ret_str (ret, b ? b : resp, error);
    free (resp);
    return st;
  }
}

static TS_Status
h_get (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
       TS_Error *e)
{
  (void) ud;
  return h_do (v, a, n, r, e, "get", "GET", false, false);
}

static TS_Status
h_status (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
          TS_Error *e)
{
  (void) ud;
  return h_do (v, a, n, r, e, "status", "GET", true, false);
}

static TS_Status
h_headers (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
           TS_Error *e)
{
  (void) ud;
  return h_do (v, a, n, r, e, "headers", "GET", false, true);
}

static TS_Status
h_post (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
        TS_Error *e)
{
  (void) ud;
  return h_do (v, a, n, r, e, "post", "POST", false, false);
}

static TS_Status
h_url_parse (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
             TS_Value *ret, TS_Error *error)
{
  const char *url;
  url_t u;
  char joined[2400];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "url_parse") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &url, error, "url_parse") != TS_OK)
    return TS_ERR_INVAL;
  if (url_parse (url, &u, error) != TS_OK)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  snprintf (joined, sizeof joined, "%s|%s|%s", u.host, u.port, u.path);
  return ts_std_ret_str (ret, joined, error);
}

static const TS_FuncDef http_funcs[] = {
  { "get", h_get, NULL },
  { "status", h_status, NULL },
  { "headers", h_headers, NULL },
  { "post", h_post, NULL },
  { "url_parse", h_url_parse, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_http_module = { "std.http", http_funcs };
