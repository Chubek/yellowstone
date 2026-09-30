/* dt_tiexpand.c -- Terminfo parameter-expansion engine (self-contained).
 *
 * Implements the Terminfo parameterized-string language documented in
 * terminfo(5) without depending on any terminal library.  Semantics are
 * strict and documented in domterm.h; known intentional differences from
 * historical implementations: stack underflow, div/mod by zero, type
 * errors and unknown directives are hard errors rather than silent
 * quirks; %-style flags follow the documented printf subset. */
#include "dt_internal.h"

typedef struct
{
  bool is_str;
  int32_t num;
  const char *str; /* Borrowed. */
} val_t;

typedef struct
{
  const char *s;
  size_t len;
  size_t pos;
  char out[DT_TI_MAX_EXPAND + 1];
  size_t outlen;
  unsigned steps;
  DT_TIParam params[9];
  val_t dyn[26];
  val_t stat[26];
  val_t stack[DT_TI_MAX_STACK];
  unsigned sp;
  DT_Status status;
  DT_Error *err;
  /* Conditional frames for executing (non-skipped) code.  EXPECT_T
     means `%?` seen without `%t` yet; THEN/ELSE track the live branch.
     `%t` is accepted in any live state so Algol-68 else-if chains
     (`%e cond %t body ...`) work; `%e`/`%;` in EXPECT_T are stray. */
  struct
  {
    int state; /* 0 = EXPECT_T, 1 = THEN, 2 = ELSE. */
  } frames[32];
  unsigned depth;
} exp_t;

static void
exp_fail (exp_t *e, DT_Status st, const char *fmt, ...)
{
  va_list ap;
  if (e->status == DT_OK)
    {
      e->status = st;
      if (e->err)
        {
          e->err->code = st;
          e->err->system_errno = 0;
          e->err->offset = e->pos;
          va_start (ap, fmt);
          vsnprintf (e->err->message, sizeof e->err->message, fmt, ap);
          va_end (ap);
        }
    }
}

static DT_Status
exp_emit (exp_t *e, const void *data, size_t n)
{
  if (e->status != DT_OK)
    return e->status;
  if (e->outlen + n > DT_TI_MAX_EXPAND)
    {
      exp_fail (e, DT_ERR_LIMIT, "expansion exceeds %u bytes",
                DT_TI_MAX_EXPAND);
      return e->status;
    }
  memcpy (e->out + e->outlen, data, n);
  e->outlen += n;
  return DT_OK;
}

static DT_Status
exp_emit_byte (exp_t *e, char c)
{
  return exp_emit (e, &c, 1);
}

static DT_Status
exp_step (exp_t *e)
{
  if (e->status != DT_OK)
    return e->status;
  if (++e->steps > DT_TI_MAX_STEPS)
    {
      exp_fail (e, DT_ERR_LIMIT, "expansion step limit exceeded");
      return e->status;
    }
  return DT_OK;
}

static DT_Status
exp_push (exp_t *e, val_t v)
{
  if (e->sp >= DT_TI_MAX_STACK)
    {
      exp_fail (e, DT_ERR_PARSE, "parameter stack overflow");
      return e->status;
    }
  e->stack[e->sp++] = v;
  return DT_OK;
}

static DT_Status
exp_pop (exp_t *e, val_t *v)
{
  if (e->sp == 0)
    {
      exp_fail (e, DT_ERR_PARSE, "parameter stack underflow");
      return e->status;
    }
  *v = e->stack[--e->sp];
  return DT_OK;
}

static DT_Status
exp_pop_num (exp_t *e, int32_t *n)
{
  val_t v;
  if (exp_pop (e, &v) != DT_OK)
    return e->status;
  if (v.is_str)
    {
      exp_fail (e, DT_ERR_PARSE, "number expected, string found");
      return e->status;
    }
  *n = v.num;
  return DT_OK;
}

static const char *
val_str (val_t *v, char *numbuf, size_t cap)
{
  if (v->is_str)
    return v->str ? v->str : "";
  snprintf (numbuf, cap, "%d", (int) v->num);
  return numbuf;
}

