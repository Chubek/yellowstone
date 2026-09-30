/* dt_puppeteer.c -- terminal session orchestration. */
#include "dt_internal.h"

#include <signal.h>

struct DT_Puppeteer
{
  DT_PTYSession *pty;
  DT_TIProf *profile;
  DT_Recorder *recorder; /* Borrowed. */
  volatile sig_atomic_t pending_resize;
  volatile sig_atomic_t pending_rows;
  volatile sig_atomic_t pending_cols;
};

void
dt_puppeteer_options_init (DT_PuppeteerOptions *options)
{
  if (!options)
    return;
  options->struct_size = (uint32_t) sizeof *options;
  dt_pty_options_init (&options->pty);
  options->terminal_name = NULL;
  options->record = false;
  options->recorder = NULL;
  options->reserved = 0;
}

DT_Puppeteer *
dt_puppeteer_create (const DT_PuppeteerOptions *options, DT_Error *error)
{
  DT_Puppeteer *p;
  if (!options)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL options");
      return NULL;
    }
  p = calloc (1, sizeof *p);
  if (!p)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  p->pty = dt_pty_spawn (&options->pty, error);
  if (!p->pty)
    {
      free (p);
      return NULL;
    }
  if (options->terminal_name)
    {
      DT_TIDB *db = dt_tidb_open_default (error);
      if (!db)
        {
          dt_pty_close (p->pty);
          free (p);
          return NULL;
        }
      p->profile = dt_tiprof_load (db, options->terminal_name, error);
      dt_tidb_close (db);
      if (!p->profile)
        {
          dt_pty_close (p->pty);
          free (p);
          return NULL;
        }
    }
  if (options->record)
    p->recorder = options->recorder; /* Borrowed (may be NULL). */
  return p;
}

void
dt_puppeteer_free (DT_Puppeteer *puppeteer)
{
  if (!puppeteer)
    return;
  dt_pty_close (puppeteer->pty);
  dt_tiprof_free (puppeteer->profile);
  free (puppeteer);
}

DT_PTYSession *
dt_puppeteer_pty (DT_Puppeteer *puppeteer)
{
  return puppeteer ? puppeteer->pty : NULL;
}

DT_TIProf *
dt_puppeteer_profile (DT_Puppeteer *puppeteer)
{
  return puppeteer ? puppeteer->profile : NULL;
}

DT_Recorder *
dt_puppeteer_recorder (DT_Puppeteer *puppeteer)
{
  return puppeteer ? puppeteer->recorder : NULL;
}

void
dt_puppeteer_request_resize (DT_Puppeteer *puppeteer, unsigned short rows,
                             unsigned short columns)
{
  if (!puppeteer)
    return;
  puppeteer->pending_rows = (sig_atomic_t) rows;
  puppeteer->pending_cols = (sig_atomic_t) columns;
  puppeteer->pending_resize = 1;
}

static DT_Status
dt_puppeteer_apply_pending (DT_Puppeteer *p, DT_Error *error)
{
  if (p->pending_resize)
    {
      unsigned short rows = (unsigned short) p->pending_rows;
      unsigned short cols = (unsigned short) p->pending_cols;
      DT_Status st;
      p->pending_resize = 0;
      st = dt_pty_resize (p->pty, rows, cols, error);
      if (st != DT_OK)
        return st;
      if (p->recorder)
        {
          DT_RecordEvent ev;
          ev.type = DT_RECORD_RESIZE;
          ev.timestamp_ns = 0;
          ev.data = NULL;
          ev.size = 0;
          ev.rows = rows;
          ev.columns = cols;
          st = dt_recorder_write (p->recorder, &ev, error);
          if (st != DT_OK)
            return st;
        }
    }
  return DT_OK;
}

DT_Status
dt_puppeteer_resize (DT_Puppeteer *puppeteer, unsigned short rows,
                     unsigned short columns, DT_Error *error)
{
  DT_Status st;
  if (!puppeteer)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL puppeteer");
      return DT_ERR_INVALID_ARGUMENT;
    }
  puppeteer->pending_resize = 0;
  st = dt_pty_resize (puppeteer->pty, rows, columns, error);
  if (st != DT_OK)
    return st;
  if (puppeteer->recorder)
    {
      DT_RecordEvent ev;
      ev.type = DT_RECORD_RESIZE;
      ev.timestamp_ns = 0;
      ev.data = NULL;
      ev.size = 0;
      ev.rows = rows;
      ev.columns = columns;
      return dt_recorder_write (puppeteer->recorder, &ev, error);
    }
  return DT_OK;
}

DT_Status
dt_puppeteer_pump_once (DT_Puppeteer *puppeteer, int timeout_ms,
                        DT_Error *error)
{
  struct pollfd pfd;
  int pr;
  uint8_t buf[65536];
  size_t nread = 0;
  DT_Status st;
  if (!puppeteer)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL puppeteer");
      return DT_ERR_INVALID_ARGUMENT;
    }
  st = dt_puppeteer_apply_pending (puppeteer, error);
  if (st != DT_OK)
    return st;
  pfd.fd = dt_pty_pollfd (puppeteer->pty);
  pfd.events = POLLIN;
  pfd.revents = 0;
  for (;;)
    {
      pr = poll (&pfd, 1, timeout_ms);
      if (pr < 0 && errno == EINTR)
        continue;
      break;
    }
  if (pr < 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "poll failed");
      return DT_ERR_SYSTEM;
    }
  if (pr == 0)
    {
      dt_err_set (error, DT_ERR_TIMEOUT, 0, 0, "pump idle");
      return DT_ERR_TIMEOUT;
    }
  st = dt_pty_read (puppeteer->pty, buf, sizeof buf, &nread, error);
  if (st == DT_ERR_WOULD_BLOCK)
    {
      dt_err_set (error, DT_ERR_TIMEOUT, 0, 0, "pump idle");
      return DT_ERR_TIMEOUT;
    }
  if (st != DT_OK)
    return st;
  if (puppeteer->recorder && nread)
    {
      DT_RecordEvent ev;
      ev.type = DT_RECORD_OUTPUT;
      ev.timestamp_ns = 0;
      ev.data = buf;
      ev.size = nread;
      ev.rows = 0;
      ev.columns = 0;
      return dt_recorder_write (puppeteer->recorder, &ev, error);
    }
  return DT_OK;
}

DT_Status
dt_puppeteer_wait (DT_Puppeteer *puppeteer, int *exit_code,
                   bool *exited_normally, DT_Error *error)
{
  if (!puppeteer)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL puppeteer");
      return DT_ERR_INVALID_ARGUMENT;
    }
  return dt_pty_wait (puppeteer->pty, exit_code, exited_normally, error);
}
