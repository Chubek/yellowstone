/* dt_record.c -- versioned session recording and deterministic replay. */
#include "dt_internal.h"

#include <time.h>

#define DT_RECORD_MAGIC "DMTR"
#define DT_RECORD_VERSION 1u

struct DT_Recorder
{
  FILE *f;
  bool own;
};

struct DT_Replayer
{
  FILE *f;
  bool own;
  DT_ReplayMode mode;
  DT_ReplayMode resume_mode;
  double speed;
  uint8_t *evbuf;
  size_t evcap;
  DT_RecordEvent cur;
  bool have_cur;
  char *compiled;
  bool started; /* Pacing baseline initialized. */
  uint64_t first_ts;
  uint64_t start_mono_ns;
};

const char *
dt_record_type_string (DT_RecordType type)
{
  switch (type)
    {
    case DT_RECORD_INPUT: return "INPUT";
    case DT_RECORD_OUTPUT: return "OUTPUT";
    case DT_RECORD_RESIZE: return "RESIZE";
    case DT_RECORD_META: return "META";
    case DT_RECORD_HEARTBEAT: return "HEARTBEAT";
    default: return "UNKNOWN";
    }
}

static uint64_t
dt_now_realtime_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_REALTIME, &ts);
  return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
}

static uint64_t
dt_now_mono_ns (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
}

DT_Recorder *
dt_recorder_create (FILE *stream, bool take_ownership, DT_Error *error)
{
  DT_Recorder *r;
  uint8_t hdr[8];
  if (!stream)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL stream");
      return NULL;
    }
  r = calloc (1, sizeof *r);
  if (!r)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  memcpy (hdr, DT_RECORD_MAGIC, 4);
  dt_put_u16le (hdr + 4, DT_RECORD_VERSION);
  dt_put_u16le (hdr + 6, 0);
  if (fwrite (hdr, 1, sizeof hdr, stream) != sizeof hdr)
    {
      int e = errno;
      free (r);
      dt_err_set (error, DT_ERR_IO, e, 0, "cannot write record header");
      return NULL;
    }
  r->f = stream;
  r->own = take_ownership;
  return r;
}

void
dt_recorder_free (DT_Recorder *recorder)
{
  if (!recorder)
    return;
  fflush (recorder->f);
  if (recorder->own)
    fclose (recorder->f);
  free (recorder);
}

DT_Status
dt_recorder_write (DT_Recorder *recorder, const DT_RecordEvent *event,
                   DT_Error *error)
{
  uint8_t rh[16];
  uint8_t resize_payload[4];
  const uint8_t *payload = NULL;
  size_t payload_len = 0;
  uint32_t crc;
  uint64_t ts;
  if (!recorder || !event)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  switch (event->type)
    {
    case DT_RECORD_INPUT:
    case DT_RECORD_OUTPUT:
    case DT_RECORD_META:
      payload = event->data;
      payload_len = event->size;
      if (payload_len && !payload)
        {
          dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                      "NULL payload data");
          return DT_ERR_INVALID_ARGUMENT;
        }
      break;
    case DT_RECORD_RESIZE:
      dt_put_u16le (resize_payload, event->rows);
      dt_put_u16le (resize_payload + 2, event->columns);
      payload = resize_payload;
      payload_len = 4;
      break;
    case DT_RECORD_HEARTBEAT:
      payload = NULL;
      payload_len = 0;
      break;
    default:
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "unknown record type %d", (int) event->type);
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (payload_len > DT_RECORD_MAX_PAYLOAD)
    {
      dt_err_set (error, DT_ERR_LIMIT, 0, 0, "record payload too large");
      return DT_ERR_LIMIT;
    }
  ts = event->timestamp_ns ? event->timestamp_ns : dt_now_realtime_ns ();
  rh[0] = (uint8_t) event->type;
  rh[1] = 0;
  rh[2] = 0;
  rh[3] = 0;
  dt_put_u64le (rh + 4, ts);
  dt_put_u32le (rh + 12, (uint32_t) payload_len);
  crc = dt_crc32_update (0xFFFFFFFFu, rh, sizeof rh);
  if (payload_len)
    crc = dt_crc32_update (crc, payload, payload_len);
  crc ^= 0xFFFFFFFFu;
  if (fwrite (rh, 1, sizeof rh, recorder->f) != sizeof rh ||
      (payload_len && fwrite (payload, 1, payload_len, recorder->f) !=
                      payload_len))
    {
      dt_err_set (error, DT_ERR_IO, errno, 0, "record write failed");
      return DT_ERR_IO;
    }
  {
    uint8_t crcle[4];
    dt_put_u32le (crcle, crc);
    if (fwrite (crcle, 1, 4, recorder->f) != 4)
      {
        dt_err_set (error, DT_ERR_IO, errno, 0, "record write failed");
        return DT_ERR_IO;
      }
  }
  return DT_OK;
}

