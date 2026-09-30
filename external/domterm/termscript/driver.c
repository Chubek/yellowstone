/* driver.c -- the `termscript` command-line driver.
 *
 * Usage:
 *   termscript [--help] [-c] [-o OUT] [--static] [--cc CC] script.ts
 *   termscript [--help] [-c] [-o OUT] [--static] [--cc CC] -
 *
 * Modes (two backends, per the DomTERM specification):
 * - VM (default): run the script with the tree-walking VM and print
 *   captured G:puts output to stdout.
 * - Transpile to C: with -o OUT and without -c, emit C to a temporary
 *   file and compile+link it against libtermscript (plus
 *   libtermscript_stdlib for the std.* native modules, plus
 *   libtermscript_standalone and libdomterm) with $CC (default "cc").
 *   Native libraries link statically (static archives); --static adds
 *   -static to the whole link.
 * - With -c: stop after emitting the C file (to OUT, or stdout when -o
 *   is absent); no compilation or linking happens.
 *
 * `-` reads the script from stdin.  Transpiled C needs libtermscript at
 * link time; use an installed tree or set LIBRARY_PATH accordingly.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "domterm.h"

#define TS_DRIVER_MAX_STDIN (1u << 20)

static void
usage (FILE *f)
{
  fputs ("usage: termscript [--help] [-c] [-o OUT] [--static] [--cc CC] script.ts\n", f);
}

static char *
read_stdin_all (void)
{
  size_t cap = 8192, len = 0;
  char *buf = malloc (cap);
  size_t r;
  if (!buf)
    return NULL;
  while ((r = fread (buf + len, 1, cap - len - 1, stdin)) > 0)
    {
      len += r;
      if (len + 1 >= cap)
        {
          char *nb;
          if (cap >= TS_DRIVER_MAX_STDIN)
            {
              free (buf);
              return NULL;
            }
          cap *= 2;
          nb = realloc (buf, cap);
          if (!nb)
            {
              free (buf);
              return NULL;
            }
          buf = nb;
        }
    }
  buf[len] = '\0';
  return buf;
}

static char *
read_file_all (const char *path)
{
  FILE *f = fopen (path, "rb");
  char *buf;
  size_t cap = 8192, len = 0, r;
  if (!f)
    return NULL;
  buf = malloc (cap);
  if (!buf)
    {
      fclose (f);
      return NULL;
    }
  while ((r = fread (buf + len, 1, cap - len - 1, f)) > 0)
    {
      char *nb;
      len += r;
      if (len + 1 < cap)
        continue;
      if (cap >= TS_DRIVER_MAX_STDIN)
        {
          free (buf);
          fclose (f);
          return NULL;
        }
      cap *= 2;
      nb = realloc (buf, cap);
      if (!nb)
        {
          free (buf);
          fclose (f);
          return NULL;
        }
      buf = nb;
    }
  fclose (f);
  buf[len] = '\0';
  return buf;
}

static int
run_vm (const char *source, bool from_stdin)
{
  DT_TermVM *vm;
  DT_Error err;
  char *output = NULL;
  DT_Status st;
  dt_error_clear (&err);
  vm = dt_termscript_create (&err);
  if (!vm)
    {
      fprintf (stderr, "termscript: %s\n", err.message);
      return 1;
    }
  if (from_stdin)
    st = dt_termscript_run_string (vm, source, &output, &err);
  else
    st = dt_termscript_run_file (vm, source, &output, &err);
  if (st != DT_OK)
    {
      fprintf (stderr, "termscript: %s\n", err.message);
      dt_termscript_free (vm);
      return 1;
    }
  fputs (output ? output : "", stdout);
  free (output);
  dt_termscript_free (vm);
  return 0;
}

static int
emit_c (const char *source, bool from_stdin, const char *out_path)
{
  DT_TermVM *vm;
  DT_Error err;
  const char *code;
  char *file_text = NULL;
  FILE *out;
  dt_error_clear (&err);
  vm = dt_termscript_create (&err);
  if (!vm)
    {
      fprintf (stderr, "termscript: %s\n", err.message);
      return 1;
    }
  if (from_stdin)
    {
      file_text = read_stdin_all ();
      if (!file_text)
        {
          fprintf (stderr, "termscript: cannot read stdin\n");
          dt_termscript_free (vm);
          return 1;
        }
      source = file_text;
    }
  else
    {
      /* compile_to_c needs source text, not the path. */
      file_text = read_file_all (source);
      if (!file_text)
        {
          fprintf (stderr, "termscript: cannot open '%s': %s\n", source,
                   strerror (errno));
          dt_termscript_free (vm);
          return 1;
        }
    }
  {
    char unit[128];
    /* Derive a unit name from the script path (alnum, rest '_');
     * stdin keeps the default. */
    const char *base = from_stdin ? "" : source;
    size_t i;
    const char *slash = strrchr (base, '/');
    if (slash)
      base = slash + 1;
    for (i = 0; i + 1 < sizeof unit && base[i] && base[i] != '.'; i++)
      unit[i] = (base[i] == '_' ||
                 (base[i] >= '0' && base[i] <= '9') ||
                 (base[i] >= 'A' && base[i] <= 'Z') ||
                 (base[i] >= 'a' && base[i] <= 'z')) ? base[i] : '_';
    unit[i] = '\0';
    if (!unit[0])
      strcpy (unit, "termscript_unit");
    code = dt_termscript_compile_to_c (vm, file_text, unit, &err);
  }
  free (file_text);
  if (!code)
    {
      fprintf (stderr, "termscript: %s\n", err.message);
      dt_termscript_free (vm);
      return 1;
    }
  if (!out_path)
    {
      fputs (code, stdout);
      dt_termscript_free (vm);
      return 0;
    }
  out = fopen (out_path, "w");
  if (!out)
    {
      fprintf (stderr, "termscript: cannot write '%s': %s\n", out_path,
               strerror (errno));
      dt_termscript_free (vm);
      return 1;
    }
  fputs (code, out);
  if (fclose (out) != 0)
    {
      fprintf (stderr, "termscript: cannot write '%s': %s\n", out_path,
               strerror (errno));
      dt_termscript_free (vm);
      return 1;
    }
  dt_termscript_free (vm);
  return 0;
}

