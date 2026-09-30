/* Termcap / terminfo name and representable parameter-program conversion.
 * Some terminfo programs have no termcap equivalent; reject those safely. */
#include "termcap/termcap.h"
#include "common/ts_std_common.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *info,*cap; } pair;
static const pair names[]={
  {"clear","cl"},{"cup","cm"},{"home","ho"},{"el","ce"},{"ed","cd"},
  {"bold","md"},{"sgr0","me"},{"rev","mr"},{"smcup","ti"},{"rmcup","te"},
  {"civis","vi"},{"cnorm","ve"},{"bel","bl"},{"cub1","le"},{"cuf1","nd"},
  {"cuu1","up"},{"cud1","do"},{"cols","co"},{"lines","li"},{"colors","Co"},
  {"setaf","AF"},{"setab","AB"},{"kcuu1","ku"},{"kcud1","kd"},
  {"kcub1","kl"},{"kcuf1","kr"},{"dch1","dc"},{"ich1","ic"},
  {"il1","al"},{"dl1","dl"},{"smso","so"},{"rmso","se"},
  {"smul","us"},{"rmul","ue"},{"cr","cr"},{"ht","ta"},{"ind","sf"},
  {"ri","sr"},{NULL,NULL}
};
static TS_Status fail(TS_Error *e,TS_Status st,const char *message){ts_error_set(e,st,0,0,message);return st;}
static TS_Status name_convert(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){const char *s;size_t i;int reverse=(int)(intptr_t)u;if(ts_std_argc(v,n,1,1,e,"capability name")!=TS_OK||ts_std_str(a,&s,e,"capability name")!=TS_OK)return TS_ERR_INVAL;for(i=0;names[i].info;i++)if(!strcmp(s,reverse?names[i].cap:names[i].info))return ts_std_ret_str(r,reverse?names[i].info:names[i].cap,e);ts_std_ret_nil(r);return TS_OK;}
static int emit(ts_sbuf_t *b,const char *s){return ts_sbuf_str(b,s);}
/* i: terminfo -> termcap. Common stack forms are lowered to the
 * implicit-current-parameter termcap language. Parameter order switches
 * are expressible only as the initial %r directive. */
static int info_program(ts_sbuf_t *b,const char *s){size_t i=0;int current=1,seen=0;
  if(strstr(s,"%p2") && strstr(s,"%p1") && strstr(s,"%p2")<strstr(s,"%p1")){
    if(emit(b,"%r"))return -1;
    current=2;
  }
  while(s[i]){
    if(s[i]!='%'){if(ts_sbuf_ch(b,s[i++]))return -1;continue;}
    i++;
    if(s[i]=='%'){if(emit(b,"%%"))return -1;i++;continue;}
    if(s[i]=='i'){if(emit(b,"%i"))return -1;i++;continue;}
    if(s[i]=='p' && (s[i+1]=='1'||s[i+1]=='2')){
      int p=s[i+1]-'0';i+=2;
      if(p!=current || s[i]!='%')return -2;
      i++;
      if(s[i]=='d'){if(emit(b,"%d"))return -1;i++;}
      else if(s[i]=='c'){if(emit(b,"%."))return -1;i++;}
      else if(s[i]=='2' && s[i+1]=='d'){if(emit(b,"%2"))return -1;i+=2;}
      else if(s[i]=='3' && s[i+1]=='d'){if(emit(b,"%3"))return -1;i+=2;}
      else return -2;
      current=current==1?2:1;seen++;
      continue;
    }
    /* %i%d is not a valid terminfo program (no pushed argument). */
    return -2;
  }
  (void)seen;return 0;
}
static int cap_program(ts_sbuf_t *b,const char *s){size_t i=0;int current=1;
  while(s[i]){
    if(s[i]!='%'){if(ts_sbuf_ch(b,s[i++]))return -1;continue;}
    i++;
    if(s[i]=='%'){if(emit(b,"%%"))return -1;i++;continue;}
    if(s[i]=='i'){if(emit(b,"%i"))return -1;i++;continue;}
    if(s[i]=='r'){
      if(current!=1)return -2;
      current=2;i++;continue;
    }
    if(s[i]=='d'||s[i]=='.'||s[i]=='2'||s[i]=='3'){
      char param[4]={'%','p',(char)('0'+current),0};
      if(emit(b,param))return -1;
      if(s[i]=='d' && emit(b,"%d"))return -1;
      if(s[i]=='.' && emit(b,"%c"))return -1;
      if(s[i]=='2' && emit(b,"%2d"))return -1;
      if(s[i]=='3' && emit(b,"%3d"))return -1;
      i++;current=current==1?2:1;continue;
    }
    return -2;
  }
  return 0;
}
/* Preserve existing source-level backslash escapes. Encode raw control
 * characters, and quote termcap's colon separator when necessary. */
static int encode(ts_sbuf_t *b,const char *s,int cap){size_t i;for(i=0;s[i];i++){
  unsigned char c=(unsigned char)s[i];
  if(c=='\\'&&s[i+1]){if(ts_sbuf_ch(b,'\\')||ts_sbuf_ch(b,s[++i]))return -1;}
  else if(c==':'&&cap){if(emit(b,"\\:"))return -1;}
  else if(c==27){if(emit(b,"\\E"))return -1;}
  else if(c=='\n'){if(emit(b,"\\n"))return -1;}
  else if(c=='\r'){if(emit(b,"\\r"))return -1;}
  else if(c=='\t'){if(emit(b,"\\t"))return -1;}
  else if(c<32||c==127){if(ts_sbuf_printf(b,"\\%03o",c))return -1;}
  else if(ts_sbuf_ch(b,c))return -1;
}return 0;}
static TS_Status entry(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){const char *name,*value,*alias=NULL;ts_sbuf_t program,entry;size_t i;char *out;int rc,reverse=(int)(intptr_t)u;TS_Status st;
  if(ts_std_argc(v,n,2,2,e,"capability entry")!=TS_OK||ts_std_str(a,&name,e,"capability entry")!=TS_OK||ts_std_str(a+1,&value,e,"capability entry")!=TS_OK)return TS_ERR_INVAL;
  for(i=0;names[i].info;i++)if(!strcmp(name,reverse?names[i].cap:names[i].info)){alias=reverse?names[i].info:names[i].cap;break;}
  if(!alias){ts_std_ret_nil(r);return TS_OK;}
  ts_sbuf_init(&program);ts_sbuf_init(&entry);
  rc=reverse?cap_program(&program,value):info_program(&program,value);
  if(rc==-2){st=fail(e,TS_ERR_INVAL,"capability parameter program is not representable");goto done;}
  if(rc || emit(&entry,alias)||ts_sbuf_ch(&entry,'=')||encode(&entry,program.data?program.data:"",!reverse)||ts_sbuf_ch(&entry,reverse?',':':')){st=fail(e,TS_ERR_NOMEM,"out of memory");goto done;}
  out=ts_sbuf_take(&entry);if(!out){st=fail(e,TS_ERR_NOMEM,"out of memory");goto done;}
  st=ts_std_ret_str(r,out,e);free(out);
done:ts_sbuf_free(&entry);ts_sbuf_free(&program);return st;
}
static const TS_FuncDef funcs[]={{"to_termcap_name",name_convert,NULL},{"to_terminfo_name",name_convert,(void *)1},{"to_termcap_entry",entry,NULL},{"to_terminfo_entry",entry,(void *)1},{NULL,NULL,NULL}};
const TS_Module ts_std_termcap_module={"std.termcap",funcs};
