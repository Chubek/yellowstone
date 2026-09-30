/* ts_stdlib.c -- registry for the Termscript standard library.
 *
 * One table pairs every native module with its companion name; the
 * companions themselves are data (installed .tsc files, resolved at
 * import time through the VM search path).
 */
#include "ts_stdlib.h"

static const TS_Module *const stdlib_modules[] = {
  &ts_std_array_module,
  &ts_std_autocomp_module,
  &ts_std_buffer_module,
  &ts_std_codec_module,
  &ts_std_color_module,
  &ts_std_csv_module,
  &ts_std_draw_module,
  &ts_std_edit_history_module,
  &ts_std_env_module,
  &ts_std_exec_module,
  &ts_std_expect_module,
  &ts_std_fsm_module,
  &ts_std_glob_module,
  &ts_std_http_module,
  &ts_std_io_module,
  &ts_std_ipc_module,
  &ts_std_json_module,
  &ts_std_key_module,
  &ts_std_list_module,
  &ts_std_log_module,
  &ts_std_map_module,
  &ts_std_menu_module,
  &ts_std_notify_module,
  &ts_std_panel_module,
  &ts_std_pipe_module,
  &ts_std_pty_module,
  &ts_std_prompt_module,
  &ts_std_readline_module,
  &ts_std_regex_module,
  &ts_std_rules_module,
  &ts_std_sched_module,
  &ts_std_screen_module,
  &ts_std_signal_module,
  &ts_std_socket_module,
  &ts_std_stream_module,
  &ts_std_style_module,
  &ts_std_syntax_module,
  &ts_std_table_module,
  &ts_std_termcap_module,
  &ts_std_terminfo_module,
  &ts_std_test_module,
  &ts_std_text_search_module,
  &ts_std_theme_module,
  &ts_std_toml_module,
  &ts_std_verbatim_module,
  &ts_std_vterm_module,
  &ts_std_widget_module,
  &ts_std_word_motion_module,
  &ts_std_yaml_module,
};

TS_Status
ts_stdlib_register_all (TS_VM *vm, TS_Error *error)
{
  size_t i;
  if (!vm)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad vm");
      return TS_ERR_INVAL;
    }
  for (i = 0;
       i < sizeof stdlib_modules / sizeof stdlib_modules[0]; i++)
    {
      TS_Status st = ts_vm_register_module (vm, stdlib_modules[i],
                                             error);
      if (st != TS_OK)
        return st;
    }
  return TS_OK;
}

const char *
ts_stdlib_default_dir (void)
{
#ifdef TERMSCRIPT_STDLIB_DIR
  return TERMSCRIPT_STDLIB_DIR;
#else
  return "./stdlib";
#endif
}

TS_Status
ts_stdlib_search_path (TS_VM *vm, TS_Error *error)
{
  TS_Status st;
  if (!vm)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "bad vm");
      return TS_ERR_INVAL;
    }
  st = ts_vm_add_search_path (vm, ts_stdlib_default_dir (), error);
  if (st != TS_OK)
    return st;
#ifdef TERMSCRIPT_STDLIB_SRC
  /* Build-tree fallback so the driver, tests and embeds work before
   * (or without) installation; the install dir above wins whenever
   * both exist.  Absent paths are harmless: probing skips them. */
  return ts_vm_add_search_path (vm, TERMSCRIPT_STDLIB_SRC, error);
#else
  return TS_OK;
#endif
}
