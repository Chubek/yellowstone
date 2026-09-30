/* expect.c -- std.expect: scripted interaction with a child.
 *
 * run() spawns /bin/sh -c with piped stdio; expect() waits (poll)
 * for a substring with a millisecond deadline and returns the text
 * through the end of the match; close() reaps with the exit code.
 */
#include "expect/expect.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
  pid_t pid;
  int wfd; /* to child stdin */
  int rfd; /* from child stdout (stderr merged) */
  ts_sbuf_t pending;
  bool reaped;
  int exit_code;
} expect_t;

static const char expect_tag_id = 0;

static void
expect_free (void *p)
{
  expect_t *h = p;
  int st;
  if (!h)
    return;
  if (!h->reaped && h->pid > 0)
    {
      kill (h->pid, SIGKILL);
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
    }
  if (h->wfd >= 0)
    close (h->wfd);
  if (h->rfd >= 0)
    close (h->rfd);
  ts_sbuf_free (&h->pending);
  free (h);
}

static int64_t
mono_ms (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static TS_Status
e_run (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *cmd;
  int to_child[2], from_child[2];
  pid_t pid;
  expect_t *h;
  char desc[96];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "run") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &cmd, error, "run") != TS_OK)
    return TS_ERR_INVAL;
  if (pipe (to_child) != 0 || pipe (from_child) != 0)
    {
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "run: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  pid = fork ();
  if (pid < 0)
    {
      int e = errno;
      close (to_child[0]);
      close (to_child[1]);
      close (from_child[0]);
      close (from_child[1]);
      ts_error_set (error, TS_ERR_SYSTEM, e, 0, "run: %s",
                    strerror (e));
      return TS_ERR_SYSTEM;
    }
  if (pid == 0)
    {
      dup2 (to_child[0], 0);
      dup2 (from_child[1], 1);
      dup2 (from_child[1], 2);
      close (to_child[0]);
      close (to_child[1]);
      close (from_child[0]);
      close (from_child[1]);
      execl ("/bin/sh", "sh", "-c", cmd, (char *) NULL);
      _exit (127);
    }
  close (to_child[0]);
  close (from_child[1]);
  h = calloc (1, sizeof *h);
  if (!h)
    {
      int st;
      kill (pid, SIGKILL);
      while (waitpid (pid, &st, 0) < 0 && errno == EINTR)
        ;
      close (to_child[1]);
      close (from_child[0]);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  h->pid = pid;
  h->wfd = to_child[1];
  h->rfd = from_child[0];
  ts_sbuf_init (&h->pending);
  snprintf (desc, sizeof desc, "<expect pid=%d>", (int) pid);
  if (ts_value_make_handle (ret, h, expect_free, desc,
                            &expect_tag_id) != TS_OK)
    {
      expect_free (h);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
e_expect (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  expect_t *h;
  const char *pat;
  int64_t timeout_ms = 5000;
  int64_t deadline;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 3, error, "expect") != TS_OK)
    return TS_ERR_INVAL;
  h = ts_std_handle (&argv[0], &expect_tag_id, error, "expect");
  if (!h || ts_std_str (&argv[1], &pat, error, "expect") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 3 && ts_std_int (&argv[2], &timeout_ms, error,
                               "expect") != TS_OK)
    return TS_ERR_INVAL;
  if (timeout_ms < 0 || timeout_ms > 600000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "expect: bad timeout");
      return TS_ERR_INVAL;
    }
  deadline = mono_ms () + timeout_ms;
  for (;;)
    {
      const char *hit;
      const char *base = h->pending.data ? h->pending.data : "";
      struct pollfd pf;
      char chunk[4096];
      ssize_t r;
      int pr;
      hit = strstr (base, pat);
      if (hit)
        {
          TS_Status st = ts_std_ret_strn (ret, base,
                                          (size_t) (hit - base) +
                                          strlen (pat), error);
          /* Consume through the match. */
          size_t used = (size_t) (hit - base) + strlen (pat);
          size_t rest = h->pending.len - used;
          memmove (h->pending.data, h->pending.data + used, rest);
          h->pending.len = rest;
          h->pending.data[rest] = '\0';
          return st;
        }
      pf.fd = h->rfd;
      pf.events = POLLIN;
      pf.revents = 0;
      pr = poll (&pf, 1, (int) (deadline - mono_ms ()));
      if (pr < 0)
        {
          if (errno == EINTR)
            continue;
          ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "expect: %s",
                        strerror (errno));
          return TS_ERR_SYSTEM;
        }
      if (!pr)
        {
          ts_std_ret_nil (ret); /* Deadline: nil. */
          return TS_OK;
        }
      r = read (h->rfd, chunk, sizeof chunk);
      if (r <= 0)
        {
          ts_std_ret_nil (ret); /* EOF: pattern never arrived. */
          return TS_OK;
        }
      if (ts_sbuf_put (&h->pending, chunk, (size_t) r) != 0)
        {
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                        "out of memory");
          return TS_ERR_NOMEM;
        }
      if (h->pending.len > (1u << 20))
        {
          ts_error_set (error, TS_ERR_LIMIT, 0, 0,
                        "expect: output too large");
          return TS_ERR_LIMIT;
        }
    }
}

static TS_Status
e_send (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  expect_t *h;
  const char *text;
  size_t n, sent = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  h = ts_std_handle (&argv[0], &expect_tag_id, error, "send");
  if (!h || ts_std_str (&argv[1], &text, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  if (h->wfd < 0)
    {
      ts_std_ret_bool (ret, false);
      return TS_OK;
    }
  n = strlen (text);
  while (sent < n)
    {
      ssize_t r = write (h->wfd, text + sent, n - sent);
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
e_close (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  expect_t *h;
  int st;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "close") != TS_OK)
    return TS_ERR_INVAL;
  h = ts_std_handle (&argv[0], &expect_tag_id, error, "close");
  if (!h)
    return TS_ERR_INVAL;
  if (h->wfd >= 0)
    {
      close (h->wfd);
      h->wfd = -1;
    }
  if (!h->reaped && h->pid > 0)
    {
      while (waitpid (h->pid, &st, 0) < 0 && errno == EINTR)
        ;
      h->reaped = true;
      h->exit_code = WIFEXITED (st) ? WEXITSTATUS (st) :
                     WIFSIGNALED (st) ? 128 + WTERMSIG (st) : 127;
    }
  if (h->rfd >= 0)
    {
      close (h->rfd);
      h->rfd = -1;
    }
  ts_std_ret_int (ret, h->exit_code);
  return TS_OK;
}

static const TS_FuncDef expect_funcs[] = {
  { "run", e_run, NULL },
  { "expect", e_expect, NULL },
  { "send", e_send, NULL },
  { "close", e_close, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_expect_module = { "std.expect", expect_funcs };
