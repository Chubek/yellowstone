#include "word_motion/word_motion.h"
#include "common/ts_std_common.h"
#include <ctype.h>
#include <stdint.h>
#include <string.h>
static int word(unsigned char c){return isalnum(c)||c=='_';}
static TS_Status motion(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){const char *s;int64_t pos;size_t i,len;int back=(int)(intptr_t)u;if(ts_std_argc(v,n,2,2,e,"word motion")!=TS_OK || ts_std_str(a,&s,e,"word motion")!=TS_OK || ts_std_int(a+1,&pos,e,"word motion")!=TS_OK)return TS_ERR_INVAL;len=strlen(s);if(pos<0 || (uint64_t)pos>len){ts_error_set(e,TS_ERR_INVAL,0,0,"word motion: position out of range");return TS_ERR_INVAL;}i=(size_t)pos;if(back){while(i && !word((unsigned char)s[i-1]))i--;while(i && word((unsigned char)s[i-1]))i--;}else{while(i<len && word((unsigned char)s[i]))i++;while(i<len && !word((unsigned char)s[i]))i++;}ts_std_ret_int(r,(int64_t)i);return TS_OK;}
static const TS_FuncDef funcs[]={{"next",motion,NULL},{"previous",motion,(void *)1},{NULL,NULL,NULL}};
const TS_Module ts_std_word_motion_module={"std.word_motion",funcs};
