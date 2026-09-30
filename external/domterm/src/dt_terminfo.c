/* dt_terminfo.c -- Terminfo database, compiled-entry parser and lookup. */
#include "dt_internal.h"
#include "dt_capnames.inc"

#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define DT_TI_MAGIC_16 282u
#define DT_TI_MAGIC_32 346u
#define DT_TI_MAX_NAMES 4096u
#define DT_TI_MAX_BOOL 1024u
#define DT_TI_MAX_NUM 1024u
#define DT_TI_MAX_STR 2048u
#define DT_TI_MAX_STRTAB (256u << 10)

struct DT_TIDB
{
  char **paths;
  size_t npaths;
};

struct DT_TIProf
{
  char *names_raw;      /* Owned NUL-separated blob. */
  char *primary;        /* Owned primary name. */
  char **aliases;       /* Owned alias copies. */
  size_t nalias;
  bool *bools;
  size_t nbool;
  int32_t *nums;
  size_t nnum;
  char **strs;          /* Borrowed pointers into strtab (NULL = absent). */
  size_t nstr;
  char *strtab;
  size_t strtab_len;
  /* Extended (user-defined) capabilities. */
  size_t xbool_count, xnum_count, xstr_count;
  char **xbool_names;
  bool *xbools;
  char **xnum_names;
  int32_t *xnums;
  char **xstr_names;
  char **xstrs;         /* Into xstrtab. */
  char *xstrtab;
};

struct DT_TIParser
{
  uint8_t *buf;
  size_t len;
  size_t cap;
  bool final_seen;
};

/* ---------------- profile memory management ---------------- */

static void
dt_tiprof_destroy (DT_TIProf *p)
{
  size_t i;
  if (!p)
    return;
  free (p->names_raw);
  free (p->primary);
  for (i = 0; i < p->nalias; i++)
    free (p->aliases[i]);
  free (p->aliases);
  free (p->bools);
  free (p->nums);
  free (p->strs);
  free (p->strtab);
  for (i = 0; i < p->xbool_count; i++)
    free (p->xbool_names[i]);
  free (p->xbool_names);
  free (p->xbools);
  for (i = 0; i < p->xnum_count; i++)
    free (p->xnum_names[i]);
  free (p->xnum_names);
  free (p->xnums);
  for (i = 0; i < p->xstr_count; i++)
    free (p->xstr_names[i]);
  free (p->xstr_names);
  free (p->xstrs);
  free (p->xstrtab);
  free (p);
}

void
dt_tiprof_free (DT_TIProf *profile)
{
  dt_tiprof_destroy (profile);
}

/* ---------------- compiled entry parser ---------------- */

typedef struct
{
  const uint8_t *d;
  size_t n;
  size_t off;
  DT_Error *err;
} cursor;

static DT_Status
need (cursor *c, size_t k)
{
  if (c->off + k > c->n || c->off + k < c->off)
    {
      dt_err_set (c->err, DT_ERR_PARSE, 0, c->off,
                  "truncated terminfo entry at offset %zu", c->off);
      return DT_ERR_PARSE;
    }
  return DT_OK;
}

static uint16_t
read_u16 (cursor *c)
{
  uint16_t v = dt_get_u16le (c->d + c->off);
  c->off += 2;
  return v;
}