DT_Status
dt_recorder_flush (DT_Recorder *recorder, DT_Error *error)
{
  if (!recorder)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL recorder");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (fflush (recorder->f) != 0)
    {
      dt_err_set (error, DT_ERR_IO, errno, 0, "flush failed");
      return DT_ERR_IO;
    }
  return DT_OK;
}

/* ---------------- replayer ---------------- */

static DT_Status
dt_read_exact (FILE *f, void *buf, size_t len, bool *eof)
{
  uint8_t *p = buf;
  size_t got = 0;
  *eof = false;
  while (got < len)
    {
      size_t r = fread (p + got, 1, len - got, f);
      if (r == 0)
        {
          if (ferror (f))
            return DT_ERR_IO;
          if (got == 0)
            {
              *eof = true;
              return DT_ERR_EOF;
            }
          return DT_ERR_PARSE; /* Truncated record. */
        }
      got += r;
    }
  return DT_OK;
}

DT_Replayer *
dt_replayer_create (FILE *stream, bool take_ownership, DT_Error *error)
{
  DT_Replayer *r;
  uint8_t hdr[8];
  bool eof = false;
  DT_Status st;
  if (!stream)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL stream");
      return NULL;
    }
  r = calloc (1, sizeof *r);
  if (!r)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  st = dt_read_exact (stream, hdr, sizeof hdr, &eof);
  if (st == DT_ERR_EOF)
    {
      free (r);
      dt_err_set (error, DT_ERR_EOF, 0, 0, "empty recording");
      return NULL;
    }
  if (st != DT_OK)
    {
      free (r);
      dt_err_set (error, DT_ERR_PARSE, 0, 0, "truncated record header");
      return NULL;
    }
  if (memcmp (hdr, DT_RECORD_MAGIC, 4) != 0)
    {
      free (r);
      dt_err_set (error, DT_ERR_PROTOCOL, 0, 0, "bad recording magic");
      return NULL;
    }
  if (dt_get_u16le (hdr + 4) != DT_RECORD_VERSION)
    {
      free (r);
      dt_err_set (error, DT_ERR_UNSUPPORTED, 0, 0,
                  "unsupported recording version %u", dt_get_u16le (hdr + 4));
      return NULL;
    }
  if (dt_get_u16le (hdr + 6) != 0)
    {
      free (r);
      dt_err_set (error, DT_ERR_PROTOCOL, 0, 0,
                  "reserved header flags must be zero");
      return NULL;
    }
  r->f = stream;
  r->own = take_ownership;
  r->mode = DT_REPLAY_FAST;
  r->resume_mode = DT_REPLAY_FAST;
  r->speed = 1.0;
  return r;
}

void
dt_replayer_free (DT_Replayer *replayer)
{
  if (!replayer)
    return;
  free (replayer->evbuf);
  free (replayer->compiled);
  if (replayer->own)
    fclose (replayer->f);
  free (replayer);
}

