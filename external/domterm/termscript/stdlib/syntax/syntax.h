#ifndef TS_STD_SYNTAX_H
#define TS_STD_SYNTAX_H
#include <termscript/termscript.h>

extern const TS_Module ts_std_syntax_module;

/* Highlight source with the same implementation as std.syntax:highlight.
 * On success, *out receives a malloc'd string which the caller frees.
 * This lets rendering libraries compose syntax colouring without a VM call. */
TS_Status ts_std_syntax_highlight (const char *language, const char *source,
                                   char **out, TS_Error *error);

#endif /* TS_STD_SYNTAX_H */