static int
build_exe (const char *source, bool from_stdin, const char *out_path,
           bool link_static, const char *cc)
{
  DT_TermVM *vm;
  DT_Error err;
  const char *code;
  char tmp[] = "/tmp/termscriptXXXXXX.c";
  int fd;
  FILE *f;
  int rc;
  dt_error_clear (&err);
  vm = dt_termscript_create (&err);
  if (!vm)
    {
      fprintf (stderr, "termscript: %s\n", err.message);
      return 1;
    }
  if (from_stdin)
    {
      char *text = read_stdin_all ();
      if (!text)
        {
          fprintf (stderr, "termscript: cannot read stdin\n");
          dt_termscript_free (vm);
          return 1;
        }
      code = dt_termscript_compile_to_c (vm, text, "termscript_unit",
                                         &err);
      free (text);
    }
  else
    {
      /* Reuse run_file text? compile_to_c needs source text; read it. */
      FILE *sf = fopen (source, "rb");
      char *text = NULL;
      size_t cap = 8192, len = 0, r;
      if (!sf)
        {
          fprintf (stderr, "termscript: cannot open '%s': %s\n", source,
                   strerror (errno));
          dt_termscript_free (vm);
          return 1;
        }
      text = malloc (cap);
      if (!text)
        {
          fclose (sf);
          dt_termscript_free (vm);
          return 1;
        }
      while ((r = fread (text + len, 1, cap - len - 1, sf)) > 0)
        {
          char *nb;
          len += r;
          if (len + 1 < cap)
            continue;
          if (cap >= TS_DRIVER_MAX_STDIN)
            {
              fprintf (stderr, "termscript: script too large\n");
              free (text);
              fclose (sf);
              dt_termscript_free (vm);
              return 1;
            }
          cap *= 2;
          nb = realloc (text, cap);
          if (!nb)
            {
              free (text);
              fclose (sf);
              dt_termscript_free (vm);
              return 1;
            }
          text = nb;
        }
      fclose (sf);
      text[len] = '\0';
      code = dt_termscript_compile_to_c (vm, text, "termscript_unit",
                                         &err);
      free (text);
    }
  if (!code)
    {
      fprintf (stderr, "termscript: %s\n", err.message);
      dt_termscript_free (vm);
      return 1;
    }
  fd = mkstemps (tmp, 2);
  if (fd < 0)
    {
      fprintf (stderr, "termscript: cannot create temp file: %s\n",
               strerror (errno));
      dt_termscript_free (vm);
      return 1;
    }
  f = fdopen (fd, "w");
  if (!f)
    {
      close (fd);
      unlink (tmp);
      dt_termscript_free (vm);
      return 1;
    }
  fputs (code, f);
  fclose (f);
  dt_termscript_free (vm);
  {
    char cmd[4096];
    /* Native modules link statically: the stdlib archive carries every
     * std.* native module (termscript-side companions are already
     * compiled into the unit, so no .tsc lookup happens at run time). */
    snprintf (cmd, sizeof cmd, "%s %s -o '%s' '%s' -ltermscript -ltermscript_stdlib -ltermscript_standalone -ldomterm",
              cc ? cc : "cc", link_static ? "-static" : "", out_path,
              tmp);
    rc = system (cmd);
  }
  unlink (tmp);
  if (rc != 0)
    {
      fprintf (stderr, "termscript: link failed (rc=%d)\n", rc);
      return 1;
    }
  return 0;
}

int
main (int argc, char **argv)
{
  bool emit_only = false;
  bool link_static = false;
  const char *out_path = NULL;
  const char *cc = NULL;
  const char *script = NULL;
  int i;
  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--help") == 0 || strcmp (argv[i], "-h") == 0)
        {
          usage (stdout);
          return 0;
        }
      else if (strcmp (argv[i], "-c") == 0)
        emit_only = true;
      else if (strcmp (argv[i], "--static") == 0)
        link_static = true;
      else if (strcmp (argv[i], "-o") == 0)
        {
          if (++i >= argc)
            {
              usage (stderr);
              return 2;
            }
          out_path = argv[i];
        }
      else if (strcmp (argv[i], "--cc") == 0)
        {
          if (++i >= argc)
            {
              usage (stderr);
              return 2;
            }
          cc = argv[i];
        }
      else if (argv[i][0] == '-')
        {
          if (strcmp (argv[i], "-") == 0 && !script)
            script = "-";
          else
            {
              usage (stderr);
              return 2;
            }
        }
      else if (!script)
        script = argv[i];
      else
        {
          usage (stderr);
          return 2;
        }
    }
  if (!script)
    {
      usage (stderr);
      return 2;
    }
  {
    bool from_stdin = strcmp (script, "-") == 0;
    if (emit_only)
      return emit_c (script, from_stdin, out_path);
    if (out_path)
      {
        if (link_static)
          fprintf (stderr, "termscript: note: --static is for the link step\n");
        return build_exe (script, from_stdin, out_path, link_static,
                          cc);
      }
    if (link_static)
      fprintf (stderr,
               "termscript: warning: --static without -o does nothing\n");
    if (cc)
      fprintf (stderr,
               "termscript: warning: --cc without -o does nothing\n");
    return run_vm (script, from_stdin);
  }
}