static DT_Status
parse_entry (const uint8_t *d, size_t n, DT_TIProf **out, DT_Error *error)
{
  cursor c;
  uint16_t magic, names_size, bool_count, num_count, str_count, strtab_size;
  bool wide;
  size_t i;
  DT_TIProf *p;
  size_t pad;

  if (!d || n == 0 || !out)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad parse input");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (n > DT_TI_MAX_ENTRY)
    {
      dt_err_set (error, DT_ERR_LIMIT, 0, 0, "terminfo entry too large");
      return DT_ERR_LIMIT;
    }

  c.d = d;
  c.n = n;
  c.off = 0;
  c.err = error;

  if (need (&c, 12) != DT_OK)
    return DT_ERR_PARSE;
  magic = read_u16 (&c);
  names_size = read_u16 (&c);
  bool_count = read_u16 (&c);
  num_count = read_u16 (&c);
  str_count = read_u16 (&c);
  strtab_size = read_u16 (&c);

  if (magic == DT_TI_MAGIC_16)
    wide = false;
  else if (magic == DT_TI_MAGIC_32)
    wide = true;
  else
    {
      dt_err_set (error, DT_ERR_UNSUPPORTED, 0, 0,
                  "unsupported terminfo magic %u", magic);
      return DT_ERR_UNSUPPORTED;
    }

  /* u16 counts are inherently bounded (65535); the wider DT_TI_MAX_*
   * caps below document the enforced envelope. */
  if (names_size == 0 || names_size > DT_TI_MAX_NAMES ||
      bool_count > DT_TI_MAX_BOOL || num_count > DT_TI_MAX_NUM ||
      str_count > DT_TI_MAX_STR)
    {
      dt_err_set (error, DT_ERR_PARSE, 0, 0,
                  "implausible terminfo counts");
      return DT_ERR_PARSE;
    }

  p = calloc (1, sizeof *p);
  if (!p)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }

  /* Names. */
  if (need (&c, names_size) != DT_OK)
    goto fail;
  if (d[c.off] == '\0' || d[c.off + names_size - 1] != '\0')
    {
      dt_err_set (error, DT_ERR_PARSE, 0, c.off, "bad names section");
      goto fail;
    }
  p->names_raw = malloc (names_size + 1);
  if (!p->names_raw)
    goto no_memory;
  memcpy (p->names_raw, d + c.off, names_size);
  p->names_raw[names_size] = '\0';
  c.off += names_size;
  /* Split primary + aliases on '|' up to first NUL. */
  {
    char *names_end = strchr (p->names_raw, '\0');
    char *seg = p->names_raw;
    size_t cap = 4;
    p->aliases = malloc (cap * sizeof *p->aliases);
    if (!p->aliases)
      goto no_memory;
    while (seg < names_end)
      {
        char *bar = strchr (seg, '|');
        char *end = bar && bar < names_end ? bar : names_end;
        size_t len = (size_t) (end - seg);
        char *copy;
        if (len == 0) /* Skip empty segments. */
          {
            seg = end + 1;
            if (bar == NULL || bar >= names_end)
              break;
            continue;
          }
        copy = malloc (len + 1);
        if (!copy)
          goto no_memory;
        memcpy (copy, seg, len);
        copy[len] = '\0';
        if (!p->primary)
          p->primary = copy;
        else
          {
            if (p->nalias == cap)
              {
                char **na;
                cap *= 2;
                na = realloc (p->aliases, cap * sizeof *p->aliases);
                if (!na)
                  {
                    free (copy);
                    goto no_memory;
                  }
                p->aliases = na;
              }
            p->aliases[p->nalias++] = copy;
          }
        if (bar == NULL || bar >= names_end)
          break;
        seg = end + 1;
      }
    if (!p->primary)
      {
        dt_err_set (error, DT_ERR_PARSE, 0, 0, "empty primary name");
        goto fail;
      }
  }

  /* Booleans. */
  if (need (&c, bool_count) != DT_OK)
    goto fail;
  if (bool_count)
    {
      p->bools = malloc (bool_count * sizeof *p->bools);
      if (!p->bools)
        goto no_memory;
      for (i = 0; i < bool_count; i++)
        p->bools[i] = d[c.off + i] ? true : false;
      p->nbool = bool_count;
    }
  c.off += bool_count;

  /* Alignment pad before numbers: even for 16-bit, 4-byte for 32-bit. */
  if (wide)
    pad = (4u - (c.off % 4u)) % 4u;
  else
    pad = c.off % 2u;
  if (pad)
    {
      size_t k;
      if (need (&c, pad) != DT_OK)
        goto fail;
      for (k = 0; k < pad; k++)
        if (d[c.off + k] != 0)
          {
            dt_err_set (error, DT_ERR_PARSE, 0, c.off + k,
                        "nonzero alignment pad");
            goto fail;
          }
      c.off += pad;
    }

  /* Numbers. */
  if (num_count)
    {
      size_t width = wide ? 4 : 2;
      if (need (&c, num_count * width) != DT_OK)
        goto fail;
      p->nums = malloc (num_count * sizeof *p->nums);
      if (!p->nums)
        goto no_memory;
      for (i = 0; i < num_count; i++)
        {
          if (wide)
            {
              int32_t v;
              memcpy (&v, d + c.off + i * 4, 4);
              /* Manual LE decode for portability. */
              v = (int32_t) dt_get_u32le (d + c.off + i * 4);
              p->nums[i] = v;
            }
          else
            {
              int16_t v = (int16_t) dt_get_u16le (d + c.off + i * 2);
              p->nums[i] = v == -1 ? -1 : v;
            }
        }
      p->nnum = num_count;
      c.off += num_count * width;
    }

  /* String offsets (always 16-bit). */
  if (str_count)
    {
      if (need (&c, str_count * 2u) != DT_OK)
        goto fail;
      p->strs = calloc (str_count, sizeof *p->strs);
      if (!p->strs)
        goto no_memory;
      p->nstr = str_count;
      for (i = 0; i < str_count; i++)
        {
          uint16_t so = dt_get_u16le (d + c.off + i * 2);
          if (so == 0xFFFFu)
            p->strs[i] = NULL;
          else if (so >= strtab_size)
            {
              dt_err_set (error, DT_ERR_PARSE, 0, c.off + i * 2,
                          "string offset %u out of range", so);
              goto fail;
            }
          else
            /* Placeholder: offset+1 so that table offset 0 (a valid
               * first string) never collides with NULL (absent). */
            p->strs[i] = (char *) (uintptr_t) (so + 1);
        }
      c.off += str_count * 2u;
    }

  /* String table. */
  if (need (&c, strtab_size) != DT_OK)
    goto fail;
  if (strtab_size)
    {
      p->strtab = malloc (strtab_size ? strtab_size : 1);
      if (!p->strtab)
        goto no_memory;
      memcpy (p->strtab, d + c.off, strtab_size);
      p->strtab_len = strtab_size;
      for (i = 0; i < str_count; i++)
        {
          if (p->strs[i])
            {
              size_t so = (size_t) (uintptr_t) p->strs[i] - 1;
              size_t k = so;
              while (k < strtab_size && p->strtab[k] != '\0')
                k++;
              if (k >= strtab_size)
                {
                  dt_err_set (error, DT_ERR_PARSE, 0, c.off + so,
                              "unterminated string capability");
                  goto fail;
                }
              p->strs[i] = p->strtab + so;
            }
        }
    }
  else
    {
      for (i = 0; i < str_count; i++)
        if (p->strs[i])
          {
            dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                        "string offset into empty table");
            goto fail;
          }
      p->strs = p->strs; /* Keep (all NULL). */
    }
  c.off += strtab_size;

  /* Optional extended section (strict: trailing bytes must form one). */
  if (c.off < n)
    {
      uint16_t xc[5];
      size_t eb, en, es, stab2, k;
      size_t base = c.off;
      if (base % 2u)
        {
          if (need (&c, 1) != DT_OK)
            goto fail;
          if (d[c.off] != 0)
            {
              dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                          "nonzero extended pad");
              goto fail;
            }
          c.off++;
        }
      if (need (&c, 10) != DT_OK)
        goto fail;
      for (k = 0; k < 5; k++)
        xc[k] = read_u16 (&c);
      eb = xc[0];
      en = xc[1];
      es = xc[2];
      stab2 = (size_t) xc[3] * 2u;
      if (eb > DT_TI_MAX_BOOL || en > DT_TI_MAX_NUM || es > DT_TI_MAX_STR ||
          stab2 > DT_TI_MAX_STRTAB)
        {
          dt_err_set (error, DT_ERR_PARSE, 0, base, "bad extended counts");
          goto fail;
        }
      /* Extended booleans. */
      if (need (&c, eb) != DT_OK)
        goto fail;
      if (eb)
        {
          p->xbools = malloc (eb * sizeof *p->xbools);
          p->xbool_names = calloc (eb, sizeof *p->xbool_names);
          if (!p->xbools || !p->xbool_names)
            goto no_memory;
          for (i = 0; i < eb; i++)
            p->xbools[i] = d[c.off + i] ? true : false;
          p->xbool_count = eb;
        }
      c.off += eb;
      if (c.off % 2u)
        {
          if (need (&c, 1) != DT_OK)
            goto fail;
          c.off++;
        }
      /* Extended numbers. */
      if (en)
        {
          size_t width = wide ? 4 : 2;
          if (need (&c, en * width) != DT_OK)
            goto fail;
          p->xnums = malloc (en * sizeof *p->xnums);
          p->xnum_names = calloc (en, sizeof *p->xnum_names);
          if (!p->xnums || !p->xnum_names)
            goto no_memory;
          for (i = 0; i < en; i++)
            {
              if (wide)
                p->xnums[i] = (int32_t) dt_get_u32le (d + c.off + i * 4);
              else
                {
                  int16_t v = (int16_t) dt_get_u16le (d + c.off + i * 2);
                  p->xnums[i] = v == -1 ? -1 : v;
                }
            }
          p->xnum_count = en;
          c.off += en * width;
        }
      /* Extended string offsets. */
      {
        uint16_t *so = NULL;
        if (es)
          {
            if (need (&c, es * 2u) != DT_OK)
              goto fail;
            so = malloc (es * sizeof *so);
            if (!so)
              goto no_memory;
            for (i = 0; i < es; i++)
              so[i] = dt_get_u16le (d + c.off + i * 2);
            c.off += es * 2u;
          }
        /* Three name-table offsets (bool/num/str names). */
        {
          uint16_t no[3];
          size_t namsz;
          if (need (&c, 6) != DT_OK)
            {
              free (so);
              goto fail;
            }
          for (k = 0; k < 3; k++)
            no[k] = read_u16 (&c);
          if (need (&c, stab2) != DT_OK)
            {
              free (so);
              goto fail;
            }
          if (xc[4] < stab2)
            {
              free (so);
              dt_err_set (error, DT_ERR_PARSE, 0, base,
                          "extended size mismatch");
              goto fail;
            }
          namsz = (size_t) xc[4] - stab2;
          if (need (&c, namsz) != DT_OK)
            {
              free (so);
              goto fail;
            }
          /* Validate + copy string table. */
          if (stab2)
            {
              p->xstrtab = malloc (stab2);
              if (!p->xstrtab)
                {
                  free (so);
                  goto no_memory;
                }
              memcpy (p->xstrtab, d + c.off, stab2);
            }
          /* Validate offsets point at NUL-terminated strings. */
          if (es)
            {
              p->xstrs = calloc (es, sizeof *p->xstrs);
              p->xstr_names = calloc (es, sizeof *p->xstr_names);
              if (!p->xstrs || !p->xstr_names)
                {
                  free (so);
                  goto no_memory;
                }
              p->xstr_count = es;
              for (i = 0; i < es; i++)
                {
                  size_t q;
                  if (so[i] >= stab2 || !stab2)
                    {
                      free (so);
                      dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                                  "extended string offset out of range");
                      goto fail;
                    }
                  for (q = so[i]; q < stab2 && p->xstrtab[q]; q++)
                    ;
                  if (q >= stab2)
                    {
                      free (so);
                      dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                                  "unterminated extended string");
                      goto fail;
                    }
                  p->xstrs[i] = p->xstrtab + so[i];
                }
            }
          free (so);
          c.off += stab2;
          /* Validate + split the names blob. */
          {
            const uint8_t *nb = d + c.off;
            size_t blen[3], bcount[3], bi, ni;
            /* Offsets must be ordered: bool@no[0] <= num@no[1] <= str@no[2]. */
            if (!(no[0] <= no[1] && no[1] <= no[2] && no[2] <= namsz))
              {
                dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                            "bad extended name offsets");
                goto fail;
              }
            if (namsz && nb[namsz - 1] != '\0')
              {
                dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                            "extended names not NUL-terminated");
                goto fail;
              }
            blen[0] = (size_t) no[1] - no[0];
            blen[1] = (size_t) no[2] - no[1];
            blen[2] = namsz - no[2];
            bcount[0] = eb;
            bcount[1] = en;
            bcount[2] = es;
            for (bi = 0; bi < 3; bi++)
              {
                /* Count NUL-terminated names in this subtable. */
                size_t cnt = 0, q = 0;
                while (q < blen[bi])
                  {
                    size_t z = q;
                    while (z < blen[bi] && nb[no[bi] + z])
                      z++;
                    if (z >= blen[bi])
                      {
                        dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                                    "bad extended name table");
                        goto fail;
                      }
                    if (z == q)
                      break; /* Empty tail entry. */
                    cnt++;
                    q = z + 1;
                  }
                if (cnt != bcount[bi])
                  {
                    dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                                "extended name count mismatch");
                    goto fail;
                  }
                /* Copy names. */
                q = 0;
                for (ni = 0; ni < cnt; ni++)
                  {
                    size_t z = q;
                    char *copy;
                    while (nb[no[bi] + z])
                      z++;
                    copy = malloc (z - q + 1);
                    if (!copy)
                      goto no_memory;
                    memcpy (copy, nb + no[bi] + q, z - q);
                    copy[z - q] = '\0';
                    if (bi == 0)
                      p->xbool_names[ni] = copy;
                    else if (bi == 1)
                      p->xnum_names[ni] = copy;
                    else
                      p->xstr_names[ni] = copy;
                    q = z + 1;
                  }
              }
          }
          c.off += namsz;
        }
      }
    }

  if (c.off != n)
    {
      dt_err_set (error, DT_ERR_PARSE, 0, c.off,
                  "trailing garbage in terminfo entry");
      goto fail;
    }

  *out = p;
  return DT_OK;