void
dt_replayer_release_event (DT_Replayer *replayer, DT_RecordEvent *event)
{
  if (!replayer || !event)
    return;
  if (replayer->have_cur)
    {
      replayer->have_cur = false;
      event->data = NULL;
      event->size = 0;
    }
}

DT_ReplayMode
dt_replayer_get_mode (const DT_Replayer *replayer)
{
  return replayer ? replayer->mode : DT_REPLAY_FAST;
}

void
dt_replayer_set_mode (DT_Replayer *replayer, DT_ReplayMode mode)
{
  if (!replayer || mode < DT_REPLAY_REALTIME || mode > DT_REPLAY_PAUSED)
    return;
  replayer->mode = mode;
  if (mode != DT_REPLAY_PAUSED)
    replayer->resume_mode = mode;
}

void
dt_replayer_set_speed (DT_Replayer *replayer, double speed)
{
  if (!replayer || !(speed > 0))
    return;
  replayer->speed = speed;
}

void
dt_replayer_pause (DT_Replayer *replayer)
{
  if (!replayer)
    return;
  if (replayer->mode != DT_REPLAY_PAUSED)
    replayer->resume_mode = replayer->mode;
  replayer->mode = DT_REPLAY_PAUSED;
}

void
dt_replayer_resume (DT_Replayer *replayer)
{
  if (!replayer)
    return;
  if (replayer->mode == DT_REPLAY_PAUSED)
    replayer->mode = replayer->resume_mode;
}

DT_Status
dt_replayer_rewind (DT_Replayer *replayer, DT_Error *error)
{
  if (!replayer)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL replayer");
      return DT_ERR_INVALID_ARGUMENT;
    }
  replayer->have_cur = false;
  replayer->started = false;
  if (fseek (replayer->f, 8, SEEK_SET) != 0)
    {
      dt_err_set (error, DT_ERR_SYSTEM, errno, 0, "rewind failed");
      return DT_ERR_SYSTEM;
    }
  clearerr (replayer->f);
  return DT_OK;
}

