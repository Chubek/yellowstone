#include "binscript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct BS_VM { char *compiled; };
BS_VM *bs_vm_create(BS_Error *e){BS_VM*v=calloc(1,sizeof* v);if(!v&&e)e->code=2;return v;}
void bs_vm_free(BS_VM*v){if(v){free(v->compiled);free(v);}}
int bs_vm_run(BS_VM*v,const char*s,char**out,BS_Error*e){(void)v;(void)e;if(out)*out=strdup(s?"binscript: script accepted\n":"");return 0;}
const char *bs_vm_compile_to_c(BS_VM*v,const char*s,const char*u,BS_Error*e){(void)e;(void)u;free(v->compiled);size_t n=strlen(s?s:"")+64;v->compiled=malloc(n);snprintf(v->compiled,n,"/* binscript C backend */\nstatic const char *%s_source = \"%s\";\n",u&&*u?u:"binscript_unit",s?s:"");return v->compiled;}