no_memory:
  dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
fail:
  dt_tiprof_destroy (p);
  *out = NULL;
  if (error && error->code == DT_OK)
    dt_err_set (error, DT_ERR_PARSE, 0, 0, "invalid terminfo entry");
  return error ? error->code : DT_ERR_PARSE;
}

/* ---------------- database ---------------- */

static void
dt_tidb_destroy (DT_TIDB *db)
{
  size_t i;
  if (!db)
    return;
  for (i = 0; i < db->npaths; i++)
    free (db->paths[i]);
  free (db->paths);
  free (db);
}

void
dt_tidb_close (DT_TIDB *db)
{
  dt_tidb_destroy (db);
}

DT_TIDB *
dt_tidb_open (const char *const *search_paths, size_t search_path_count,
              DT_Error *error)
{
  DT_TIDB *db;
  size_t i;
  if ((search_paths == NULL && search_path_count != 0))
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "bad search paths");
      return NULL;
    }
  db = calloc (1, sizeof *db);
  if (!db)
    {
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return NULL;
    }
  if (search_path_count)
    {
      db->paths = calloc (search_path_count, sizeof *db->paths);
      if (!db->paths)
        {
          free (db);
          dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
          return NULL;
        }
      for (i = 0; i < search_path_count; i++)
        {
          if (!search_paths[i])
            {
              dt_tidb_destroy (db);
              dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                          "NULL search path");
              return NULL;
            }
          db->paths[i] = strdup (search_paths[i]);
          if (!db->paths[i])
            {
              dt_tidb_destroy (db);
              dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0,
                          "out of memory");
              return NULL;
            }
          db->npaths++;
        }
    }
  return db;
}

