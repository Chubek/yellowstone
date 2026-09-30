%{ #include <stdlib.h> %}
%start program
%skip /( |\t|\r|\n|#.*)+/
%%
program: stmts { $$ = $1; } ;
stmts: stmts stmt { $$ = 0; } | %empty { $$ = 0; } ;
stmt: /[A-Za-z_][A-Za-z0-9_]*/ ";" { $$ = 0; } ;
%%
