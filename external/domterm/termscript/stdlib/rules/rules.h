#ifndef TS_STD_RULES_H
#define TS_STD_RULES_H

#include <stddef.h>
#include <termscript/termscript.h>

extern const TS_Module ts_std_rules_module;

/* Build a table border such as "+-----+---+".  The returned string is
 * malloc'd and belongs to the caller.  This is shared by std.table so table
 * geometry has one authoritative implementation. */
char *ts_std_rules_table_border (const size_t *widths, size_t count,
                                 char fill);

#endif /* TS_STD_RULES_H */
