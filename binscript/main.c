#include "binscript.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char**argv){if(argc<2){fprintf(stderr,"usage: binscript FILE\n");return 2;}FILE*f=fopen(argv[1],"rb");if(!f)return 1;fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*s=malloc((size_t)n+1);fread(s,1,(size_t)n,f);s[n]=0;fclose(f);BS_Error e={0};BS_VM*v=bs_vm_create(&e);char*out=0;int rc=bs_vm_run(v,s,&out,&e);if(out){fputs(out,stdout);free(out);}bs_vm_free(v);free(s);return rc;}