/* Skip forward to the next structural delimiter at nesting depth 0
   relative to entry.  When stop_at_test is true (false-branch of %t),
   stops at %t, %e or %;  (returned as 't', 'e', ';'); otherwise (dead
   then/else-branch after %e) only %; stops the skip, so `%t`/`%e`
   inside dead else-if code cannot end it early.  Leaves e->pos after
   the delimiter (callers rewind for 't' when needed), or returns 0 on
   error. */
static int
exp_skip (exp_t *e, bool stop_at_test)
{
  unsigned nest = 0;
  while (e->pos < e->len)
    {
      char c = e->s[e->pos];
      if (c != '%')
        {
          e->pos++;
          continue;
        }
      if (exp_step (e) != DT_OK)
        return 0;
      e->pos++;
      if (e->pos >= e->len)
        {
          exp_fail (e, DT_ERR_PARSE, "dangling %% in skipped region");
          return 0;
        }
      c = e->s[e->pos++];
      if (c == '?')
        nest++;
      else if (c == ';')
        {
          if (nest == 0)
            return ';';
          nest--;
        }
      else if (stop_at_test && (c == 't' || c == 'e') && nest == 0)
        return c;
      /* All other directives (including printf specs) are skipped
         opaquely: consume a full printf spec so a nested %; ... cannot
         occur inside one (it cannot: specs end at the conversion). */
      else if (c == ':' || c == '-' || c == '+' || c == ' ' || c == '#' ||
               c == '0' || (c >= '0' && c <= '9') || c == '.')
        {
          /* Rewind one and fall through to printf-spec skipping below. */
          e->pos--;
          goto printf_spec;
        }
      else if (c == '\'')
        {
          if (e->pos < e->len)
            e->pos++; /* Char constant body. */
          if (e->pos < e->len && e->s[e->pos] == '\'')
            e->pos++;
        }
      else if (c == '{')
        {
          while (e->pos < e->len && e->s[e->pos] != '}')
            e->pos++;
          if (e->pos < e->len)
            e->pos++;
        }
      continue;
printf_spec:
      {
        /* Consume [flags][width][.prec][conv] so delimiters can't hide. */
        while (e->pos < e->len)
          {
            char k = e->s[e->pos];
            if (k == ':' || k == '-' || k == '+' || k == ' ' || k == '#' ||
                k == '0' || (k >= '0' && k <= '9') || k == '.' || k == '*')
              e->pos++;
            else
              break;
          }
        if (e->pos < e->len)
          e->pos++; /* Conversion char. */
        continue;
      }
    }
  exp_fail (e, DT_ERR_PARSE, "unterminated conditional");
  return 0;
}

