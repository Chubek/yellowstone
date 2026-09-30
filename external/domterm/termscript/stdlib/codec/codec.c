/* codec.c -- std.codec: text encodings and small string utilities.
 *
 * base64/hex/url codecs plus crc32 and everyday predicates; the
 * Termscript side (codec.tsc) composes them (e.g. hex-of-base64).
 */
#include "codec/codec.h"

#include "common/ts_std_common.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char b64_table[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static TS_Status
c_b64enc (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  size_t n, i;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "b64enc") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "b64enc") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (s);
  ts_sbuf_init (&b);
  for (i = 0; i < n; i += 3)
    {
      unsigned a = (unsigned char) s[i];
      unsigned bb = i + 1 < n ? (unsigned char) s[i + 1] : 0;
      unsigned c = i + 2 < n ? (unsigned char) s[i + 2] : 0;
      unsigned triple = (a << 16) | (bb << 8) | c;
      char q[4];
      q[0] = b64_table[(triple >> 18) & 63];
      q[1] = b64_table[(triple >> 12) & 63];
      q[2] = i + 1 < n ? b64_table[(triple >> 6) & 63] : '=';
      q[3] = i + 2 < n ? b64_table[triple & 63] : '=';
      if (ts_sbuf_put (&b, q, 4) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static int
b64_val (char c)
{
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  if (c == '+')
    return 62;
  if (c == '/')
    return 63;
  return -1;
}

static TS_Status
c_b64dec (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  size_t n, i;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "b64dec") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "b64dec") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (s);
  if (n % 4 != 0)
    {
      ts_error_set (error, TS_ERR_PARSE, 0, 0, "b64dec: bad length");
      return TS_ERR_PARSE;
    }
  ts_sbuf_init (&b);
  for (i = 0; i < n; i += 4)
    {
      unsigned v[4];
      unsigned triple;
      int k, pad = 0;
      for (k = 0; k < 4; k++)
        {
          if (s[i + k] == '=')
            {
              v[k] = 0;
              pad++;
            }
          else
            {
              int q = b64_val (s[i + k]);
              if (q < 0)
                {
                  ts_sbuf_free (&b);
                  ts_error_set (error, TS_ERR_PARSE, 0, 0,
                                "b64dec: bad character");
                  return TS_ERR_PARSE;
                }
              v[k] = (unsigned) q;
            }
        }
      triple = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
      {
        char o[3];
        o[0] = (char) ((triple >> 16) & 0xFF);
        o[1] = (char) ((triple >> 8) & 0xFF);
        o[2] = (char) (triple & 0xFF);
        if (ts_sbuf_put (&b, o, (size_t) (3 - pad)) != 0)
          {
            ts_sbuf_free (&b);
            ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
            return TS_ERR_NOMEM;
          }
      }
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
c_hexenc (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  static const char *const hexd = "0123456789abcdef";
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "hexenc") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "hexenc") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      char q[2];
      q[0] = hexd[((unsigned char) *s >> 4) & 15];
      q[1] = hexd[(unsigned char) *s & 15];
      if (ts_sbuf_put (&b, q, 2) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static int
hex_val (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

static TS_Status
c_hexdec (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  size_t n, i;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "hexdec") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "hexdec") != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (s);
  if (n % 2 != 0)
    {
      ts_error_set (error, TS_ERR_PARSE, 0, 0, "hexdec: odd length");
      return TS_ERR_PARSE;
    }
  ts_sbuf_init (&b);
  for (i = 0; i < n; i += 2)
    {
      int hi = hex_val (s[i]), lo = hex_val (s[i + 1]);
      char o;
      if (hi < 0 || lo < 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_PARSE, 0, 0,
                        "hexdec: bad character");
          return TS_ERR_PARSE;
        }
      o = (char) ((hi << 4) | lo);
      if (ts_sbuf_ch (&b, o) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  out = ts_sbuf_take (&b);
  /* Embedded NULs truncate the Termscript string (documented). */
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static bool
url_unreserved (char c)
{
  return isalnum ((unsigned char) c) || c == '-' || c == '_' ||
         c == '.' || c == '~';
}

static TS_Status
c_urlenc (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  static const char *const hexd = "0123456789ABCDEF";
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "urlenc") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "urlenc") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      if (url_unreserved (*s))
        {
          if (ts_sbuf_ch (&b, *s) != 0)
            goto oom;
        }
      else
        {
          char q[3];
          q[0] = '%';
          q[1] = hexd[((unsigned char) *s >> 4) & 15];
          q[2] = hexd[(unsigned char) *s & 15];
          if (ts_sbuf_put (&b, q, 3) != 0)
            goto oom;
        }
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    goto oom;
  return TS_OK;
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
c_urldec (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "urldec") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "urldec") != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      if (*s == '%' && s[1] && s[2])
        {
          int hi = hex_val (s[1]), lo = hex_val (s[2]);
          if (hi < 0 || lo < 0)
            {
              ts_sbuf_free (&b);
              ts_error_set (error, TS_ERR_PARSE, 0, 0,
                            "urldec: bad escape");
              return TS_ERR_PARSE;
            }
          if (ts_sbuf_ch (&b, (char) ((hi << 4) | lo)) != 0)
            goto oom;
          s += 2;
        }
      else if (*s == '+')
        {
          if (ts_sbuf_ch (&b, ' ') != 0)
            goto oom;
        }
      else if (ts_sbuf_ch (&b, *s) != 0)
        goto oom;
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    goto oom;
  return TS_OK;
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static uint32_t crc_tab[256];
static pthread_once_t crc_once = PTHREAD_ONCE_INIT;

static void
crc_init (void)
{
  uint32_t i, j, c;
  for (i = 0; i < 256; i++)
    {
      c = i;
      for (j = 0; j < 8; j++)
        c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      crc_tab[i] = c;
    }
}

static TS_Status
c_crc32 (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *s;
  uint32_t crc = 0xFFFFFFFFu;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "crc32") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "crc32") != TS_OK)
    return TS_ERR_INVAL;
  pthread_once (&crc_once, crc_init);
  for (; *s; s++)
    crc = crc_tab[(crc ^ (unsigned char) *s) & 0xFF] ^ (crc >> 8);
  ts_std_ret_int (ret, (int64_t) (crc ^ 0xFFFFFFFFu));
  return TS_OK;
}