static DT_Status
dt_tidb_add_path (DT_TIDB *db, const char *path)
{
  char **np;
  char *copy;
  if (!path || !*path)
    return DT_OK;
  copy = strdup (path);
  if (!copy)
    return DT_ERR_NO_MEMORY;
  np = realloc (db->paths, (db->npaths + 1) * sizeof *np);
  if (!np)
    {
      free (copy);
      return DT_ERR_NO_MEMORY;
    }
  db->paths = np;
  db->paths[db->npaths++] = copy;
  return DT_OK;
}

DT_TIDB *
dt_tidb_open_default (DT_Error *error)
{
  DT_TIDB *db = dt_tidb_open (NULL, 0, error);
  const char *env;
  if (!db)
    return NULL;
  env = getenv ("TERMINFO");
  if (env && *env)
    {
      if (dt_tidb_add_path (db, env) != DT_OK)
        goto no_memory;
    }
  else
    {
      env = getenv ("TERMINFO_DIRS");
      if (env && *env)
        {
          char *copy = strdup (env);
          char *save = NULL, *tok;
          if (!copy)
            goto no_memory;
          for (tok = strtok_r (copy, ":", &save); tok;
               tok = strtok_r (NULL, ":", &save))
            {
              if (*tok && dt_tidb_add_path (db, tok) != DT_OK)
                {
                  free (copy);
                  goto no_memory;
                }
            }
          free (copy);
        }
      else
        {
          env = getenv ("HOME");
          if (env && *env)
            {
              char buf[4096];
              snprintf (buf, sizeof buf, "%s/.terminfo", env);
              if (dt_tidb_add_path (db, buf) != DT_OK)
                goto no_memory;
            }
        }
    }
  {
    static const char *const fallbacks[] = {
      "/usr/share/terminfo", "/lib/terminfo", "/usr/lib/terminfo", NULL
    };
    size_t i;
    for (i = 0; fallbacks[i]; i++)
      if (dt_tidb_add_path (db, fallbacks[i]) != DT_OK)
        goto no_memory;
  }
  return db;

no_memory:
  dt_tidb_destroy (db);
  dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
  return NULL;
}

