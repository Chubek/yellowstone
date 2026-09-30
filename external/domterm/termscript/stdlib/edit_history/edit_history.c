/* edit_history.c -- std.edit_history: bounded snapshot undo/redo.
 *
 * The history owns complete editor states rather than patches.  It is a
 * small bridge for readline- and REPL-style editors: record() adds a state,
 * undo()/redo() select a state, and a new record after undo discards redo.
 * Returned strings are Termscript-owned copies.  Handles are not thread-safe.
 */
#include "edit_history/edit_history.h"

#include "common/ts_std_common.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
  char **items;
  size_t len;
  size_t cap;
  size_t cursor;
  size_t limit; /* Zero means unbounded. */
} history_t;

static const char history_tag;

static TS_Status
oom (TS_Error *error)
{
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static void
history_free (void *payload)
{
  history_t *history = payload;
  size_t i;
  if (!history)
    return;
  for (i = 0; i < history->len; i++)
    free (history->items[i]);
  free (history->items);
  free (history);
}

static TS_Status
ensure_capacity (history_t *history, size_t wanted, TS_Error *error)
{
  size_t cap;
  char **items;
  if (wanted <= history->cap)
    return TS_OK;
  cap = history->cap ? history->cap : 4;
  while (cap < wanted)
    {
      if (cap > SIZE_MAX / 2)
        return oom (error);
      cap *= 2;
    }
  items = realloc (history->items, cap * sizeof *items);
  if (!items)
    return oom (error);
  history->items = items;
  history->cap = cap;
  return TS_OK;
}

static void
discard_after_cursor (history_t *history)
{
  size_t i;
  for (i = history->cursor + 1; i < history->len; i++)
    free (history->items[i]);
  history->len = history->cursor + 1;
}

static void
trim_to_limit (history_t *history)
{
  size_t remove;
  size_t i;
  if (!history->limit || history->len <= history->limit)
    return;
  remove = history->len - history->limit;
  for (i = 0; i < remove; i++)
    free (history->items[i]);
  memmove (history->items, history->items + remove,
           (history->len - remove) * sizeof *history->items);
  history->len -= remove;
  /* A limit is a rolling window over the newest snapshots.  If the active
   * snapshot fell outside that window, select its oldest retained entry. */
  history->cursor = history->cursor >= remove ? history->cursor - remove : 0;
}

static history_t *
get_history (const TS_Value *value, TS_Error *error, const char *name)
{
  return ts_std_handle (value, &history_tag, error, name);
}

static TS_Status
h_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *initial = "";
  history_t *history;
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 1, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc && ts_std_str (&argv[0], &initial, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  history = calloc (1, sizeof *history);
  if (!history)
    return oom (error);
  if (ensure_capacity (history, 1, error) != TS_OK)
    {
      history_free (history);
      return TS_ERR_NOMEM;
    }
  history->items[0] = strdup (initial);
  if (!history->items[0])
    {
      history_free (history);
      return oom (error);
    }
  history->len = 1;
  if (ts_value_make_handle (ret, history, history_free, "<edit_history>",
                            &history_tag) != TS_OK)
    {
      history_free (history);
      return oom (error);
    }
  return TS_OK;
}

static TS_Status
h_record (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  history_t *history;
  const char *snapshot;
  char *copy;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "record") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "record");
  if (!history || ts_std_str (&argv[1], &snapshot, error, "record") != TS_OK)
    return TS_ERR_INVAL;
  if (strcmp (history->items[history->cursor], snapshot) == 0)
    return ts_std_ret_str (ret, snapshot, error);
  copy = strdup (snapshot);
  if (!copy)
    return oom (error);
  discard_after_cursor (history);
  if (ensure_capacity (history, history->len + 1, error) != TS_OK)
    {
      free (copy);
      return TS_ERR_NOMEM;
    }
  history->items[history->len++] = copy;
  history->cursor = history->len - 1;
  trim_to_limit (history);
  return ts_std_ret_str (ret, history->items[history->cursor], error);
}

static TS_Status
h_undo (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  history_t *history;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "undo") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "undo");
  if (!history)
    return TS_ERR_INVAL;
  if (history->cursor)
    history->cursor--;
  return ts_std_ret_str (ret, history->items[history->cursor], error);
}

static TS_Status
h_redo (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  history_t *history;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "redo") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "redo");
  if (!history)
    return TS_ERR_INVAL;
  if (history->cursor + 1 < history->len)
    history->cursor++;
  return ts_std_ret_str (ret, history->items[history->cursor], error);
}

static TS_Status
h_current (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  history_t *history;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "current") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "current");
  if (!history)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, history->items[history->cursor], error);
}

static TS_Status
h_position (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  history_t *history;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "position") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "position");
  if (!history)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) history->cursor);
  return TS_OK;
}

static TS_Status
h_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  history_t *history;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "len") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "len");
  if (!history)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) history->len);
  return TS_OK;
}

static TS_Status
h_at (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
      TS_Value *ret, TS_Error *error)
{
  history_t *history;
  int64_t index;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "at") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "at");
  if (!history || ts_std_int (&argv[1], &index, error, "at") != TS_OK)
    return TS_ERR_INVAL;
  if (index < 0 || (uint64_t) index >= history->len)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  return ts_std_ret_str (ret, history->items[index], error);
}

static TS_Status
h_limit (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  history_t *history;
  int64_t limit;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "limit") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "limit");
  if (!history || ts_std_int (&argv[1], &limit, error, "limit") != TS_OK)
    return TS_ERR_INVAL;
  if (limit < 1)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "limit: must be positive");
      return TS_ERR_INVAL;
    }
  history->limit = (size_t) limit;
  trim_to_limit (history);
  ts_std_ret_int (ret, (int64_t) history->limit);
  return TS_OK;
}

static TS_Status
h_clear (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  history_t *history;
  char *current;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "clear") != TS_OK)
    return TS_ERR_INVAL;
  history = get_history (&argv[0], error, "clear");
  if (!history)
    return TS_ERR_INVAL;
  current = strdup (history->items[history->cursor]);
  if (!current)
    return oom (error);
  for (i = 0; i < history->len; i++)
    free (history->items[i]);
  history->items[0] = current;
  history->len = 1;
  history->cursor = 0;
  return ts_std_ret_str (ret, current, error);
}

static const TS_FuncDef history_funcs[] = {
  { "new", h_new, NULL },
  { "record", h_record, NULL },
  { "add", h_record, NULL },
  { "undo", h_undo, NULL },
  { "previous", h_undo, NULL },
  { "redo", h_redo, NULL },
  { "next", h_redo, NULL },
  { "current", h_current, NULL },
  { "position", h_position, NULL },
  { "len", h_len, NULL },
  { "at", h_at, NULL },
  { "limit", h_limit, NULL },
  { "clear", h_clear, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_edit_history_module = {
  "std.edit_history", history_funcs
};