static TS_Status
c_case (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
        TS_Error *error, const char *fname, bool upper)
{
  const char *s;
  ts_sbuf_t b;
  char *out;
  if (ts_std_argc (vm, argc, 1, 1, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (; *s; s++)
    {
      char c = upper ? (char) toupper ((unsigned char) *s) :
                       (char) tolower ((unsigned char) *s);
      if (ts_sbuf_ch (&b, c) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
c_upper (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return c_case (v, a, n, r, e, "upper", true);
}

static TS_Status
c_lower (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
         TS_Error *e)
{
  (void) ud;
  return c_case (v, a, n, r, e, "lower", false);
}

static TS_Status
c_trim (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  const char *s;
  const char *end;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "trim") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "trim") != TS_OK)
    return TS_ERR_INVAL;
  while (*s && isspace ((unsigned char) *s))
    s++;
  end = s + strlen (s);
  while (end > s && isspace ((unsigned char) end[-1]))
    end--;
  return ts_std_ret_strn (ret, s, (size_t) (end - s), error);
}

static TS_Status
c_replace (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  const char *s, *from, *to;
  ts_sbuf_t b;
  size_t flen;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "replace") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "replace") != TS_OK ||
      ts_std_str (&argv[1], &from, error, "replace") != TS_OK ||
      ts_std_str (&argv[2], &to, error, "replace") != TS_OK)
    return TS_ERR_INVAL;
  flen = strlen (from);
  ts_sbuf_init (&b);
  if (!flen)
    {
      if (ts_sbuf_str (&b, s) != 0)
        goto oom;
    }
  else
    {
      const char *hit;
      while ((hit = strstr (s, from)) != NULL)
        {
          if (ts_sbuf_put (&b, s, (size_t) (hit - s)) != 0 ||
              ts_sbuf_str (&b, to) != 0)
            goto oom;
          s = hit + flen;
        }
      if (ts_sbuf_str (&b, s) != 0)
        goto oom;
    }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    goto oom;
  return TS_OK;
oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
c_affix (TS_VM *vm, const TS_Value *argv, size_t argc, TS_Value *ret,
         TS_Error *error, const char *fname, int mode)
{
  /* mode 0 = contains, 1 = starts_with, 2 = ends_with. */
  const char *s, *sub;
  size_t n, m;
  bool hit = false;
  if (ts_std_argc (vm, argc, 2, 2, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, fname) != TS_OK ||
      ts_std_str (&argv[1], &sub, error, fname) != TS_OK)
    return TS_ERR_INVAL;
  n = strlen (s);
  m = strlen (sub);
  if (mode == 0)
    hit = strstr (s, sub) != NULL;
  else if (mode == 1)
    hit = strncmp (s, sub, m) == 0;
  else
    hit = n >= m && strcmp (s + n - m, sub) == 0;
  ts_std_ret_bool (ret, hit);
  return TS_OK;
}

static TS_Status
c_contains (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
            TS_Error *e)
{
  (void) ud;
  return c_affix (v, a, n, r, e, "contains", 0);
}

static TS_Status
c_starts_with (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
               TS_Error *e)
{
  (void) ud;
  return c_affix (v, a, n, r, e, "starts_with", 1);
}

static TS_Status
c_ends_with (TS_VM *v, void *ud, const TS_Value *a, size_t n, TS_Value *r,
             TS_Error *e)
{
  (void) ud;
  return c_affix (v, a, n, r, e, "ends_with", 2);
}

static TS_Status
c_repeat (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *s;
  int64_t n, i;
  ts_sbuf_t b;
  char *out;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "repeat") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "repeat") != TS_OK ||
      ts_std_int (&argv[1], &n, error, "repeat") != TS_OK)
    return TS_ERR_INVAL;
  if (n < 0 || n > 100000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "repeat: bad count");
      return TS_ERR_INVAL;
    }
  ts_sbuf_init (&b);
  for (i = 0; i < n; i++)
    if (ts_sbuf_str (&b, s) != 0)
      {
        ts_sbuf_free (&b);
        ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
        return TS_ERR_NOMEM;
      }
  out = ts_sbuf_take (&b);
  ts_value_free (ret);
  ret->type = TS_STRING;
  ret->as.string = out;
  if (!ret->as.string)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
c_split (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  const char *s, *sep;
  size_t slen;
  ts_std_list_t *l;
  const char *p;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "split") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &s, error, "split") != TS_OK ||
      ts_std_str (&argv[1], &sep, error, "split") != TS_OK)
    return TS_ERR_INVAL;
  l = ts_std_list_new ();
  if (!l)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  slen = strlen (sep);
  if (!slen)
    {
      /* Empty separator: split into single characters. */
      for (p = s; *p; p++)
        if (ts_std_ret_strn (ret, p, 1, error) != TS_OK ||
            ts_std_list_push (l, ret) != TS_OK)
          {
            ts_std_list_free (l);
            ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
            return TS_ERR_NOMEM;
          }
    }
  else
    {
      for (p = s;;)
        {
          const char *hit = strstr (p, sep);
          TS_Value v;
          v.type = TS_NIL;
          if (ts_std_ret_strn (&v, p, hit ? (size_t) (hit - p) :
                               strlen (p), error) != TS_OK ||
              ts_std_list_push (l, &v) != TS_OK)
            {
              ts_value_free (&v);
              ts_std_list_free (l);
              ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
              return TS_ERR_NOMEM;
            }
          ts_value_free (&v);
          if (!hit)
            break;
          p = hit + slen;
        }
    }
  ts_value_free (ret);
  return ts_std_list_wrap (ret, l, error);
}

static const TS_FuncDef codec_funcs[] = {
  { "b64enc", c_b64enc, NULL },
  { "b64dec", c_b64dec, NULL },
  { "hexenc", c_hexenc, NULL },
  { "hexdec", c_hexdec, NULL },
  { "urlenc", c_urlenc, NULL },
  { "urldec", c_urldec, NULL },
  { "crc32", c_crc32, NULL },
  { "upper", c_upper, NULL },
  { "lower", c_lower, NULL },
  { "trim", c_trim, NULL },
  { "replace", c_replace, NULL },
  { "contains", c_contains, NULL },
  { "starts_with", c_starts_with, NULL },
  { "ends_with", c_ends_with, NULL },
  { "repeat", c_repeat, NULL },
  { "split", c_split, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_codec_module = { "std.codec", codec_funcs };
