/* dom.c -- shared document model for std.json / std.yaml / std.toml.
 *
 * One variant DOM, three strict parsers, one JSON stringify.  The JSON
 * parser is a hand-written recursive-descent reader (depth-capped);
 * YAML covers the documented block subset (indent maps/lists, scalar
 * coercion, `#` comments); TOML delegates to vendored tomlc99
 * (third_party/tomlc99) and converts its tables/arrays into this DOM.
 */
#include "common/ts_std_common.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "toml.h"

struct ts_dom
{
  ts_dom_kind_t kind;
  bool boolean;
  int64_t integer;
  char *string;
  ts_dom_t **items;
  size_t nitems, cap_items;
  char **keys;
  ts_dom_t **vals;
  size_t npairs;
};

static const char dom_tag_id = 0;

const void *
ts_dom_tag (void)
{
  return &dom_tag_id;
}

static ts_dom_t *
dom_new (ts_dom_kind_t kind)
{
  ts_dom_t *d = calloc (1, sizeof *d);
  if (d)
    d->kind = kind;
  return d;
}

ts_dom_t *ts_dom_new_null (void) { return dom_new (TS_DOM_NULL); }
ts_dom_t *
ts_dom_new_bool (bool b)
{
  ts_dom_t *d = dom_new (TS_DOM_BOOL);
  if (d)
    d->boolean = b;
  return d;
}
ts_dom_t *
ts_dom_new_int (int64_t i)
{
  ts_dom_t *d = dom_new (TS_DOM_INT);
  if (d)
    d->integer = i;
  return d;
}
ts_dom_t *
ts_dom_new_str (const char *s)
{
  ts_dom_t *d = dom_new (TS_DOM_STR);
  if (!d)
    return NULL;
  d->string = strdup (s ? s : "");
  if (!d->string)
    {
      free (d);
      return NULL;
    }
  return d;
}
ts_dom_t *ts_dom_new_arr (void) { return dom_new (TS_DOM_ARR); }
ts_dom_t *ts_dom_new_obj (void) { return dom_new (TS_DOM_OBJ); }

void
ts_dom_free (void *p)
{
  ts_dom_t *d = p;
  size_t i;
  if (!d)
    return;
  free (d->string);
  for (i = 0; i < d->nitems; i++)
    ts_dom_free (d->items[i]);
  free (d->items);
  for (i = 0; i < d->npairs; i++)
    {
      free (d->keys[i]);
      ts_dom_free (d->vals[i]);
    }
  free (d->keys);
  free (d->vals);
  free (d);
}

TS_Status
ts_dom_arr_push (ts_dom_t *arr, ts_dom_t *v)
{
  ts_dom_t **nv;
  if (!arr || arr->kind != TS_DOM_ARR || !v)
    {
      ts_dom_free (v);
      return TS_ERR_INVAL;
    }
  if (arr->nitems == arr->cap_items)
    {
      size_t nc = arr->cap_items ? arr->cap_items * 2 : 4;
      nv = realloc (arr->items, nc * sizeof *nv);
      if (!nv)
        {
          ts_dom_free (v);
          return TS_ERR_NOMEM;
        }
      arr->items = nv;
      arr->cap_items = nc;
    }
  arr->items[arr->nitems++] = v;
  return TS_OK;
}

TS_Status
ts_dom_obj_set (ts_dom_t *obj, const char *key, ts_dom_t *v)
{
  size_t i;
  char **nk;
  ts_dom_t **nv;
  if (!obj || obj->kind != TS_DOM_OBJ || !key || !v)
    {
      ts_dom_free (v);
      return TS_ERR_INVAL;
    }
  for (i = 0; i < obj->npairs; i++)
    if (strcmp (obj->keys[i], key) == 0)
      {
        ts_dom_free (obj->vals[i]);
        obj->vals[i] = v;
        return TS_OK;
      }
  nk = realloc (obj->keys, (obj->npairs + 1) * sizeof *nk);
  nv = realloc (obj->vals, (obj->npairs + 1) * sizeof *nv);
  if (!nk || !nv)
    {
      free (nk);
      free (nv);
      ts_dom_free (v);
      return TS_ERR_NOMEM;
    }
  obj->keys = nk;
  obj->vals = nv;
  obj->keys[obj->npairs] = strdup (key);
  if (!obj->keys[obj->npairs])
    {
      ts_dom_free (v);
      return TS_ERR_NOMEM;
    }
  obj->vals[obj->npairs++] = v;
  return TS_OK;
}

/* ---------------- JSON parser ---------------- */

typedef struct
{
  const char *s;
  size_t len;
  size_t pos;
  int depth;
  TS_Error *error;
} json_p_t;

static bool
json_fail (json_p_t *p, const char *msg)
{
  ts_error_set (p->error, TS_ERR_PARSE, 0, p->pos, "json: %s", msg);
  return false;
}

