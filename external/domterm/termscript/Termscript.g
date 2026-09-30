%{
/* Termscript.g -- Termscript language grammar for scripts/aurocks.pl.
 *
 * Generate the parser with:
 *   perl scripts/aurocks.pl domlibs/domterm/termscript/Termscript.g \
 *     > termscript_parser.c
 *
 * The generated parser is a scannerless PEG-style recursive-descent
 * parser: alternatives are tried in order with full backtracking, the
 * first complete match wins, and the entry point (generated with
 * --entrypoint ts_gram_parse) only succeeds when
 * the whole input is consumed.  The grammar below is written so that
 * correctness never depends on alternative order:
 *
 * - Statement, argument and primary alternatives have pairwise-disjoint
 *   success sets (verified by construction; see the notes on each rule).
 * The generated parser runs %skip before every terminal, so a boundary
 *   check in a separate rule can never observe the character after a
 *   keyword (the space is already gone).  Keywords are therefore matched
 *   as single regex terminals that include one trailing boundary
 *   character or end-of-input (`/const([^A-Za-z0-9_]|$)/`); the ts_kw
 *   helper pushes the boundary character back so no input is lost.
 *   This keeps `constant = 5;` (assign) distinct from `const x = 5;`
 *   without depending on alternative order.
 * - The words `or`, `true`, `false`, `nil` are dispatched in C helpers
 *   from a single IDENT terminal (ts_check_or for the operator;
 *   ts_mk_unary / ts_mk_argword map lone true/false/nil to literals),
 *   which also makes `or` effectively reserved: greedy call arguments
 *   can never swallow an `or` operator.
 * - Call-vs-variable is left-factored (`ident untail`) instead of
 *   relying on alternative order.
 * - `%empty` alternatives sort last by generator construction, which the
 *   `untail` and `if_tail` rules rely on.
 *
 * Semantic actions build the dt_node_t AST implemented in
 * domlibs/domterm/termscript/ts_runtime.c.  Action constraints imposed by
 * scripts/aurocks.pl (all honored here):
 *
 * - One C call per action, no braces inside actions.
 * - `vN->...` on semantic values is rewritten by the generator, so
 *   actions never dereference values; all logic lives in ts_* helpers.
 * - Only the LAST matched terminal's text is visible (`token`); earlier
 *   terminals in the same alternative are inaccessible.  Every action
 *   consumes `token` exactly once: a ts_* helper adopts it, or the
 *   action frees it.  Unreachable earlier token buffers are an
 *   aurocks-runtime characteristic (each terminal match mallocs); see
 *   "Memory" below.
 * - `$TOKEN`/`token` must not be referenced in left-recursive tail
 *   actions (only `tail_token` exists there); tail actions below only
 *   use v1/v2/v3.
 * - Bare punctuation symbols are silently dropped by the generator, so
 *   every terminal here is quoted (";") or a /regex/.
 * - No `//` or block comments inside the rule section (stripped by the
 *   generator before parsing rules).
 *
 * Memory: successful parses adopt every AST node and every adopted
 * token into the returned tree (freed with dt_node_free).  Non-adopted
 * terminal buffers from successful alternatives, and partial trees from
 * abandoned alternatives on parse failure, are released only with the
 * process; this mirrors the aurocks runtime, which mallocs per terminal
 * match without a free path on backtracking.  Failed parses therefore
 * cost bounded scratch memory proportional to the input scanned.
 *
 * Line numbers: actions capture ts_curline(attempt.s) (`attempt` is in
 * scope in both seed and tail actions).  Positions rewind over trailing
 * layout, so lines are exact except when trailing `#` comment lines
 * intervene (off by the comment count, documented in ts_curline).
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* AST constructors, argument accumulators and parse helpers, all defined
 * (non-static) in domlibs/domterm/termscript/ts_runtime.c.  All take/return
 * void * across the translation-unit boundary; concrete types stay
 * private to ts_runtime.c.  Helpers adopt every `char *` they are
 * given and return NULL only on allocation failure (recording it with
 * ts_oom()) or deliberate rule rejection (ts_check_or / ts_word_guard /
 * reserved-word args, which make the action `return 0` to backtrack). */
void *ts_mk_seq (void);
void *ts_seq_append (void *seq, void *stmt);
void *ts_mk_const (char *name, void *expr, unsigned line);
void *ts_mk_assign (char *name, void *expr, unsigned line);
void *ts_mk_exprstmt (void *expr, unsigned line);
void *ts_mk_while (void *cond, void *body, unsigned line);
void *ts_mk_if (void *cond, void *thenb, void *elseb, unsigned line);
void *ts_mk_or (void *lhs, void *rhs, unsigned line);
void *ts_mk_unary (char *name, void *tail, unsigned line);
void *ts_mk_callrest (char *func, void *args, unsigned line);
void *ts_mk_ref (char *name, unsigned line);
void *ts_mk_var (char *name, unsigned line);
void *ts_mk_number (char *tok, unsigned line);
void *ts_mk_string (char *tok, unsigned line);
void *ts_mk_argstr (char *tok, unsigned line);
void *ts_mk_argnum (char *tok, unsigned line);
void *ts_mk_argword (char *tok, unsigned line);
void *ts_mk_argref (char *tok, unsigned line);
void *ts_args_new (void);
void *ts_args_add (void *args, void *arg);
void *ts_check_or (char *tok);
void *ts_kw (char *tok, const char **pos, unsigned kwlen);
char *ts_ident (char *tok);
unsigned ts_curline (const char *pos);
void ts_oom (void);
%}