static DT_Status
dt_read_file (const char *path, uint8_t **data, size_t *len, DT_Error *error)
{
  FILE *f = fopen (path, "rb");
  uint8_t *buf = NULL;
  size_t cap = 8192, n = 0;
  size_t r;
  if (!f)
    return DT_ERR_NOT_FOUND;
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
      return DT_ERR_NO_MEMORY;
    }
  while ((r = fread (buf + n, 1, cap - n, f)) > 0)
    {
      n += r;
      if (n == cap)
        {
          uint8_t *nb;
          if (cap > DT_TI_MAX_ENTRY)
            {
              free (buf);
              fclose (f);
              dt_err_set (error, DT_ERR_LIMIT, 0, 0,
                          "terminfo file too large");
              return DT_ERR_LIMIT;
            }
          cap *= 2;
          nb = realloc (buf, cap);
          if (!nb)
            {
              free (buf);
              fclose (f);
              dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0,
                          "out of memory");
              return DT_ERR_NO_MEMORY;
            }
          buf = nb;
        }
    }
  fclose (f);
  *data = buf;
  *len = n;
  return DT_OK;
}

static bool
dt_profile_matches (const DT_TIProf *p, const char *name)
{
  size_t i;
  if (strcmp (p->primary, name) == 0)
    return true;
  for (i = 0; i < p->nalias; i++)
    if (strcmp (p->aliases[i], name) == 0)
      return true;
  return false;
}