static DT_Status
dt_replayer_read_one (DT_Replayer *replayer, DT_RecordEvent *event,
                      DT_Error *error)
{
  uint8_t rh[16];
  bool eof = false;
  DT_Status st;
  uint32_t payload_len, crc_file, crc_calc;
  for (;;)
    {
      st = dt_read_exact (replayer->f, rh, sizeof rh, &eof);
      if (st == DT_ERR_EOF)
        {
          dt_err_set (error, DT_ERR_EOF, 0, 0, "end of recording");
          return DT_ERR_EOF;
        }
      if (st != DT_OK)
        {
          if (st == DT_ERR_IO)
            dt_err_set (error, DT_ERR_IO, errno, 0, "record read failed");
          else
            dt_err_set (error, DT_ERR_PARSE, 0, 0, "truncated record");
          return st == DT_ERR_IO ? DT_ERR_IO : DT_ERR_PARSE;
        }
      {
        unsigned type = rh[0];
        if (rh[1] != 0 || rh[2] != 0 || rh[3] != 0)
          {
            dt_err_set (error, DT_ERR_PROTOCOL, 0, 0,
                        "reserved record flags must be zero");
            return DT_ERR_PROTOCOL;
          }
        payload_len = dt_get_u32le (rh + 12);
        if (payload_len > DT_RECORD_MAX_PAYLOAD)
          {
            dt_err_set (error, DT_ERR_LIMIT, 0, 0,
                        "record payload too large");
            return DT_ERR_LIMIT;
          }
        if (type < DT_RECORD_INPUT || type > DT_RECORD_HEARTBEAT)
          {
            if (type < 0x80)
              {
                /* Forward-compatible informational type: skip. */
                size_t left = payload_len + 4;
                static uint8_t discard[4096];
                while (left)
                  {
                    size_t want = left < sizeof discard ? left :
                                                            sizeof discard;
                    bool deof = false;
                    DT_Status ds = dt_read_exact (replayer->f, discard,
                                                  want, &deof);
                    if (ds != DT_OK)
                      {
                        dt_err_set (error, DT_ERR_PARSE, 0, 0,
                                    "truncated skipped record");
                        return DT_ERR_PARSE;
                      }
                    left -= want;
                  }
                continue; /* Next record. */
              }
            dt_err_set (error, DT_ERR_PROTOCOL, 0, 0,
                        "reserved record type %u", type);
            return DT_ERR_PROTOCOL;
          }
        if (payload_len > replayer->evcap)
          {
            uint8_t *nb = realloc (replayer->evbuf, payload_len ?
                                                          payload_len : 1);
            if (!nb)
              {
                dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0,
                            "out of memory");
                return DT_ERR_NO_MEMORY;
              }
            replayer->evbuf = nb;
            replayer->evcap = payload_len;
          }
        if (payload_len)
          {
            st = dt_read_exact (replayer->f, replayer->evbuf, payload_len,
                                &eof);
            if (st != DT_OK)
              {
                dt_err_set (error, DT_ERR_PARSE, 0, 0,
                            "truncated record payload");
                return DT_ERR_PARSE;
              }
          }
        {
          uint8_t crcle[4];
          st = dt_read_exact (replayer->f, crcle, 4, &eof);
          if (st != DT_OK)
            {
              dt_err_set (error, DT_ERR_PARSE, 0, 0,
                          "truncated record checksum");
              return DT_ERR_PARSE;
            }
          crc_file = dt_get_u32le (crcle);
        }
        crc_calc = dt_crc32_update (0xFFFFFFFFu, rh, sizeof rh);
        if (payload_len)
          crc_calc = dt_crc32_update (crc_calc, replayer->evbuf,
                                      payload_len);
        crc_calc ^= 0xFFFFFFFFu;
        if (crc_calc != crc_file)
          {
            dt_err_set (error, DT_ERR_PROTOCOL, 0, 0,
                        "record checksum mismatch (corrupt recording)");
            return DT_ERR_PROTOCOL;
          }
        event->type = (DT_RecordType) type;
        event->timestamp_ns = dt_get_u64le (rh + 4);
        event->rows = 0;
        event->columns = 0;
        if (type == DT_RECORD_RESIZE)
          {
            if (payload_len != 4)
              {
                dt_err_set (error, DT_ERR_PARSE, 0, 0,
                            "bad resize payload");
                return DT_ERR_PARSE;
              }
            event->rows = dt_get_u16le (replayer->evbuf);
            event->columns = dt_get_u16le (replayer->evbuf + 2);
            event->data = NULL;
            event->size = 0;
          }
        else if (type == DT_RECORD_HEARTBEAT)
          {
            if (payload_len != 0)
              {
                dt_err_set (error, DT_ERR_PARSE, 0, 0,
                            "bad heartbeat payload");
                return DT_ERR_PARSE;
              }
            event->data = NULL;
            event->size = 0;
          }
        else
          {
            event->data = payload_len ? replayer->evbuf : NULL;
            event->size = payload_len;
          }
        replayer->have_cur = true;
        replayer->cur = *event;
        return DT_OK;
      }
    }
}