%start termscript
%skip /( |\t|\r|\n|#.*)+/

%%

termscript: stmts { $$ = $1; } ;

stmts: stmts stmt { $$ = ts_seq_append(v1, v2); if (result == 0) return 0; }
     | %empty { $$ = ts_mk_seq(); if (result == 0) return 0; }
     ;

stmt: const_decl { $$ = $1; }
    | while_stmt { $$ = $1; }
    | if_stmt { $$ = $1; }
    | assign_stmt { $$ = $1; }
    | expr_stmt { $$ = $1; }
    ;

const_decl: const_kw ident "=" expr ";" { $$ = ts_mk_const(v2, v4, ts_curline(attempt.s)); free(token); if (result == 0) return 0; } ;

assign_stmt: ident "=" expr ";" { $$ = ts_mk_assign(v1, v3, ts_curline(attempt.s)); free(token); if (result == 0) return 0; } ;

expr_stmt: expr ";" { $$ = ts_mk_exprstmt(v1, ts_curline(attempt.s)); free(token); if (result == 0) return 0; } ;

while_stmt: while_kw expr do_kw stmts end_kw { $$ = ts_mk_while(v2, v4, ts_curline(attempt.s)); if (result == 0) return 0; } ;

if_stmt: if_kw expr do_kw stmts if_tail { $$ = ts_mk_if(v2, v4, v5, ts_curline(attempt.s)); if (result == 0) return 0; } ;

if_tail: else_kw stmts end_kw { $$ = $2; }
       | end_kw { $$ = 0; }
       ;

const_kw: /const([^A-Za-z0-9_]|$)/ { $$ = ts_kw(token, &attempt.s, 5); if (result == 0) return 0; } ;

while_kw: /while([^A-Za-z0-9_]|$)/ { $$ = ts_kw(token, &attempt.s, 5); if (result == 0) return 0; } ;

do_kw: /do([^A-Za-z0-9_]|$)/ { $$ = ts_kw(token, &attempt.s, 2); if (result == 0) return 0; } ;

end_kw: /end([^A-Za-z0-9_]|$)/ { $$ = ts_kw(token, &attempt.s, 3); if (result == 0) return 0; } ;

else_kw: /else([^A-Za-z0-9_]|$)/ { $$ = ts_kw(token, &attempt.s, 4); if (result == 0) return 0; } ;

if_kw: /if([^A-Za-z0-9_]|$)/ { $$ = ts_kw(token, &attempt.s, 2); if (result == 0) return 0; } ;

expr: expr or_op unary { $$ = ts_mk_or(v1, v3, ts_curline(attempt.s)); if (result == 0) return 0; }
    | unary { $$ = $1; }
    ;

or_op: IDENT { $$ = ts_check_or(v1); if (result == 0) return 0; } ;

unary: "&" IDENT { $$ = ts_mk_ref(v2, ts_curline(attempt.s)); free(token); if (result == 0) return 0; }
     | ident untail { $$ = ts_mk_unary(v1, v2, ts_curline(attempt.s)); if (result == 0) return 0; }
     | NUMBER { $$ = ts_mk_number(v1, ts_curline(attempt.s)); if (result == 0) return 0; }
     | STRING { $$ = ts_mk_string(v1, ts_curline(attempt.s)); if (result == 0) return 0; }
     ;

untail: ":" ident args { $$ = ts_mk_callrest(v2, v3, ts_curline(attempt.s)); free(token); if (result == 0) return 0; }
      | %empty { $$ = 0; }
      ;

arg: STRING { $$ = ts_mk_argstr(v1, ts_curline(attempt.s)); if (result == 0) return 0; }
   | NUMBER { $$ = ts_mk_argnum(v1, ts_curline(attempt.s)); if (result == 0) return 0; }
   | "&" IDENT { $$ = ts_mk_argref(v2, ts_curline(attempt.s)); free(token); if (result == 0) return 0; }
   | IDENT { $$ = ts_mk_argword(v1, ts_curline(attempt.s)); if (result == 0) return 0; }
   ;

args: args arg { $$ = ts_args_add(v1, v2); if (result == 0) return 0; }
    | %empty { $$ = ts_args_new(); if (result == 0) return 0; }
    ;

ident: IDENT { $$ = ts_ident(v1); if (result == 0) return 0; } ;

NUMBER: /-?(0|[1-9][0-9]*)/ { $$ = token; } ;

STRING: /"([^"\\]|\\.)*"/ { $$ = token; } ;

IDENT: /[A-Za-z_][A-Za-z0-9_]*/ { $$ = token; } ;

%%

/* No epilogue code: the AST constructors and the evaluator live in
 * domlibs/domterm/termscript/ts_runtime.c.  The build compiles the generated
 * parser alongside it (see domlibs/domterm/CMakeLists.txt).
 */