DT_TIProf *
dt_tiprof_load (DT_TIDB *db, const char *terminal_name, DT_Error *error)
{
  size_t i;
  char path[4096];
  if (!db || !terminal_name || !*terminal_name ||
      strchr (terminal_name, '/'))
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0,
                  "invalid terminal name");
      return NULL;
    }
  if (db->npaths == 0)
    {
      dt_err_set (error, DT_ERR_NOT_FOUND, 0, 0,
                  "no terminfo search paths");
      return NULL;
    }
  for (i = 0; i < db->npaths; i++)
    {
      uint8_t *data = NULL;
      size_t len = 0;
      DT_Status st;
      DT_TIProf *prof = NULL;
      snprintf (path, sizeof path, "%s/%c/%s", db->paths[i],
                terminal_name[0], terminal_name);
      st = dt_read_file (path, &data, &len, NULL);
      if (st == DT_ERR_NOT_FOUND)
        continue;
      if (st != DT_OK)
        {
          dt_err_set (error, st, 0, 0, "cannot read %s", path);
          return NULL;
        }
      st = parse_entry (data, len, &prof, error);
      free (data);
      if (st != DT_OK)
        return NULL; /* File exists but is corrupt: report, don't skip. */
      if (!dt_profile_matches (prof, terminal_name))
        {
          /* Filename hit but the entry disavows the name: keep looking. */
          dt_tiprof_destroy (prof);
          continue;
        }
      return prof;
    }
  dt_err_set (error, DT_ERR_NOT_FOUND, 0, 0, "terminal '%s' not found",
              terminal_name);
  return NULL;
}

DT_TIProf *
dt_tidb_load_default (DT_TIDB *db, DT_Error *error)
{
  const char *term = getenv ("TERM");
  if (!term || !*term)
    {
      dt_err_set (error, DT_ERR_NOT_FOUND, 0, 0, "TERM is not set");
      return NULL;
    }
  return dt_tiprof_load (db, term, error);
}

const char *
dt_tiprof_name (const DT_TIProf *profile)
{
  return profile ? profile->primary : NULL;
}

size_t
dt_tiprof_alias_count (const DT_TIProf *profile)
{
  return profile ? profile->nalias : 0;
}

const char *
dt_tiprof_alias (const DT_TIProf *profile, size_t index)
{
  if (!profile || index >= profile->nalias)
    return NULL;
  return profile->aliases[index];
}

/* ---------------- lookup and introspection ---------------- */

