/* table.c -- std.table: ruled text tables over separator-delimited rows.
 *
 * render(headers, rows[, column_separator[, row_separator]]) uses "|" and
 * "\n" by default.  It deliberately accepts simple text, not CSV: use a
 * separator that cannot occur in a cell when values may contain pipes.
 */
#include "table/table.h"

#include "common/ts_std_common.h"
#include "rules/rules.h"

#include <stdlib.h>
#include <string.h>

#define TABLE_MAX_COLUMNS 128
#define TABLE_MAX_CELL_WIDTH 10000

static TS_Status
oom (TS_Error *error)
{
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
bad (TS_Error *error, const char *message)
{
  ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s", message);
  return TS_ERR_INVAL;
}

static TS_Status
measure (const char *rows, const char *column_separator,
         const char *row_separator, size_t **widths_out, size_t *count_out,
         TS_Error *error)
{
  const size_t collen = strlen (column_separator);
  const size_t rowlen = strlen (row_separator);
  const char *row = rows;
  size_t *widths = NULL;
  size_t count = 0;
  for (;;)
    {
      const char *row_end = strstr (row, row_separator);
      const char *limit = row_end ? row_end : row + strlen (row);
      const char *cell = row;
      size_t column = 0;
      for (;;)
        {
          const char *split = strstr (cell, column_separator);
          const char *end = split && split < limit ? split : limit;
          size_t width = (size_t) (end - cell);
          if (width > TABLE_MAX_CELL_WIDTH || column >= TABLE_MAX_COLUMNS)
            {
              free (widths);
              return bad (error, "table: too many or too wide cells");
            }
          if (column >= count)
            {
              size_t *grown = realloc (widths, (column + 1) * sizeof *grown);
              if (!grown)
                {
                  free (widths);
                  return oom (error);
                }
              widths = grown;
              widths[column] = 0;
              count = column + 1;
            }
          if (width > widths[column])
            widths[column] = width;
          if (end == limit)
            break;
          cell = split + collen;
          column++;
        }
      if (!row_end)
        break;
      row = row_end + rowlen;
    }
  *widths_out = widths;
  *count_out = count;
  return TS_OK;
}

static int
append_row (ts_sbuf_t *out, const char *row, const char *column_separator,
            size_t row_length, const size_t *widths, size_t columns)
{
  const size_t separator_length = strlen (column_separator);
  const char *cell = row;
  const char *limit = row + row_length;
  size_t column;
  if (ts_sbuf_ch (out, '|') != 0)
    return -1;
  for (column = 0; column < columns; column++)
    {
      const char *end = cell;
      size_t length = 0;
      size_t padding;
      if (cell < limit)
        {
          const char *split = strstr (cell, column_separator);
          end = split && split < limit ? split : limit;
          length = (size_t) (end - cell);
        }
      if (ts_sbuf_ch (out, ' ') != 0 ||
          ts_sbuf_put (out, cell, length) != 0)
        return -1;
      for (padding = length; padding < widths[column]; padding++)
        if (ts_sbuf_ch (out, ' ') != 0)
          return -1;
      if (ts_sbuf_ch (out, ' ') != 0 || ts_sbuf_ch (out, '|') != 0)
        return -1;
      if (cell < limit && end != limit)
        cell = end + separator_length;
      else
        cell = limit;
    }
  return 0;
}

static int
append_border (ts_sbuf_t *out, const size_t *widths, size_t columns,
               char fill)
{
  char *border = ts_std_rules_table_border (widths, columns, fill);
  int status;
  if (!border)
    return -1;
  status = ts_sbuf_str (out, border);
  free (border);
  return status;
}

static TS_Status
t_render (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  const char *headers;
  const char *rows;
  const char *column_separator = "|";
  const char *row_separator = "\n";
  size_t *widths = NULL;
  size_t columns = 0;
  const char *row;
  ts_sbuf_t out;
  TS_Status status;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 4, error, "render") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &headers, error, "render") != TS_OK ||
      ts_std_str (&argv[1], &rows, error, "render") != TS_OK ||
      (argc >= 3 && ts_std_str (&argv[2], &column_separator, error, "render") != TS_OK) ||
      (argc == 4 && ts_std_str (&argv[3], &row_separator, error, "render") != TS_OK))
    return TS_ERR_INVAL;
  if (!column_separator[0] || !row_separator[0])
    return bad (error, "render: separators must not be empty");
  if (strstr (headers, row_separator))
    return bad (error, "render: headers must be one row");
  status = measure (headers, column_separator, row_separator, &widths,
                    &columns, error);
  if (status != TS_OK)
    return status;
  {
    size_t *row_widths = NULL;
    size_t row_columns = 0;
    size_t row_count;
    status = measure (rows, column_separator, row_separator, &row_widths,
                      &row_columns, error);
    if (status != TS_OK)
      {
        free (widths);
        return status;
      }
    row_count = row_columns;
    if (row_count > columns)
      {
        size_t *grown = realloc (widths, row_count * sizeof *grown);
        if (!grown)
          {
            free (row_widths);
            free (widths);
            return oom (error);
          }
        widths = grown;
        for (; columns < row_count; columns++)
          widths[columns] = 0;
      }
    for (row_columns = 0; row_columns < row_count; row_columns++)
      if (row_widths[row_columns] > widths[row_columns])
        widths[row_columns] = row_widths[row_columns];
    free (row_widths);
  }
  ts_sbuf_init (&out);
  if (append_border (&out, widths, columns, '-') != 0 ||
      ts_sbuf_ch (&out, '\n') != 0 ||
      append_row (&out, headers, column_separator, strlen (headers), widths, columns) != 0 ||
      ts_sbuf_ch (&out, '\n') != 0 ||
      append_border (&out, widths, columns, '=') != 0)
    goto out_of_memory;
  for (row = rows;;)
    {
      const char *row_end = strstr (row, row_separator);
      size_t row_length = row_end ? (size_t) (row_end - row) : strlen (row);
      if (ts_sbuf_ch (&out, '\n') != 0 ||
          append_row (&out, row, column_separator, row_length, widths, columns) != 0 ||
          ts_sbuf_ch (&out, '\n') != 0 ||
          append_border (&out, widths, columns, '-') != 0)
        goto out_of_memory;
      if (!row_end)
        break;
      row = row_end + strlen (row_separator);
    }
  free (widths);
  {
    char *text = ts_sbuf_take (&out);
    ts_value_free (ret);
    ret->type = TS_STRING;
    ret->as.string = text;
    if (text)
      return TS_OK;
  }
out_of_memory:
  free (widths);
  ts_sbuf_free (&out);
  return oom (error);
}

static const TS_FuncDef table_funcs[] = {
  { "render", t_render, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_table_module = { "std.table", table_funcs };