static DT_Status
exp_format (exp_t *e, const char *spec, size_t speclen, val_t v)
{
  char fmt[64];
  char numbuf[64];
  const char *str = NULL;
  char outbuf[512];
  int need;
  size_t k;
  char conv;
  if (speclen + 2 > sizeof fmt)
    {
      exp_fail (e, DT_ERR_PARSE, "format spec too long");
      return e->status;
    }
  /* Validate the spec so libc printf quirks cannot leak through:
     optional ':' introducer, flags [-+# space 0], width, .precision,
     one conversion.  Anything else (e.g. '*') is a parse error. */
  {
    size_t i = 0;
    if (i < speclen && spec[i] == ':')
      i++;
    while (i + 1 < speclen && (spec[i] == '-' || spec[i] == '+' ||
                               spec[i] == ' ' || spec[i] == '#' ||
                               spec[i] == '0'))
      i++;
    while (i + 1 < speclen && spec[i] >= '0' && spec[i] <= '9')
      i++;
    if (i + 1 < speclen && spec[i] == '.')
      {
        i++;
        while (i + 1 < speclen && spec[i] >= '0' && spec[i] <= '9')
          i++;
      }
    if (i + 1 != speclen)
      {
        exp_fail (e, DT_ERR_PARSE, "bad format spec");
        return e->status;
      }
    if (spec[0] == ':')
      {
        spec++;
        speclen--;
      }
  }
  fmt[0] = '%';
  memcpy (fmt + 1, spec, speclen);
  fmt[speclen + 1] = '\0';
  conv = spec[speclen - 1];
  if (conv == 's')
    {
      char tmp[64];
      const char *sv;
      if (!v.is_str)
        {
          exp_fail (e, DT_ERR_PARSE, "%%s needs a string operand");
          return e->status;
        }
      sv = val_str (&v, tmp, sizeof tmp);
      /* Honor precision manually to stay in bounded buffers. */
      size_t maxlen = strlen (sv);
      const char *dot = strchr (fmt, '.');
      if (dot)
        {
          unsigned long prec = strtoul (dot + 1, NULL, 10);
          if (prec < maxlen)
            maxlen = (size_t) prec;
        }
      /* Build with precision cap via a bounded copy. */
      {
        char *cp = malloc (maxlen + 1);
        char wfmt[64];
        if (!cp)
          {
            exp_fail (e, DT_ERR_NO_MEMORY, "out of memory");
            return e->status;
          }
        memcpy (cp, sv, maxlen);
        cp[maxlen] = '\0';
        /* Strip precision from fmt (already applied above). */
        {
          size_t fi = 0, fo = 0;
          while (fmt[fi] && fo + 1 < sizeof wfmt)
            {
              if (fmt[fi] == '.' && fo > 0)
                {
                  fi++;
                  while (fmt[fi] >= '0' && fmt[fi] <= '9')
                    fi++;
                  continue;
                }
              wfmt[fo++] = fmt[fi++];
            }
          wfmt[fo] = '\0';
          need = snprintf (outbuf, sizeof outbuf, wfmt, cp);
        }
        free (cp);
        if (need < 0)
          {
            exp_fail (e, DT_ERR_PARSE, "format failure");
            return e->status;
          }
        if ((size_t) need >= sizeof outbuf)
          {
            exp_fail (e, DT_ERR_LIMIT, "formatted field too large");
            return e->status;
          }
        return exp_emit (e, outbuf, (size_t) need);
      }
    }
  if (v.is_str)
    {
      exp_fail (e, DT_ERR_PARSE, "number expected, string found");
      return e->status;
    }
  if (conv == 'c')
    {
      char ch;
      if (v.is_str)
        {
          const char *sv = v.str ? v.str : "";
          if (!sv[0])
            {
              exp_fail (e, DT_ERR_PARSE, "%%c of empty string");
              return e->status;
            }
          ch = sv[0];
        }
      else
        ch = (char) v.num;
      (void) numbuf;
      (void) str;
      /* Width with %c: pad manually. */
      {
        unsigned long width = 0;
        const char *p = fmt + 1;
        bool left = false;
        if (*p == ':')
          p++;
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' ||
               *p == '0')
          {
            if (*p == '-')
              left = true;
            p++;
          }
        while (*p >= '0' && *p <= '9')
          {
            width = width * 10 + (unsigned long) (*p - '0');
            p++;
          }
        if (width > 1)
          {
            if (width - 1 + 1 >= sizeof outbuf)
              {
                exp_fail (e, DT_ERR_LIMIT, "formatted field too large");
                return e->status;
              }
            if (left)
              {
                outbuf[0] = ch;
                memset (outbuf + 1, ' ', width - 1);
              }
            else
              {
                memset (outbuf, ' ', width - 1);
                outbuf[width - 1] = ch;
              }
            return exp_emit (e, outbuf, (size_t) width);
          }
      }
      return exp_emit_byte (e, ch);
    }
  /* Numeric conversions. */
  if (conv == 'd')
    need = snprintf (outbuf, sizeof outbuf, fmt, (int) v.num);
  else
    need = snprintf (outbuf, sizeof outbuf, fmt, (unsigned) v.num);
  if (need < 0)
    {
      exp_fail (e, DT_ERR_PARSE, "format failure");
      return e->status;
    }
  if ((size_t) need >= sizeof outbuf)
    {
      /* Retry with an exact-size buffer (still subject to output cap). */
      char *big = malloc ((size_t) need + 1);
      if (!big)
        {
          exp_fail (e, DT_ERR_NO_MEMORY, "out of memory");
          return e->status;
        }
      if (conv == 'd')
        snprintf (big, (size_t) need + 1, fmt, (int) v.num);
      else
        snprintf (big, (size_t) need + 1, fmt, (unsigned) v.num);
      exp_emit (e, big, (size_t) need);
      free (big);
      return e->status;
    }
  for (k = 0; outbuf[k]; k++)
    ;
  return exp_emit (e, outbuf, (size_t) need);
}