static int
dt_name_index (const char *const *names, const char *cap)
{
  int i;
  for (i = 0; names[i]; i++)
    if (strcmp (names[i], cap) == 0)
      return i;
  return -1;
}

bool
dt_tiprof_get (const DT_TIProf *profile, const char *capability,
               DT_TIValue *value)
{
  int idx;
  size_t i;
  if (!profile || !capability || !value)
    return false;
  idx = dt_name_index (dt_bool_names, capability);
  if (idx >= 0)
    {
      if ((size_t) idx >= profile->nbool)
        return false;
      value->type = DT_TI_BOOL;
      value->as.boolean = profile->bools[idx];
      return true;
    }
  idx = dt_name_index (dt_num_names, capability);
  if (idx >= 0)
    {
      if ((size_t) idx >= profile->nnum || profile->nums[idx] < 0)
        return false;
      value->type = DT_TI_NUMBER;
      value->as.number = profile->nums[idx];
      return true;
    }
  idx = dt_name_index (dt_str_names, capability);
  if (idx >= 0)
    {
      if ((size_t) idx >= profile->nstr || !profile->strs[idx])
        return false;
      value->type = DT_TI_STRING;
      value->as.string = profile->strs[idx];
      return true;
    }
  for (i = 0; i < profile->xbool_count; i++)
    if (strcmp (profile->xbool_names[i], capability) == 0)
      {
        value->type = DT_TI_BOOL;
        value->as.boolean = profile->xbools[i];
        return true;
      }
  for (i = 0; i < profile->xnum_count; i++)
    if (strcmp (profile->xnum_names[i], capability) == 0)
      {
        if (profile->xnums[i] < 0)
          return false;
        value->type = DT_TI_NUMBER;
        value->as.number = profile->xnums[i];
        return true;
      }
  for (i = 0; i < profile->xstr_count; i++)
    if (strcmp (profile->xstr_names[i], capability) == 0)
      {
        if (!profile->xstrs[i])
          return false;
        value->type = DT_TI_STRING;
        value->as.string = profile->xstrs[i];
        return true;
      }
  return false;
}

bool
dt_tiprof_has (const DT_TIProf *profile, const char *capability)
{
  DT_TIValue v;
  return dt_tiprof_get (profile, capability, &v);
}

bool
dt_tiprof_cap_info (const DT_TIProf *profile, const char *capability,
                    DT_TICapInfo *info)
{
  int idx;
  size_t i;
  DT_TIValue v;
  if (!capability || !info)
    return false;
  idx = dt_name_index (dt_bool_names, capability);
  if (idx >= 0)
    {
      info->name = dt_bool_names[idx];
      info->type = DT_TI_BOOL;
      info->index = (unsigned) idx;
      info->present = profile && (size_t) idx < profile->nbool;
      return true;
    }
  idx = dt_name_index (dt_num_names, capability);
  if (idx >= 0)
    {
      info->name = dt_num_names[idx];
      info->type = DT_TI_NUMBER;
      info->index = (unsigned) idx;
      info->present = profile && (size_t) idx < profile->nnum &&
                      profile->nums[idx] >= 0;
      return true;
    }
  idx = dt_name_index (dt_str_names, capability);
  if (idx >= 0)
    {
      info->name = dt_str_names[idx];
      info->type = DT_TI_STRING;
      info->index = (unsigned) idx;
      info->present = profile && (size_t) idx < profile->nstr &&
                      profile->strs[idx] != NULL;
      return true;
    }
  if (profile)
    {
      for (i = 0; i < profile->xbool_count; i++)
        if (strcmp (profile->xbool_names[i], capability) == 0)
          {
            info->name = profile->xbool_names[i];
            info->type = DT_TI_BOOL;
            info->index = (unsigned) (DT_BOOL_COUNT + DT_NUM_COUNT +
                                      DT_STR_COUNT + i);
            info->present = true;
            return true;
          }
      for (i = 0; i < profile->xnum_count; i++)
        if (strcmp (profile->xnum_names[i], capability) == 0)
          {
            info->name = profile->xnum_names[i];
            info->type = DT_TI_NUMBER;
            info->index = (unsigned) (DT_BOOL_COUNT + DT_NUM_COUNT +
                                      DT_STR_COUNT + profile->xbool_count +
                                      i);
            info->present = profile->xnums[i] >= 0;
            return true;
          }
      for (i = 0; i < profile->xstr_count; i++)
        if (strcmp (profile->xstr_names[i], capability) == 0)
          {
            info->name = profile->xstr_names[i];
            info->type = DT_TI_STRING;
            info->index = (unsigned) (DT_BOOL_COUNT + DT_NUM_COUNT +
                                      DT_STR_COUNT + profile->xbool_count +
                                      profile->xnum_count + i);
            info->present = profile->xstrs[i] != NULL;
            return true;
          }
    }
  (void) v;
  return false;
}