static void
dt_replay_pace (DT_Replayer *replayer, const DT_RecordEvent *event)
{
  uint64_t target_off, elapsed, want_ns, now;
  if (replayer->mode != DT_REPLAY_REALTIME)
    return;
  if (replayer->speed <= 0)
    return;
  if (!replayer->started)
    {
      replayer->first_ts = event->timestamp_ns;
      replayer->start_mono_ns = dt_now_mono_ns ();
      replayer->started = true;
      return;
    }
  if (event->timestamp_ns < replayer->first_ts)
    return;
  target_off = event->timestamp_ns - replayer->first_ts;
  target_off = (uint64_t) ((double) target_off / replayer->speed);
  now = dt_now_mono_ns ();
  elapsed = now >= replayer->start_mono_ns ? now - replayer->start_mono_ns :
                                             0;
  if (target_off <= elapsed)
    return;
  want_ns = target_off - elapsed;
  while (want_ns)
    {
      struct timespec ts;
      ts.tv_sec = (time_t) (want_ns / 1000000000u);
      ts.tv_nsec = (long) (want_ns % 1000000000u);
      if (nanosleep (&ts, &ts) == 0)
        break;
      if (errno != EINTR)
        break;
      want_ns = (uint64_t) ts.tv_sec * 1000000000u + (uint64_t) ts.tv_nsec;
    }
}

DT_Status
dt_replayer_next (DT_Replayer *replayer, DT_RecordEvent *event,
                  DT_Error *error)
{
  DT_Status st;
  if (!replayer || !event)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (replayer->mode == DT_REPLAY_PAUSED)
    {
      dt_err_set (error, DT_ERR_WOULD_BLOCK, 0, 0, "replay is paused");
      return DT_ERR_WOULD_BLOCK;
    }
  if (replayer->mode == DT_REPLAY_STEP)
    {
      dt_err_set (error, DT_ERR_WOULD_BLOCK, 0, 0,
                  "step mode: use dt_replayer_step");
      return DT_ERR_WOULD_BLOCK;
    }
  st = dt_replayer_read_one (replayer, event, error);
  if (st == DT_OK)
    dt_replay_pace (replayer, event);
  return st;
}

DT_Status
dt_replayer_step (DT_Replayer *replayer, DT_RecordEvent *event,
                  DT_Error *error)
{
  if (!replayer || !event)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (replayer->mode == DT_REPLAY_PAUSED)
    {
      dt_err_set (error, DT_ERR_WOULD_BLOCK, 0, 0, "replay is paused");
      return DT_ERR_WOULD_BLOCK;
    }
  return dt_replayer_read_one (replayer, event, error);
}

/* ---------------- compile (replay-program emitters) ---------------- */

typedef struct
{
  DT_RecordType type;
  uint64_t ts;
  uint8_t *data;
  size_t len;
  unsigned short rows, columns;
} crecord;

static void
dt_crecords_free (crecord *recs, size_t n)
{
  size_t i;
  for (i = 0; i < n; i++)
    free (recs[i].data);
  free (recs);
}

