#include "../termscript.h"

/* Optional stdlib shim.  It deliberately leaves only the core G module
 * available; applications can register their own modules normally. */
void ts_stdlib_register_all(TS_VM *vm) { (void)vm; }
const char *ts_stdlib_search_path(void) { return ""; }