static bool
is_flag_char (char c)
{
  return c == ':' || c == '-' || c == '+' || c == ' ' || c == '#' ||
         c == '0';
}

static DT_Status
exp_run (exp_t *e)
{
  while (e->pos < e->len && e->status == DT_OK)
    {
      char c = e->s[e->pos];
      if (c != '%')
        {
          exp_emit_byte (e, c);
          e->pos++;
          continue;
        }
      if (exp_step (e) != DT_OK)
        return e->status;
      e->pos++; /* Consume '%'. */
      if (e->pos >= e->len)
        {
          exp_fail (e, DT_ERR_PARSE, "dangling %% at end of string");
          return e->status;
        }
      c = e->s[e->pos++];
      switch (c)
        {
        case '%':
          exp_emit_byte (e, '%');
          break;
        case 'p':
          {
            if (e->pos >= e->len || e->s[e->pos] < '1' ||
                e->s[e->pos] > '9')
              {
                exp_fail (e, DT_ERR_PARSE, "bad %%p parameter");
                return e->status;
              }
            {
              unsigned idx = (unsigned) (e->s[e->pos++] - '1');
              val_t v;
              v.is_str = e->params[idx].is_string;
              v.num = e->params[idx].number;
              v.str = e->params[idx].string;
              exp_push (e, v);
            }
          }
          break;
        case 'P':
        case 'g':
          {
            bool is_set = (c == 'P');
            bool lower;
            unsigned idx;
            if (e->pos >= e->len ||
                !((e->s[e->pos] >= 'a' && e->s[e->pos] <= 'z') ||
                  (e->s[e->pos] >= 'A' && e->s[e->pos] <= 'Z')))
              {
                exp_fail (e, DT_ERR_PARSE, "bad variable name");
                return e->status;
              }
            c = e->s[e->pos++];
            lower = c >= 'a';
            idx = (unsigned) (lower ? c - 'a' : c - 'A');
            if (is_set)
              {
                val_t v;
                if (exp_pop (e, &v) != DT_OK)
                  return e->status;
                if (lower)
                  e->dyn[idx] = v;
                else
                  e->stat[idx] = v;
              }
            else
              exp_push (e, lower ? e->dyn[idx] : e->stat[idx]);
          }
          break;
        case '\'':
          {
            val_t v;
            if (e->pos >= e->len)
              {
                exp_fail (e, DT_ERR_PARSE, "unterminated char constant");
                return e->status;
              }
            v.is_str = false;
            v.num = (unsigned char) e->s[e->pos++];
            v.str = NULL;
            if (e->pos >= e->len || e->s[e->pos] != '\'')
              {
                exp_fail (e, DT_ERR_PARSE, "unterminated char constant");
                return e->status;
              }
            e->pos++;
            exp_push (e, v);
          }
          break;
        case '{':
          {
            int32_t acc = 0;
            bool any = false;
            while (e->pos < e->len && e->s[e->pos] >= '0' &&
                   e->s[e->pos] <= '9')
              {
                any = true;
                acc = acc * 10 + (e->s[e->pos++] - '0');
              }
            if (e->pos >= e->len || e->s[e->pos] != '}')
              {
                exp_fail (e, DT_ERR_PARSE, "bad integer constant");
                return e->status;
              }
            e->pos++;
            {
              val_t v;
              v.is_str = false;
              v.num = any ? acc : 0;
              v.str = NULL;
              exp_push (e, v);
            }
          }
          break;
        case 'l':
          {
            val_t v;
            char tmp[64];
            const char *sv;
            val_t r;
            if (exp_pop (e, &v) != DT_OK)
              return e->status;
            if (!v.is_str)
              {
                exp_fail (e, DT_ERR_PARSE, "%%l needs a string");
                return e->status;
              }
            sv = v.str ? v.str : "";
            (void) tmp;
            r.is_str = false;
            r.num = (int32_t) strlen (sv);
            r.str = NULL;
            exp_push (e, r);
          }
          break;
        case '+':
        case '-':
        case '*':
        case '/':
        case 'm':
        case '&':
        case '|':
        case '^':
        case '=':
        case '>':
        case '<':
        case 'A':
        case 'O':
          {
            int32_t x, y, r = 0;
            if (exp_pop_num (e, &y) != DT_OK ||
                exp_pop_num (e, &x) != DT_OK)
              return e->status;
            switch (c)
              {
              case '+': r = x + y; break;
              case '-': r = x - y; break;
              case '*': r = x * y; break;
              case '/':
                if (y == 0)
                  {
                    exp_fail (e, DT_ERR_PARSE, "division by zero");
                    return e->status;
                  }
                r = x / y;
                break;
              case 'm':
                if (y == 0)
                  {
                    exp_fail (e, DT_ERR_PARSE, "modulo by zero");
                    return e->status;
                  }
                r = x % y;
                break;
              case '&': r = x & y; break;
              case '|': r = x | y; break;
              case '^': r = x ^ y; break;
              case '=': r = x == y; break;
              case '>': r = x > y; break;
              case '<': r = x < y; break;
              case 'A': r = (x && y); break;
              case 'O': r = (x || y); break;
              }
            {
              val_t v;
              v.is_str = false;
              v.num = r;
              v.str = NULL;
              exp_push (e, v);
            }
          }
          break;
        case '!':
        case '~':
          {
            int32_t x;
            val_t v;
            if (exp_pop_num (e, &x) != DT_OK)
              return e->status;
            v.is_str = false;
            v.num = c == '!' ? !x : ~x;
            v.str = NULL;
            exp_push (e, v);
          }
          break;
        case 'i':
          e->params[0].number++;
          e->params[1].number++;
          break;
        case '?':
          if (e->depth >= 32)
            {
              exp_fail (e, DT_ERR_LIMIT, "conditionals nested too deep");
              return e->status;
            }
          e->frames[e->depth++].state = 0;
          break;
        case 't':
          {
            int32_t cond;
            int delim;
            if (e->depth == 0)
              {
                exp_fail (e, DT_ERR_PARSE, "stray %%t");
                return e->status;
              }
            if (exp_pop_num (e, &cond) != DT_OK)
              return e->status;
            if (cond)
              {
                e->frames[e->depth - 1].state = 1;
                break; /* Keep executing the then-branch. */
              }
            delim = exp_skip (e, true);
            if (delim == 0)
              return e->status;
            if (delim == 'e')
              e->frames[e->depth - 1].state = 2;
            else if (delim == ';')
              e->depth--; /* Conditional done. */
            else /* 't': rewind so the test executes normally. */
              {
                e->pos -= 2;
                e->frames[e->depth - 1].state = 0;
              }
          }
          break;
        case 'e':
          {
            int delim;
            if (e->depth == 0 || e->frames[e->depth - 1].state == 0)
              {
                exp_fail (e, DT_ERR_PARSE, "stray %%e");
                return e->status;
              }
            /* Executing a then- or else-branch: abandon it through the
               matching `%;`.  Inner `%t`/`%e` inside dead else-if code
               must not stop the skip. */
            delim = exp_skip (e, false);
            if (delim == 0)
              return e->status;
            if (delim != ';')
              {
                exp_fail (e, DT_ERR_PARSE, "expected %%; after %%e");
                return e->status;
              }
            e->depth--;
          }
          break;
        case ';':
          if (e->depth == 0 || e->frames[e->depth - 1].state == 0)
            {
              exp_fail (e, DT_ERR_PARSE, "stray %%;");
              return e->status;
            }
          e->depth--;
          break;
        default:
          {
            /* Printf-style spec or unsupported directive.  Note that
               '%-' and '%+' lex as binary operators (only '%:-' style
               introduces a '-' flag); c is already consumed. */
            size_t specstart = e->pos - 1; /* Index of c. */
            size_t q;
            if (c == 'd' || c == 'o' || c == 'x' || c == 'X' ||
                c == 's' || c == 'c')
              {
                q = specstart; /* Single-char spec. */
              }
            else
              {
                /* A conversion must start with a flag, width, precision
                   or the ':' introducer; anything else (e.g. `%[`, `%S`,
                   `%D`) is an unsupported directive, not a bad spec. */
                if (!is_flag_char (c) && !(c >= '0' && c <= '9') &&
                    c != '.')
                  {
                    exp_fail (e, DT_ERR_UNSUPPORTED,
                              "unsupported directive %%%c", c);
                    return e->status;
                  }
                q = e->pos;
                while (q < e->len && (is_flag_char (e->s[q]) ||
                                      e->s[q] == '*'))
                  q++;
                while (q < e->len && e->s[q] >= '0' && e->s[q] <= '9')
                  q++;
                if (q < e->len && e->s[q] == '.')
                  {
                    q++;
                    while (q < e->len && e->s[q] >= '0' && e->s[q] <= '9')
                      q++;
                  }
                if (q >= e->len || (e->s[q] != 'd' && e->s[q] != 'o' &&
                                    e->s[q] != 'x' && e->s[q] != 'X' &&
                                    e->s[q] != 's' && e->s[q] != 'c'))
                  {
                    exp_fail (e, DT_ERR_UNSUPPORTED,
                              "unsupported directive %%%c", c);
                    return e->status;
                  }
              }
            {
              val_t v;
              size_t speclen = q - specstart + 1;
              if (exp_pop (e, &v) != DT_OK)
                return e->status;
              e->pos = q + 1;
              if (exp_format (e, e->s + specstart, speclen, v) != DT_OK)
                return e->status;
            }
          }
          break;
        }
    }
  if (e->status == DT_OK && e->depth != 0)
    exp_fail (e, DT_ERR_PARSE, "unterminated conditional");
  return e->status;
}