static void
json_ws (json_p_t *p)
{
  while (p->pos < p->len && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' ||
                             p->s[p->pos] == '\n' || p->s[p->pos] == '\r'))
    p->pos++;
}

static bool json_value (json_p_t *p, ts_dom_t **out);

static int
json_hex (char c)
{
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

/* Append code point as UTF-8; caller validated ranges loosely. */
static void
json_emit_utf8 (ts_sbuf_t *b, unsigned cp)
{
  char tmp[4];
  if (cp < 0x80)
    ts_sbuf_ch (b, (char) cp);
  else if (cp < 0x800)
    {
      tmp[0] = (char) (0xC0 | (cp >> 6));
      tmp[1] = (char) (0x80 | (cp & 0x3F));
      ts_sbuf_put (b, tmp, 2);
    }
  else
    {
      tmp[0] = (char) (0xE0 | (cp >> 12));
      tmp[1] = (char) (0x80 | ((cp >> 6) & 0x3F));
      tmp[2] = (char) (0x80 | (cp & 0x3F));
      ts_sbuf_put (b, tmp, 3);
    }
}

static bool
json_string (json_p_t *p, ts_dom_t **out)
{
  ts_sbuf_t b;
  ts_sbuf_init (&b);
  p->pos++; /* opening quote */
  for (;;)
    {
      char c;
      if (p->pos >= p->len)
        {
          ts_sbuf_free (&b);
          return json_fail (p, "unterminated string");
        }
      c = p->s[p->pos++];
      if (c == '"')
        break;
      if (c == '\\')
        {
          char e;
          if (p->pos >= p->len)
            {
              ts_sbuf_free (&b);
              return json_fail (p, "bad escape");
            }
          e = p->s[p->pos++];
          switch (e)
            {
            case '"': ts_sbuf_ch (&b, '"'); break;
            case '\\': ts_sbuf_ch (&b, '\\'); break;
            case '/': ts_sbuf_ch (&b, '/'); break;
            case 'b': ts_sbuf_ch (&b, '\b'); break;
            case 'f': ts_sbuf_ch (&b, '\f'); break;
            case 'n': ts_sbuf_ch (&b, '\n'); break;
            case 'r': ts_sbuf_ch (&b, '\r'); break;
            case 't': ts_sbuf_ch (&b, '\t'); break;
            case 'u':
              {
                unsigned cp = 0;
                int k;
                if (p->pos + 4 > p->len)
                  {
                    ts_sbuf_free (&b);
                    return json_fail (p, "bad \\u escape");
                  }
                for (k = 0; k < 4; k++)
                  {
                    int h = json_hex (p->s[p->pos + k]);
                    if (h < 0)
                      {
                        ts_sbuf_free (&b);
                        return json_fail (p, "bad \\u escape");
                      }
                    cp = (cp << 4) | (unsigned) h;
                  }
                p->pos += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 6 <= p->len &&
                    p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u')
                  {
                    unsigned lo = 0;
                    for (k = 0; k < 4; k++)
                      {
                        int h = json_hex (p->s[p->pos + 2 + k]);
                        if (h < 0)
                          break;
                        lo = (lo << 4) | (unsigned) h;
                      }
                    if (k == 4 && lo >= 0xDC00 && lo <= 0xDFFF)
                      {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p->pos += 6;
                      }
                  }
                if (cp > 0x10FFFF)
                  cp = 0xFFFD;
                if (cp >= 0x10000)
                  {
                    /* Beyond BMP: emit replacement (documented limit). */
                    json_emit_utf8 (&b, 0xFFFD);
                  }
                else
                  json_emit_utf8 (&b, cp);
              }
              break;
            default:
              ts_sbuf_free (&b);
              return json_fail (p, "bad escape");
            }
        }
      else if ((unsigned char) c < 0x20)
        {
          ts_sbuf_free (&b);
          return json_fail (p, "control character in string");
        }
      else
        ts_sbuf_ch (&b, c);
    }
  *out = ts_dom_new_str (b.data ? b.data : "");
  ts_sbuf_free (&b);
  if (!*out)
    {
      ts_error_set (p->error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return false;
    }
  return true;
}

static bool
json_number (json_p_t *p, ts_dom_t **out)
{
  size_t start = p->pos;
  bool is_double = false;
  if (p->s[p->pos] == '-')
    p->pos++;
  while (p->pos < p->len && isdigit ((unsigned char) p->s[p->pos]))
    p->pos++;
  if (p->pos < p->len && p->s[p->pos] == '.')
    {
      is_double = true;
      p->pos++;
      while (p->pos < p->len && isdigit ((unsigned char) p->s[p->pos]))
        p->pos++;
    }
  if (p->pos < p->len && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E'))
    {
      is_double = true;
      p->pos++;
      if (p->pos < p->len && (p->s[p->pos] == '+' || p->s[p->pos] == '-'))
        p->pos++;
      while (p->pos < p->len && isdigit ((unsigned char) p->s[p->pos]))
        p->pos++;
    }
  if (is_double)
    {
      /* Documented coercion: doubles truncate toward zero into ints. */
      char *tmp = strndup (p->s + start, p->pos - start);
      double dv;
      if (!tmp)
        {
          ts_error_set (p->error, TS_ERR_NOMEM, ENOMEM, 0,
                        "out of memory");
          return false;
        }
      dv = strtod (tmp, NULL);
      free (tmp);
      *out = ts_dom_new_int ((int64_t) dv);
    }
  else
    {
      char *tmp = strndup (p->s + start, p->pos - start);
      char *end = NULL;
      long long v;
      if (!tmp)
        {
          ts_error_set (p->error, TS_ERR_NOMEM, ENOMEM, 0,
                        "out of memory");
          return false;
        }
      errno = 0;
      v = strtoll (tmp, &end, 10);
      free (tmp);
      if (errno == ERANGE)
        return json_fail (p, "number out of range");
      *out = ts_dom_new_int ((int64_t) v);
    }
  if (!*out)
    {
      ts_error_set (p->error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return false;
    }
  return true;
}

static bool
json_lit (json_p_t *p, const char *word, ts_dom_t *node, ts_dom_t **out)
{
  size_t n = strlen (word);
  if (p->len - p->pos < n || memcmp (p->s + p->pos, word, n) != 0)
    {
      ts_dom_free (node);
      return json_fail (p, "bad literal");
    }
  p->pos += n;
  *out = node;
  return true;
}

static bool
json_array (json_p_t *p, ts_dom_t **out)
{
  ts_dom_t *arr = ts_dom_new_arr ();
  if (!arr)
    return false;
  if (++p->depth > 64)
    {
      ts_dom_free (arr);
      return json_fail (p, "nesting too deep");
    }
  p->pos++; /* [ */
  json_ws (p);
  if (p->pos < p->len && p->s[p->pos] == ']')
    {
      p->pos++;
      p->depth--;
      *out = arr;
      return true;
    }
  for (;;)
    {
      ts_dom_t *item = NULL;
      json_ws (p);
      if (!json_value (p, &item))
        {
          ts_dom_free (arr);
          return false;
        }
      if (ts_dom_arr_push (arr, item) != TS_OK)
        {
          ts_dom_free (arr);
          ts_error_set (p->error, TS_ERR_NOMEM, ENOMEM, 0,
                        "out of memory");
          return false;
        }
      json_ws (p);
      if (p->pos >= p->len)
        {
          ts_dom_free (arr);
          return json_fail (p, "unterminated array");
        }
      if (p->s[p->pos] == ',')
        {
          p->pos++;
          continue;
        }
      if (p->s[p->pos] == ']')
        {
          p->pos++;
          p->depth--;
          *out = arr;
          return true;
        }
      ts_dom_free (arr);
      return json_fail (p, "expected ',' or ']'");
    }
}

static bool
json_object (json_p_t *p, ts_dom_t **out)
{
  ts_dom_t *obj = ts_dom_new_obj ();
  if (!obj)
    return false;
  if (++p->depth > 64)
    {
      ts_dom_free (obj);
      return json_fail (p, "nesting too deep");
    }
  p->pos++; /* { */
  json_ws (p);
  if (p->pos < p->len && p->s[p->pos] == '}')
    {
      p->pos++;
      p->depth--;
      *out = obj;
      return true;
    }
  for (;;)
    {
      ts_dom_t *key = NULL, *val = NULL;
      char *kcopy;
      json_ws (p);
      if (p->pos >= p->len || p->s[p->pos] != '"')
        {
          ts_dom_free (obj);
          return json_fail (p, "object key must be a string");
        }
      if (!json_string (p, &key))
        {
          ts_dom_free (obj);
          return false;
        }
      json_ws (p);
      if (p->pos >= p->len || p->s[p->pos] != ':')
        {
          ts_dom_free (key);
          ts_dom_free (obj);
          return json_fail (p, "expected ':'");
        }
      p->pos++;
      json_ws (p);
      if (!json_value (p, &val))
        {
          ts_dom_free (key);
          ts_dom_free (obj);
          return false;
        }
      kcopy = key->string;
      key->string = NULL;
      ts_dom_free (key);
      if (ts_dom_obj_set (obj, kcopy ? kcopy : "", val) != TS_OK)
        {
          free (kcopy);
          ts_dom_free (obj);
          ts_error_set (p->error, TS_ERR_NOMEM, ENOMEM, 0,
                        "out of memory");
          return false;
        }
      free (kcopy);
      json_ws (p);
      if (p->pos >= p->len)
        {
          ts_dom_free (obj);
          return json_fail (p, "unterminated object");
        }
      if (p->s[p->pos] == ',')
        {
          p->pos++;
          continue;
        }
      if (p->s[p->pos] == '}')
        {
          p->pos++;
          p->depth--;
          *out = obj;
          return true;
        }
      ts_dom_free (obj);
      return json_fail (p, "expected ',' or '}'");
    }
}

static bool
json_value (json_p_t *p, ts_dom_t **out)
{
  char c;
  json_ws (p);
  if (p->pos >= p->len)
    return json_fail (p, "unexpected end");
  c = p->s[p->pos];
  if (c == '"')
    return json_string (p, out);
  if (c == '{')
    return json_object (p, out);
  if (c == '[')
    return json_array (p, out);
  if (c == '-' || isdigit ((unsigned char) c))
    return json_number (p, out);
  if (c == 't')
    return json_lit (p, "true", ts_dom_new_bool (true), out);
  if (c == 'f')
    return json_lit (p, "false", ts_dom_new_bool (false), out);
  if (c == 'n')
    return json_lit (p, "null", ts_dom_new_null (), out);
  return json_fail (p, "unexpected character");
}

TS_Status
ts_dom_parse_json (const char *s, ts_dom_t **out, TS_Error *error)
{
  json_p_t p;
  ts_dom_t *root = NULL;
  if (!s || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  p.s = s;
  p.len = strlen (s);
  p.pos = 0;
  p.depth = 0;
  p.error = error;
  if (!json_value (&p, &root))
    return error ? error->code : TS_ERR_PARSE;
  json_ws (&p);
  if (p.pos != p.len)
    {
      ts_dom_free (root);
      ts_error_set (error, TS_ERR_PARSE, 0, p.pos, "json: trailing data");
      return TS_ERR_PARSE;
    }
  *out = root;
  return TS_OK;
}

/* ---------------- YAML subset parser ---------------- */

typedef struct
{
  char **lines;
  size_t nlines;
  size_t idx;
  TS_Error *error;
} yaml_p_t;

static size_t
yaml_indent (const char *line)
{
  size_t i = 0;
  while (line[i] == ' ')
    i++;
  return i;
}

static bool
yaml_blank (const char *line)
{
  size_t i = yaml_indent (line);
  return line[i] == '\0' || line[i] == '\r' || line[i] == '#';
}

/* Strip trailing spaces/CR and cut `#` comments outside quotes. */
static void
yaml_clean (char *line)
{
  bool sq = false, dq = false;
  size_t i, n;
  for (i = 0; line[i]; i++)
    {
      char c = line[i];
      if (c == '\'' && !dq)
        sq = !sq;
      else if (c == '"' && !sq)
        dq = !dq;
      else if (c == '#' && !sq && !dq && (i == 0 || line[i - 1] == ' '))
        {
          line[i] = '\0';
          break;
        }
    }
  n = strlen (line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                   line[n - 1] == '\r'))
    line[--n] = '\0';
}

static ts_dom_t *yaml_scalar (const char *text);

static ts_dom_t *yaml_block (yaml_p_t *p, size_t indent);

static ts_dom_t *
yaml_seq_item_value (yaml_p_t *p, const char *rest, size_t indent)
{
  while (*rest == ' ')
    rest++;
  if (!*rest)
    {
      /* Nested block on following lines. */
      size_t save = p->idx;
      ts_dom_t *sub;
      while (p->idx < p->nlines && yaml_blank (p->lines[p->idx]))
        p->idx++;
      if (p->idx >= p->nlines || yaml_indent (p->lines[p->idx]) <= indent)
        {
          p->idx = save;
          return ts_dom_new_null ();
        }
      sub = yaml_block (p, yaml_indent (p->lines[p->idx]));
      return sub ? sub : ts_dom_new_null ();
    }
  return yaml_scalar (rest);
}

/* Split "key: value" at the first colon that is followed by space/EOL.
 * Returns key length, or 0 when the line is not a mapping entry. */
static size_t
yaml_key_len (const char *text)
{
  bool sq = false, dq = false;
  size_t i;
  for (i = 0; text[i]; i++)
    {
      char c = text[i];
      if (c == '\'' && !dq)
        sq = !sq;
      else if (c == '"' && !sq)
        dq = !dq;
      else if (c == ':' && !sq && !dq &&
               (text[i + 1] == ' ' || text[i + 1] == '\0' ||
                text[i + 1] == '\r' || text[i + 1] == '\t'))
        return i;
    }
  return 0;
}

static ts_dom_t *
yaml_block (yaml_p_t *p, size_t indent)
{
  /* Decide sequence vs mapping from the first content line. */
  size_t save = p->idx;
  bool is_seq = false;
  while (save < p->nlines && yaml_blank (p->lines[save]))
    save++;
  if (save < p->nlines)
    {
      const char *t = p->lines[save] + indent;
      if (t[0] == '-' && (t[1] == ' ' || t[1] == '\0' || t[1] == '\r'))
        is_seq = true;
    }
  if (is_seq)
    {
      ts_dom_t *arr = ts_dom_new_arr ();
      if (!arr)
        return NULL;
      for (;;)
        {
          const char *t;
          ts_dom_t *item;
          while (p->idx < p->nlines && yaml_blank (p->lines[p->idx]))
            p->idx++;
          if (p->idx >= p->nlines ||
              yaml_indent (p->lines[p->idx]) != indent)
            break;
          t = p->lines[p->idx] + indent;
          if (!(t[0] == '-' &&
                (t[1] == ' ' || t[1] == '\0' || t[1] == '\r')))
            break;
          p->idx++;
          item = yaml_seq_item_value (p, t + 1, indent);
          if (!item || ts_dom_arr_push (arr, item) != TS_OK)
            {
              ts_dom_free (arr);
              return NULL;
            }
        }
      return arr;
    }
  {
    ts_dom_t *obj = ts_dom_new_obj ();
    if (!obj)
      return NULL;
    for (;;)
      {
        const char *t;
        size_t klen;
        char *key, *val;
        ts_dom_t *node;
        while (p->idx < p->nlines && yaml_blank (p->lines[p->idx]))
          p->idx++;
        if (p->idx >= p->nlines ||
            yaml_indent (p->lines[p->idx]) != indent)
          break;
        t = p->lines[p->idx] + indent;
        klen = yaml_key_len (t);
        if (!klen)
          break;
        key = strndup (t, klen);
        val = strdup (t + klen + 1);
        if (!key || !val)
          {
            free (key);
            free (val);
            ts_dom_free (obj);
            return NULL;
          }
        {
          /* Trim the key's trailing spaces. */
          size_t kn = strlen (key);
          while (kn > 0 && (key[kn - 1] == ' ' || key[kn - 1] == '\t'))
            key[--kn] = '\0';
        }
        p->idx++;
        {
          char *v = val;
          while (*v == ' ' || *v == '\t')
            v++;
          if (!*v)
            {
              size_t save2 = p->idx;
              while (p->idx < p->nlines && yaml_blank (p->lines[p->idx]))
                p->idx++;
              if (p->idx < p->nlines &&
                  yaml_indent (p->lines[p->idx]) > indent)
                node = yaml_block (p, yaml_indent (p->lines[p->idx]));
              else
                {
                  p->idx = save2;
                  node = ts_dom_new_null ();
                }
            }
          else
            node = yaml_scalar (v);
        }
        free (val);
        if (!node || ts_dom_obj_set (obj, key, node) != TS_OK)
          {
            free (key);
            ts_dom_free (obj);
            return NULL;
          }
        free (key);
      }
    return obj;
  }
}

static ts_dom_t *
yaml_scalar (const char *text)
{
  size_t n = strlen (text);
  char *end = NULL;
  long long iv;
  if (n >= 2 && text[0] == '"' && text[n - 1] == '"')
    {
      /* Double-quoted: handle common escapes. */
      ts_sbuf_t b;
      size_t i;
      ts_dom_t *d;
      ts_sbuf_init (&b);
      for (i = 1; i + 1 < n; i++)
        {
          if (text[i] == '\\' && i + 2 <= n)
            {
              char e = text[++i];
              switch (e)
                {
                case 'n': ts_sbuf_ch (&b, '\n'); break;
                case 't': ts_sbuf_ch (&b, '\t'); break;
                case 'r': ts_sbuf_ch (&b, '\r'); break;
                case '"': ts_sbuf_ch (&b, '"'); break;
                case '\\': ts_sbuf_ch (&b, '\\'); break;
                default: ts_sbuf_ch (&b, e); break;
                }
            }
          else
            ts_sbuf_ch (&b, text[i]);
        }
      d = ts_dom_new_str (b.data ? b.data : "");
      ts_sbuf_free (&b);
      return d;
    }
  if (n >= 2 && text[0] == '\'' && text[n - 1] == '\'')
    {
      /* Single-quoted: '' is an escaped quote. */
      ts_sbuf_t b;
      size_t i;
      ts_dom_t *d;
      ts_sbuf_init (&b);
      for (i = 1; i + 1 < n; i++)
        {
          if (text[i] == '\'' && text[i + 1] == '\'')
            {
              ts_sbuf_ch (&b, '\'');
              i++;
            }
          else
            ts_sbuf_ch (&b, text[i]);
        }
      d = ts_dom_new_str (b.data ? b.data : "");
      ts_sbuf_free (&b);
      return d;
    }
  if (strcmp (text, "null") == 0 || strcmp (text, "~") == 0 ||
      strcmp (text, "Null") == 0 || strcmp (text, "NULL") == 0)
    return ts_dom_new_null ();
  if (strcmp (text, "true") == 0 || strcmp (text, "True") == 0 ||
      strcmp (text, "TRUE") == 0)
    return ts_dom_new_bool (true);
  if (strcmp (text, "false") == 0 || strcmp (text, "False") == 0 ||
      strcmp (text, "FALSE") == 0)
    return ts_dom_new_bool (false);
  errno = 0;
  iv = strtoll (text, &end, 10);
  if (errno == 0 && end && !*end && end != text)
    return ts_dom_new_int ((int64_t) iv);
  return ts_dom_new_str (text);
}

TS_Status
ts_dom_parse_yaml (const char *s, ts_dom_t **out, TS_Error *error)
{
  yaml_p_t p;
  ts_dom_t *root;
  char *copy;
  size_t i, n = 1;
  if (!s || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  for (i = 0; s[i]; i++)
    if (s[i] == '\n')
      n++;
  copy = strdup (s);
  p.lines = calloc (n + 1, sizeof *p.lines);
  if (!copy || !p.lines)
    {
      free (copy);
      free (p.lines);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  p.nlines = 0;
  {
    char *save = NULL, *line = strtok_r (copy, "\n", &save);
    while (line)
      {
        yaml_clean (line);
        /* Skip document markers. */
        if (strcmp (line, "---") != 0 && strcmp (line, "...") != 0)
          p.lines[p.nlines++] = line;
        line = strtok_r (NULL, "\n", &save);
      }
  }
  p.idx = 0;
  p.error = error;
  while (p.idx < p.nlines && yaml_blank (p.lines[p.idx]))
    p.idx++;
  if (p.idx >= p.nlines)
    root = ts_dom_new_null ();
  else
    root = yaml_block (&p, yaml_indent (p.lines[p.idx]));
  free (p.lines);
  free (copy);
  if (!root)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  *out = root;
  return TS_OK;
}

/* ---------------- TOML via tomlc99 ---------------- */

static ts_dom_t *toml_to_dom_table (const toml_table_t *t);
static ts_dom_t *toml_to_dom_array (const toml_array_t *a);

static ts_dom_t *
toml_to_dom_datum (toml_datum_t d, char kind)
{
  if (!d.ok)
    return ts_dom_new_null ();
  switch (kind)
    {
    case 's':
      {
        ts_dom_t *n = ts_dom_new_str (d.u.s ? d.u.s : "");
        free (d.u.s);
        return n;
      }
    case 'b':
      return ts_dom_new_bool (d.u.b ? true : false);
    case 'i':
      return ts_dom_new_int (d.u.i);
    case 'd':
      return ts_dom_new_int ((int64_t) d.u.d);
    default:
      {
        /* Timestamps and unknowns become strings. */
        char buf[64];
        ts_dom_t *n;
        if (d.u.ts)
          {
            snprintf (buf, sizeof buf, "%04d-%02d-%02d",
                      d.u.ts->year ? *d.u.ts->year : 0,
                      d.u.ts->month ? *d.u.ts->month : 0,
                      d.u.ts->day ? *d.u.ts->day : 0);
            free (d.u.ts);
          }
        else
          buf[0] = '\0';
        n = ts_dom_new_str (buf);
        return n;
      }
    }
}

static ts_dom_t *
toml_to_dom_array (const toml_array_t *a)
{
  ts_dom_t *arr = ts_dom_new_arr ();
  int n, i;
  char kind;
  if (!arr || !a)
    {
      if (!arr)
        return NULL;
      return arr;
    }
  n = toml_array_nelem (a);
  kind = toml_array_kind (a);
  for (i = 0; i < n; i++)
    {
      ts_dom_t *item = NULL;
      if (kind == 't')
        item = toml_to_dom_table (toml_table_at (a, i));
      else if (kind == 'a')
        item = toml_to_dom_array (toml_array_at (a, i));
      else if (kind == 'v')
        {
          /* Probe value type in s/b/i/d order. */
          toml_datum_t d = toml_string_at (a, i);
          if (d.ok)
            item = toml_to_dom_datum (d, 's');
          else if ((d = toml_bool_at (a, i)).ok)
            item = toml_to_dom_datum (d, 'b');
          else if ((d = toml_int_at (a, i)).ok)
            item = toml_to_dom_datum (d, 'i');
          else if ((d = toml_double_at (a, i)).ok)
            item = toml_to_dom_datum (d, 'd');
          else
            item = ts_dom_new_null ();
        }
      else
        item = ts_dom_new_null ();
      if (!item || ts_dom_arr_push (arr, item) != TS_OK)
        {
          ts_dom_free (arr);
          return NULL;
        }
    }
  return arr;
}

static ts_dom_t *
toml_to_dom_table (const toml_table_t *t)
{
  ts_dom_t *obj = ts_dom_new_obj ();
  int n, i;
  if (!obj || !t)
    {
      if (!obj)
        return NULL;
      return obj;
    }
  n = toml_table_nkval (t);
  for (i = 0; i < n; i++)
    {
      const char *key = toml_key_in (t, i);
      ts_dom_t *v = NULL;
      toml_datum_t d;
      if (!key)
        continue;
      d = toml_string_in (t, key);
      if (d.ok)
        v = toml_to_dom_datum (d, 's');
      else if ((d = toml_bool_in (t, key)).ok)
        v = toml_to_dom_datum (d, 'b');
      else if ((d = toml_int_in (t, key)).ok)
        v = toml_to_dom_datum (d, 'i');
      else if ((d = toml_double_in (t, key)).ok)
        v = toml_to_dom_datum (d, 'd');
      else
        v = ts_dom_new_null ();
      if (!v || ts_dom_obj_set (obj, key, v) != TS_OK)
        {
          ts_dom_free (obj);
          return NULL;
        }
    }
  {
    /* Arrays and subtables share the key namespace; walk every key
     * once and attach non-scalar members. */
    int k = 0;
    const char *key;
    while ((key = toml_key_in (t, k++)) != NULL)
      {
        toml_array_t *a = toml_array_in (t, key);
        toml_table_t *sub = toml_table_in (t, key);
        if (a)
          {
            ts_dom_t *v = toml_to_dom_array (a);
            if (!v || ts_dom_obj_set (obj, key, v) != TS_OK)
              {
                ts_dom_free (obj);
                return NULL;
              }
          }
        else if (sub)
          {
            ts_dom_t *v = toml_to_dom_table (sub);
            if (!v || ts_dom_obj_set (obj, key, v) != TS_OK)
              {
                ts_dom_free (obj);
                return NULL;
              }
          }
      }
  }
  (void) n;
  return obj;
}

TS_Status
ts_dom_parse_toml (const char *s, ts_dom_t **out, TS_Error *error)
{
  char *copy;
  char errbuf[256];
  toml_table_t *root;
  ts_dom_t *dom;
  if (!s || !out)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad arguments");
      return TS_ERR_INVAL;
    }
  copy = strdup (s ? s : "");
  if (!copy)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  errbuf[0] = '\0';
  root = toml_parse (copy, errbuf, sizeof errbuf);
  free (copy);
  if (!root)
    {
      ts_error_set (error, TS_ERR_PARSE, 0, 0, "toml: %s",
                    errbuf[0] ? errbuf : "parse error");
      return TS_ERR_PARSE;
    }
  dom = toml_to_dom_table (root);
  toml_free (root);
  if (!dom)
    {
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  *out = dom;
  return TS_OK;
}

/* ---------------- navigation / rendering ---------------- */

const ts_dom_t *
ts_dom_key (const ts_dom_t *d, const char *key)
{
  size_t i;
  if (!d || d->kind != TS_DOM_OBJ || !key)
    return NULL;
  for (i = 0; i < d->npairs; i++)
    if (strcmp (d->keys[i], key) == 0)
      return d->vals[i];
  return NULL;
}

const ts_dom_t *
ts_dom_at (const ts_dom_t *d, size_t i)
{
  if (!d || d->kind != TS_DOM_ARR || i >= d->nitems)
    return NULL;
  return d->items[i];
}

const ts_dom_t *
ts_dom_path (const ts_dom_t *d, const char *path)
{
  const char *p = path;
  if (!d || !path)
    return NULL;
  while (*p && d)
    {
      char seg[256];
      size_t n = 0;
      bool numeric = true;
      while (*p && *p != '.')
        {
          if (n + 1 < sizeof seg)
            seg[n++] = *p;
          if (!isdigit ((unsigned char) *p))
            numeric = false;
          p++;
        }
      if (*p == '.')
        p++;
      seg[n] = '\0';
      if (!n)
        return NULL;
      if (numeric && d->kind == TS_DOM_ARR)
        {
          unsigned long idx = strtoul (seg, NULL, 10);
          d = ts_dom_at (d, (size_t) idx);
        }
      else
        {
          /* Numeric keys on objects still work (falls through). */
          d = ts_dom_key (d, seg);
        }
    }
  return d;
}

size_t
ts_dom_len (const ts_dom_t *d)
{
  if (!d)
    return 0;
  if (d->kind == TS_DOM_ARR)
    return d->nitems;
  if (d->kind == TS_DOM_OBJ)
    return d->npairs;
  if (d->kind == TS_DOM_STR && d->string)
    return strlen (d->string);
  return 0;
}

const char *
ts_dom_kind_str (const ts_dom_t *d)
{
  if (!d)
    return "nil";
  switch (d->kind)
    {
    case TS_DOM_NULL: return "null";
    case TS_DOM_BOOL: return "bool";
    case TS_DOM_INT: return "int";
    case TS_DOM_STR: return "string";
    case TS_DOM_ARR: return "array";
    case TS_DOM_OBJ: return "object";
    }
  return "nil";
}

static int
dom_emit_json_str (ts_sbuf_t *b, const char *s)
{
  const unsigned char *p;
  if (ts_sbuf_ch (b, '"') != 0)
    return -1;
  for (p = (const unsigned char *) (s ? s : ""); *p; p++)
    {
      switch (*p)
        {
        case '"': if (ts_sbuf_str (b, "\\\"") != 0) return -1; break;
        case '\\': if (ts_sbuf_str (b, "\\\\") != 0) return -1; break;
        case '\n': if (ts_sbuf_str (b, "\\n") != 0) return -1; break;
        case '\r': if (ts_sbuf_str (b, "\\r") != 0) return -1; break;
        case '\t': if (ts_sbuf_str (b, "\\t") != 0) return -1; break;
        default:
          if (*p < 0x20)
            {
              if (ts_sbuf_printf (b, "\\u%04x", *p) != 0)
                return -1;
            }
          else if (ts_sbuf_ch (b, (char) *p) != 0)
            return -1;
          break;
        }
    }
  return ts_sbuf_ch (b, '"');
}

static int
dom_stringify_into (const ts_dom_t *d, ts_sbuf_t *b)
{
  size_t i;
  if (!d)
    return ts_sbuf_str (b, "null");
  switch (d->kind)
    {
    case TS_DOM_NULL:
      return ts_sbuf_str (b, "null");
    case TS_DOM_BOOL:
      return ts_sbuf_str (b, d->boolean ? "true" : "false");
    case TS_DOM_INT:
      return ts_sbuf_printf (b, "%lld", (long long) d->integer);
    case TS_DOM_STR:
      return dom_emit_json_str (b, d->string);
    case TS_DOM_ARR:
      if (ts_sbuf_ch (b, '[') != 0)
        return -1;
      for (i = 0; i < d->nitems; i++)
        {
          if (i && ts_sbuf_ch (b, ',') != 0)
            return -1;
          if (dom_stringify_into (d->items[i], b) != 0)
            return -1;
        }
      return ts_sbuf_ch (b, ']');
    case TS_DOM_OBJ:
      if (ts_sbuf_ch (b, '{') != 0)
        return -1;
      for (i = 0; i < d->npairs; i++)
        {
          if (i && ts_sbuf_ch (b, ',') != 0)
            return -1;
          if (dom_emit_json_str (b, d->keys[i]) != 0)
            return -1;
          if (ts_sbuf_ch (b, ':') != 0)
            return -1;
          if (dom_stringify_into (d->vals[i], b) != 0)
            return -1;
        }
      return ts_sbuf_ch (b, '}');
    }
  return -1;
}

TS_Status
ts_dom_stringify (const ts_dom_t *d, char **out)
{
  ts_sbuf_t b;
  ts_sbuf_init (&b);
  if (!out || dom_stringify_into (d, &b) != 0)
    {
      ts_sbuf_free (&b);
      return TS_ERR_NOMEM;
    }
  *out = ts_sbuf_take (&b);
  if (!*out)
    return TS_ERR_NOMEM;
  return TS_OK;
}

TS_Status
ts_dom_to_value (const ts_dom_t *d, TS_Value *ret, TS_Error *error)
{
  char desc[64];
  ts_dom_t *alias;
  if (!d)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  switch (d->kind)
    {
    case TS_DOM_NULL:
      ts_std_ret_nil (ret);
      return TS_OK;
    case TS_DOM_BOOL:
      ts_std_ret_bool (ret, d->boolean);
      return TS_OK;
    case TS_DOM_INT:
      ts_std_ret_int (ret, d->integer);
      return TS_OK;
    case TS_DOM_STR:
      return ts_std_ret_str (ret, d->string, error);
    case TS_DOM_ARR:
    case TS_DOM_OBJ:
      /* Containers cross as sub-handles.  The DOM owns its nodes, so
       * clone the subtree for the new handle. */
      {
        char *text = NULL;
        ts_dom_t *clone = NULL;
        if (ts_dom_stringify (d, &text) != TS_OK)
          {
            ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0,
                          "out of memory");
            return TS_ERR_NOMEM;
          }
        if (ts_dom_parse_json (text, &clone, error) != TS_OK)
          {
            free (text);
            return error ? error->code : TS_ERR_PARSE;
          }
        free (text);
        alias = clone;
      }
      snprintf (desc, sizeof desc, "<%s len=%zu>",
                d->kind == TS_DOM_ARR ? "array" : "object",
                ts_dom_len (d));
      if (ts_value_make_handle (ret, alias, ts_dom_free, desc,
                                &dom_tag_id) != TS_OK)
        {
          ts_dom_free (alias);
          ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
      return TS_OK;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

TS_Status
ts_dom_wrap (TS_Value *ret, ts_dom_t *d, TS_Error *error)
{
  char desc[64];
  if (!d)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad document");
      return TS_ERR_INVAL;
    }
  snprintf (desc, sizeof desc, "<%s>", ts_dom_kind_str (d));
  if (ts_value_make_handle (ret, d, ts_dom_free, desc, &dom_tag_id) !=
      TS_OK)
    {
      ts_dom_free (d);
      ts_error_set (error, TS_ERR_NOMEM, ENOMEM, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

ts_dom_t *
ts_dom_unwrap (const TS_Value *v, TS_Error *error, const char *what)
{
  return ts_std_handle (v, &dom_tag_id, error,
                        what ? what : "document handle");
}
