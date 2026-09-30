/* signal.c -- std.signal: signal numbers, names and delivery.
 *
 * No handlers (Termscript has no callbacks by design): send() wraps
 * kill(2), name()/num() translate, self() reports getpid().
 */
#include "signal/signal.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct
{
  int num;
  const char *name;
} sig_entry_t;

static const sig_entry_t sig_table[] = {
  { SIGHUP, "HUP" }, { SIGINT, "INT" }, { SIGQUIT, "QUIT" },
  { SIGILL, "ILL" }, { SIGTRAP, "TRAP" }, { SIGABRT, "ABRT" },
  { SIGBUS, "BUS" }, { SIGFPE, "FPE" }, { SIGKILL, "KILL" },
  { SIGUSR1, "USR1" }, { SIGSEGV, "SEGV" }, { SIGUSR2, "USR2" },
  { SIGPIPE, "PIPE" }, { SIGALRM, "ALRM" }, { SIGTERM, "TERM" },
#ifdef SIGSTKFLT
  { SIGSTKFLT, "STKFLT" },
#endif
  { SIGCHLD, "CHLD" }, { SIGCONT, "CONT" }, { SIGSTOP, "STOP" },
  { SIGTSTP, "TSTP" }, { SIGTTIN, "TTIN" }, { SIGTTOU, "TTOU" },
  { SIGURG, "URG" }, { SIGXCPU, "XCPU" }, { SIGXFSZ, "XFSZ" },
  { SIGVTALRM, "VTALRM" }, { SIGPROF, "PROF" }, { SIGWINCH, "WINCH" },
  { SIGIO, "IO" }, { SIGPWR, "PWR" }, { SIGSYS, "SYS" },
  { 0, NULL }
};

static TS_Status
s_send (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  int64_t pid, sig;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &pid, error, "send") != TS_OK ||
      ts_std_int (&argv[1], &sig, error, "send") != TS_OK)
    return TS_ERR_INVAL;
  if (pid <= 0 || sig < 0 || sig > 64)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "send: bad pid/sig");
      return TS_ERR_INVAL;
    }
  if (kill ((pid_t) pid, (int) sig) != 0)
    {
      if (errno == ESRCH || errno == EPERM)
        {
          ts_std_ret_bool (ret, false);
          return TS_OK;
        }
      ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "send: %s",
                    strerror (errno));
      return TS_ERR_SYSTEM;
    }
  ts_std_ret_bool (ret, true);
  return TS_OK;
}

static TS_Status
s_name (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  int64_t sig;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "name") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &sig, error, "name") != TS_OK)
    return TS_ERR_INVAL;
  for (i = 0; sig_table[i].name; i++)
    if (sig_table[i].num == (int) sig)
      return ts_std_ret_str (ret, sig_table[i].name, error);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_num (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  const char *name;
  size_t i;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "num") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[0], &name, error, "num") != TS_OK)
    return TS_ERR_INVAL;
  if (strncmp (name, "SIG", 3) == 0)
    name += 3;
  for (i = 0; sig_table[i].name; i++)
    if (strcmp (name, sig_table[i].name) == 0)
      {
        ts_std_ret_int (ret, sig_table[i].num);
        return TS_OK;
      }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_self (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "self") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) getpid ());
  return TS_OK;
}

static const TS_FuncDef signal_funcs[] = {
  { "send", s_send, NULL },
  { "name", s_name, NULL },
  { "num", s_num, NULL },
  { "self", s_self, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_signal_module = { "std.signal", signal_funcs };