void
dt_buf_put (char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
  if (*len + n + 1 > *cap)
    {
      size_t nc = (*cap ? *cap * 2 : 1024);
      char *nb;
      while (nc < *len + n + 1)
        nc *= 2;
      nb = realloc (*buf, nc);
      if (!nb)
        return;
      *buf = nb;
      *cap = nc;
    }
  memcpy (*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = '\0';
}

void
dt_buf_str (char **buf, size_t *len, size_t *cap, const char *s)
{
  dt_buf_put (buf, len, cap, s, strlen (s));
}

static void
dt_buf_escaped (char **buf, size_t *len, size_t *cap, const uint8_t *d,
                size_t n, const char *fmt)
{
  /* fmt contains a single %02X-style placeholder, e.g. "\\x%02x". */
  char tmp[16];
  size_t i;
  for (i = 0; i < n; i++)
    {
      snprintf (tmp, sizeof tmp, fmt, d[i]);
      dt_buf_str (buf, len, cap, tmp);
    }
}

const char *
dt_replayer_compile (DT_Replayer *replayer, const char *tmplate,
                     DT_ReplayerTarget target)
{
  crecord *recs = NULL;
  size_t nrecs = 0, crecs = 0;
  char *out = NULL;
  size_t olen = 0, ocap = 0;
  char *body = NULL;
  size_t blen = 0, bcap = 0;
  size_t i;
  if (!replayer)
    return NULL;
  /* Drain the stream from the current position to EOF. */
  for (;;)
    {
      DT_RecordEvent ev;
      DT_Error err;
      DT_Status st;
      dt_error_clear (&err);
      st = dt_replayer_read_one (replayer, &ev, &err);
      if (st == DT_ERR_EOF)
        break;
      if (st != DT_OK)
        {
          dt_crecords_free (recs, nrecs);
          free (body);
          return NULL;
        }
      if (nrecs == crecs)
        {
          crecord *nr;
          crecs = crecs ? crecs * 2 : 16;
          nr = realloc (recs, crecs * sizeof *nr);
          if (!nr)
            {
              dt_crecords_free (recs, nrecs);
              free (body);
              return NULL;
            }
          recs = nr;
        }
      recs[nrecs].type = ev.type;
      recs[nrecs].ts = ev.timestamp_ns;
      recs[nrecs].rows = ev.rows;
      recs[nrecs].columns = ev.columns;
      recs[nrecs].len = ev.size;
      recs[nrecs].data = NULL;
      if (ev.size)
        {
          recs[nrecs].data = malloc (ev.size);
          if (!recs[nrecs].data)
            {
              dt_crecords_free (recs, nrecs);
              free (body);
              return NULL;
            }
          memcpy (recs[nrecs].data, ev.data, ev.size);
        }
      nrecs++;
    }

  /* Per-target body emitters. */
  switch (target)
    {
    case DT_CCTARGET_TERMSCRIPT:
      dt_buf_str (&body, &blen, &bcap, "# Generated by DomTERM\n");
      for (i = 0; i < nrecs; i++)
        {
          char tmp[128];
          if (recs[i].type == DT_RECORD_OUTPUT ||
              recs[i].type == DT_RECORD_INPUT)
            {
              snprintf (tmp, sizeof tmp, "G:puts \"");
              dt_buf_str (&body, &blen, &bcap, tmp);
              dt_buf_escaped (&body, &blen, &bcap, recs[i].data,
                              recs[i].len, "\\x%02x");
              dt_buf_str (&body, &blen, &bcap, "\";\n");
            }
          else if (recs[i].type == DT_RECORD_RESIZE)
            {
              snprintf (tmp, sizeof tmp, "# resize %u %u\n", recs[i].rows,
                        recs[i].columns);
              dt_buf_str (&body, &blen, &bcap, tmp);
            }
        }
      break;
    case DT_CCTARGET_JS:
      dt_buf_str (&body, &blen, &bcap,
                  "// Generated by DomTERM\nconst events = [\n");
      for (i = 0; i < nrecs; i++)
        {
          char tmp[160];
          snprintf (tmp, sizeof tmp,
                    "  {t: %llu, type: %d, data: \"",
                    (unsigned long long) recs[i].ts, (int) recs[i].type);
          dt_buf_str (&body, &blen, &bcap, tmp);
          dt_buf_escaped (&body, &blen, &bcap, recs[i].data, recs[i].len,
                          "\\x%02x");
          dt_buf_str (&body, &blen, &bcap, "\"},\n");
        }
      dt_buf_str (&body, &blen, &bcap,
                  "];\nlet i = 0;\nfunction step() {\n"
                  "  if (i >= events.length) return;\n"
                  "  const e = events[i++];\n"
                  "  if (e.type === 2) process.stdout.write(Buffer.from(e.data, 'binary'));\n"
                  "  setTimeout(step, 10);\n}\nstep();\n");
      break;
    case DT_CCTARGET_WASM:
      dt_buf_str (&body, &blen, &bcap,
                  ";; Generated by DomTERM\n(module\n"
                  "  (memory 1)\n  (func (export \"replay\"))\n");
      for (i = 0; i < nrecs; i++)
        {
          char tmp[128];
          snprintf (tmp, sizeof tmp, "  (data (i32.const %zu) \"",
                    i * 256);
          dt_buf_str (&body, &blen, &bcap, tmp);
          dt_buf_escaped (&body, &blen, &bcap, recs[i].data, recs[i].len,
                          "\\%02x");
          dt_buf_str (&body, &blen, &bcap, "\")\n");
        }
      dt_buf_str (&body, &blen, &bcap, ")\n");
      break;
    case DT_CCTARGET_WEBGPU:
      dt_buf_str (&body, &blen, &bcap,
                  "// Generated by DomTERM (WebGPU replay stub)\n"
                  "const events = [\n");
      for (i = 0; i < nrecs; i++)
        {
          char tmp[160];
          snprintf (tmp, sizeof tmp, "  {t: %llu, data: \"",
                    (unsigned long long) recs[i].ts);
          dt_buf_str (&body, &blen, &bcap, tmp);
          dt_buf_escaped (&body, &blen, &bcap, recs[i].data, recs[i].len,
                          "\\x%02x");
          dt_buf_str (&body, &blen, &bcap, "\"},\n");
        }
      dt_buf_str (&body, &blen, &bcap,
                  "];\nasync function replay(device) {\n"
                  "  // wgpu: upload event bytes as texture/buffer data\n"
                  "  for (const e of events) { await device.queue.onSubmittedWorkDone(); }\n"
                  "}\n");
      break;
    case DT_CCTARGET_MANIM:
      dt_buf_str (&body, &blen, &bcap,
                  "# Generated by DomTERM\nfrom manim import *\n\n"
                  "class Replay(Scene):\n    def construct(self):\n");
      for (i = 0; i < nrecs; i++)
        {
          if (recs[i].type != DT_RECORD_OUTPUT)
            continue;
          dt_buf_str (&body, &blen, &bcap, "        self.add(Text(\"");
          dt_buf_escaped (&body, &blen, &bcap, recs[i].data, recs[i].len,
                          "\\x%02x");
          dt_buf_str (&body, &blen, &bcap, "\"))\n");
        }
      break;
    case DT_CCTARGET_PROCESSING:
      dt_buf_str (&body, &blen, &bcap,
                  "// Generated by DomTERM (p5.js replay)\nlet events = [\n");
      for (i = 0; i < nrecs; i++)
        {
          char tmp[160];
          snprintf (tmp, sizeof tmp, "  {t: %llu, s: \"",
                    (unsigned long long) recs[i].ts);
          dt_buf_str (&body, &blen, &bcap, tmp);
          dt_buf_escaped (&body, &blen, &bcap, recs[i].data, recs[i].len,
                          "\\x%02x");
          dt_buf_str (&body, &blen, &bcap, "\"},\n");
        }
      dt_buf_str (&body, &blen, &bcap,
                  "];\nfunction setup() { createCanvas(640, 480); }\n"
                  "function draw() { /* replay events */ }\n");
      break;
    default:
      dt_crecords_free (recs, nrecs);
      free (body);
      return NULL;
    }
  dt_crecords_free (recs, nrecs);

  /* Optional template substitution of {{events}}. */
  if (tmplate && strstr (tmplate, "{{events}}"))
    {
      const char *ph = strstr (tmplate, "{{events}}");
      dt_buf_put (&out, &olen, &ocap, tmplate, (size_t) (ph - tmplate));
      dt_buf_str (&out, &olen, &ocap, body ? body : "");
      dt_buf_str (&out, &olen, &ocap, ph + strlen ("{{events}}"));
    }
  else if (tmplate)
    {
      dt_buf_str (&out, &olen, &ocap, tmplate);
      dt_buf_str (&out, &olen, &ocap, body ? body : "");
    }
  else
    dt_buf_str (&out, &olen, &ocap, body ? body : "");
  free (body);
  if (!out)
    return NULL;
  free (replayer->compiled);
  replayer->compiled = out;
  return replayer->compiled;
}