size_t
dt_tiprof_expand_params (const DT_TIProf *profile, const char *capability,
                         const DT_TIParam *params, size_t param_count,
                         char *output, size_t output_size, DT_Error *error)
{
  exp_t e;
  DT_TIValue v;
  size_t i;
  const char *capstr;
  if (!profile || !capability)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad arguments");
      return SIZE_MAX;
    }
  if (param_count && !params)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad params");
      return SIZE_MAX;
    }
  if (!dt_tiprof_get (profile, capability, &v))
    {
      DT_TICapInfo info;
      if (dt_tiprof_cap_info (profile, capability, &info))
        dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                    "capability '%s' is not a string", capability);
      else
        dt_err_set (error, DT_ERR_NOT_FOUND, 0, 0,
                    "capability '%s' not found", capability);
      return SIZE_MAX;
    }
  if (v.type != DT_TI_STRING)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "capability '%s' is not a string", capability);
      return SIZE_MAX;
    }
  capstr = v.as.string;
  memset (&e, 0, sizeof e);
  e.s = capstr;
  e.len = strlen (capstr);
  e.err = error;
  for (i = 0; i < 9; i++)
    {
      if (i < param_count && params)
        e.params[i] = params[i];
      else
        {
          e.params[i].is_string = false;
          e.params[i].number = 0;
          e.params[i].string = NULL;
        }
      if (e.params[i].is_string && !e.params[i].string)
        e.params[i].string = "";
    }
  if (exp_run (&e) != DT_OK)
    return SIZE_MAX;
  e.out[e.outlen] = '\0';
  if (output && output_size)
    {
      size_t copy = e.outlen < output_size - 1 ? e.outlen : output_size - 1;
      memcpy (output, e.out, copy);
      output[copy] = '\0';
    }
  return e.outlen;
}

size_t
dt_tiprof_expand (const DT_TIProf *profile, const char *capability,
                  const int32_t *params, size_t param_count,
                  char *output, size_t output_size, DT_Error *error)
{
  DT_TIParam conv[9];
  size_t i, n;
  if (param_count && !params)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad params");
      return SIZE_MAX;
    }
  n = param_count < 9 ? param_count : 9;
  for (i = 0; i < n; i++)
    {
      conv[i].is_string = false;
      conv[i].number = params[i];
      conv[i].string = NULL;
    }
  return dt_tiprof_expand_params (profile, capability, conv, n, output,
                                  output_size, error);
}
