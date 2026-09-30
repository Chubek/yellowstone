#include "text_search/text_search.h"
#include "common/ts_std_common.h"
#include <stdint.h>
#include <string.h>
static TS_Status search(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){const char *text,*needle;int64_t start=0;size_t i,len,match;int backwards=(int)(intptr_t)u;if(ts_std_argc(v,n,2,3,e,"find")!=TS_OK || ts_std_str(a,&text,e,"find")!=TS_OK || ts_std_str(a+1,&needle,e,"find")!=TS_OK)return TS_ERR_INVAL;if(n==3 && ts_std_int(a+2,&start,e,"find")!=TS_OK)return TS_ERR_INVAL;len=strlen(text);match=strlen(needle);if(start<0 || (uint64_t)start>len){ts_std_ret_nil(r);return TS_OK;}if(!backwards){const char *hit=strstr(text+start,needle);if(hit)ts_std_ret_int(r,(int64_t)(hit-text));else ts_std_ret_nil(r);}else{size_t end=n==2?len:(size_t)start;int found=0;size_t at=0;for(i=0;i<=end && match<=len-i;i++)if(!memcmp(text+i,needle,match)){at=i;found=1;}if(found)ts_std_ret_int(r,(int64_t)at);else ts_std_ret_nil(r);}return TS_OK;}
static const TS_FuncDef funcs[]={{"find",search,NULL},{"rfind",search,(void *)1},{NULL,NULL,NULL}};
const TS_Module ts_std_text_search_module={"std.text_search",funcs};