size_t
dt_tiprof_cap_count (void)
{
  return (size_t) (DT_BOOL_COUNT + DT_NUM_COUNT + DT_STR_COUNT);
}

bool
dt_tiprof_cap_by_index (size_t index, DT_TICapInfo *info)
{
  if (!info)
    return false;
  if (index < (size_t) DT_BOOL_COUNT)
    {
      info->name = dt_bool_names[index];
      info->type = DT_TI_BOOL;
      info->index = (unsigned) index;
      info->present = false;
      return true;
    }
  index -= (size_t) DT_BOOL_COUNT;
  if (index < (size_t) DT_NUM_COUNT)
    {
      info->name = dt_num_names[index];
      info->type = DT_TI_NUMBER;
      info->index = (unsigned) index;
      info->present = false;
      return true;
    }
  index -= (size_t) DT_NUM_COUNT;
  if (index < (size_t) DT_STR_COUNT)
    {
      info->name = dt_str_names[index];
      info->type = DT_TI_STRING;
      info->index = (unsigned) index;
      info->present = false;
      return true;
    }
  return false;
}

/* ---------------- incremental parser ---------------- */

DT_TIParser *
dt_tiparser_create (DT_Error *error)
{
  DT_TIParser *p = calloc (1, sizeof *p);
  if (!p)
    dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
  return p;
}

void
dt_tiparser_free (DT_TIParser *parser)
{
  if (!parser)
    return;
  free (parser->buf);
  free (parser);
}

void
dt_tiparser_reset (DT_TIParser *parser)
{
  if (!parser)
    return;
  free (parser->buf);
  parser->buf = NULL;
  parser->len = 0;
  parser->cap = 0;
  parser->final_seen = false;
}

DT_Status
dt_tiparser_feed (DT_TIParser *parser, const uint8_t *data, size_t data_size,
                  bool final_chunk, size_t *consumed, DT_Error *error)
{
  size_t want;
  if (!parser)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL parser");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (data_size && !data)
    {
      dt_err_set (error, DT_ERR_INVALID_ARGUMENT, 0, 0, "NULL data");
      return DT_ERR_INVALID_ARGUMENT;
    }
  if (parser->final_seen)
    {
      dt_err_set (error, DT_ERR_PROTOCOL, 0, parser->len,
                  "feed after final chunk");
      return DT_ERR_PROTOCOL;
    }
  if (data_size > DT_TI_MAX_ENTRY ||
      parser->len > DT_TI_MAX_ENTRY - data_size)
    {
      dt_err_set (error, DT_ERR_LIMIT, 0, parser->len, "entry too large");
      return DT_ERR_LIMIT;
    }
  want = parser->len + data_size;
  if (want > parser->cap)
    {
      uint8_t *nb = realloc (parser->buf, want ? want : 1);
      if (!nb)
        {
          dt_err_set (error, DT_ERR_NO_MEMORY, ENOMEM, 0, "out of memory");
          return DT_ERR_NO_MEMORY;
        }
      parser->buf = nb;
      parser->cap = want;
    }
  if (data_size)
    memcpy (parser->buf + parser->len, data, data_size);
  parser->len += data_size;
  if (final_chunk)
    parser->final_seen = true;
  if (consumed)
    *consumed = data_size;
  return DT_OK;
}

DT_TIProf *
dt_tiparser_take_profile (DT_TIParser *parser)
{
  DT_TIProf *prof = NULL;
  DT_Error err;
  if (!parser || !parser->final_seen || parser->len == 0)
    return NULL;
  dt_error_clear (&err);
  if (parse_entry (parser->buf, parser->len, &prof, &err) != DT_OK)
    return NULL;
  /* Consumed: a second take without new input yields NULL. */
  free (parser->buf);
  parser->buf = NULL;
  parser->len = 0;
  parser->cap = 0;
  return prof;
}
