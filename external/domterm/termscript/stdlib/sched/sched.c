/* sched.c -- std.sched: clocks and sleeps.
 *
 * now_ms() is CLOCK_MONOTONIC milliseconds (differences and
 * deadlines); now_s() is wall-clock seconds; sleep_ms() blocks the
 * calling thread (one VM per thread per the threading policy).
 */
#include "sched/sched.h"

#include "common/ts_std_common.h"

#include <errno.h>
#include <time.h>

static int64_t
mono_ms (void)
{
  struct timespec ts;
  clock_gettime (CLOCK_MONOTONIC, &ts);
  return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static TS_Status
s_sleep_ms (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  int64_t ms;
  struct timespec req, rem;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "sleep_ms") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &ms, error, "sleep_ms") != TS_OK)
    return TS_ERR_INVAL;
  if (ms < 0 || ms > 3600000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "sleep_ms: bad delay");
      return TS_ERR_INVAL;
    }
  req.tv_sec = ms / 1000;
  req.tv_nsec = (ms % 1000) * 1000000L;
  while (nanosleep (&req, &rem) != 0)
    {
      if (errno != EINTR)
        {
          ts_error_set (error, TS_ERR_SYSTEM, errno, 0, "sleep failed");
          return TS_ERR_SYSTEM;
        }
      req = rem;
    }
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
s_now_ms (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "now_ms") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, mono_ms ());
  return TS_OK;
}

static TS_Status
s_now_s (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  struct timespec ts;
  (void) ud;
  (void) argv;
  if (ts_std_argc (vm, argc, 0, 0, error, "now_s") != TS_OK)
    return TS_ERR_INVAL;
  clock_gettime (CLOCK_REALTIME, &ts);
  ts_std_ret_int (ret, (int64_t) ts.tv_sec);
  return TS_OK;
}

static TS_Status
s_elapsed (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  int64_t t0;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "elapsed") != TS_OK)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[0], &t0, error, "elapsed") != TS_OK)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, mono_ms () - t0);
  return TS_OK;
}

static const TS_FuncDef sched_funcs[] = {
  { "sleep_ms", s_sleep_ms, NULL },
  { "now_ms", s_now_ms, NULL },
  { "now_s", s_now_s, NULL },
  { "elapsed", s_elapsed, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_sched_module = { "std.sched", sched_funcs };
