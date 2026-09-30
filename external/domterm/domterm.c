/* domterm.c -- version, status strings and error helpers. */
#include "domterm.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const dt_status_names[] = {
  "OK",
  "INVALID_ARGUMENT",
  "NO_MEMORY",
  "IO",
  "NOT_FOUND",
  "PARSE",
  "UNSUPPORTED",
  "SYSTEM",
  "EOF",
  "TIMEOUT",
  "WOULD_BLOCK",
  "CHILD_EXITED",
  "CANCELLED",
  "PROTOCOL",
  "LIMIT",
  "AUTH"
};

const char *
dt_status_string (DT_Status status)
{
  if (status < 0 ||
      status >= (DT_Status) (sizeof dt_status_names /
                             sizeof dt_status_names[0]))
    return "UNKNOWN";
  return dt_status_names[status];
}

void
dt_error_set (DT_Error *error, DT_Status code, int system_errno,
              size_t offset, const char *fmt, ...)
{
  va_list ap;
  if (!error)
    return;
  error->code = code;
  error->system_errno = system_errno;
  error->offset = offset;
  if (!fmt)
    {
      error->message[0] = '\0';
      return;
    }
  va_start (ap, fmt);
  vsnprintf (error->message, sizeof error->message, fmt, ap);
  va_end (ap);
}

void
dt_error_clear (DT_Error *error)
{
  if (!error)
    return;
  error->code = DT_OK;
  error->system_errno = 0;
  error->offset = 0;
  error->message[0] = '\0';
}

unsigned int
dt_version_number (void)
{
  return ((DT_VERSION_MAJOR << 16) |
          (DT_VERSION_MINOR << 8) | DT_VERSION_PATCH);
}

const char *
dt_version_string (void)
{
  static char buf[32];
  snprintf (buf, sizeof buf, "%d.%d.%d",
            DT_VERSION_MAJOR, DT_VERSION_MINOR, DT_VERSION_PATCH);
  return buf;
}

bool
dt_feature_query (const char *feature)
{
  static const char *const features[] = {
    "pty", "tty", "terminfo", "record", "replay",
    "local-conn", "remote-conn", "termscript", "puppeteer",
    NULL
  };
  size_t i;
  if (!feature)
    return false;
  for (i = 0; features[i]; i++)
    if (strcmp (feature, features[i]) == 0)
      return true;
  return false; /* e.g. "tls": raw remote TCP is never encrypted. */
}
