/* Byte-offset editing with separate rope, gap, piece-table, line-list,
 * line-array, indexed-gap and mapped-file storage. */
#include "buffer/buffer.h"
#include "buffer/allocator.h"
#include "common/ts_std_common.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* A C ABI allows a DomMEMTk embedding host to supply a nonmoving
 * allocator without pulling C++ symbols into the standalone C library.
 * Each allocation records its own release callback and size. */
typedef union {
  max_align_t alignment;
  struct { TS_BufferAllocator owner; size_t size; } info;
} alloc_header_t;
static TS_BufferAllocator buffer_allocator;
void ts_std_buffer_set_allocator(const TS_BufferAllocator *allocator) {
  if(allocator && allocator->allocate && allocator->release)buffer_allocator=*allocator;
  else memset(&buffer_allocator,0,sizeof buffer_allocator);
}
static void *bmalloc(size_t n) {
  alloc_header_t *h;size_t bytes;
  if(n>SIZE_MAX-sizeof *h)return NULL;
  bytes=n+sizeof *h;
  h=buffer_allocator.allocate?buffer_allocator.allocate(buffer_allocator.context,bytes):malloc(bytes);
  if(!h)return NULL;
  h->info.owner=buffer_allocator;h->info.size=bytes;
  return h+1;
}
static void bfree(void *p) {
  alloc_header_t *h;if(!p)return;h=(alloc_header_t *)p-1;
  if(h->info.owner.release)h->info.owner.release(h->info.owner.context,h,h->info.size);
  else free(h);
}
static void *bcalloc(size_t n,size_t size) {
  void *p;if(size && n>SIZE_MAX/size)return NULL;
  p=bmalloc(n*size);if(p)memset(p,0,n*size);return p;
}
static void *brealloc(void *p,size_t n) {
  void *next;size_t old;
  if(!p)return bmalloc(n);
  old=((alloc_header_t *)p-1)->info.size-sizeof(alloc_header_t);
  next=bmalloc(n);if(!next)return NULL;
  memcpy(next,p,old<n?old:n);bfree(p);return next;
}
#define malloc bmalloc
#define calloc bcalloc
#define realloc brealloc
#define free bfree

enum { ROPE, GAP, PIECE, LINKED_LINES, ARRAY_LINES, INDEXED_GAP, MAPPED };
typedef struct chunk { char *text; size_t len; struct chunk *next; } chunk_t;
typedef struct piece { size_t offset, len; int added; struct piece *next; } piece_t;
typedef struct {
  int kind; size_t len;
  /* Gap storage: bytes before [gap,gap+gaplen), then the suffix. */
  char *data; size_t cap, gap, gaplen;
  size_t *index, nindex;
  /* Rope chunks and linked lines use separate chains. */
  chunk_t *chunks;
  /* Array of lines (each line includes its terminating newline). */
  char **lines; size_t *line_lengths, nlines;
  /* Piece table: immutable original, append-only added bytes, descriptors. */
  char *original, *added; size_t added_len, added_cap; piece_t *pieces;
} buffer_t;
static const char tag;
static const char *const kinds[] = {"rope", "gap", "piece_table", "linked_lines", "array_lines", "indexed_gap", "mmap", NULL};
static TS_Status error(TS_Error *e, TS_Status st, const char *s) { ts_error_set(e,st,0,0,s);return st; }
static void free_chunks(chunk_t *c) { while(c){chunk_t *next=c->next;free(c->text);free(c);c=next;} }
static void free_pieces(piece_t *p) { while(p){piece_t *next=p->next;free(p);p=next;} }
static void clear_storage(buffer_t *b) {
  size_t i;
  if(b->kind==MAPPED) { if(b->len) munmap(b->data,b->len); }
  else free(b->data);
  free(b->index);free_chunks(b->chunks);
  for(i=0;i<b->nlines;i++)free(b->lines[i]);
  free(b->lines);free(b->line_lengths);
  free(b->original);free(b->added);free_pieces(b->pieces);
  b->data=NULL;b->index=NULL;b->chunks=NULL;b->lines=NULL;b->line_lengths=NULL;
  b->original=b->added=NULL;b->pieces=NULL;
}
static void destroy(void *ptr) { buffer_t *b=ptr;if(b){clear_storage(b);free(b);} }
static int chunks_build(buffer_t *b,const char *s,size_t n) {
  chunk_t **tail=&b->chunks;size_t pos=0;
  while(pos<n || (!n && b->kind==LINKED_LINES && pos==0)) {
    size_t part=0;chunk_t *c;
    if(b->kind==ROPE) part=n-pos>1024?1024:n-pos;
    else {while(pos+part<n && s[pos+part]!='\n')part++;if(pos+part<n)part++;}
    c=calloc(1,sizeof *c);if(!c)return -1;
    c->text=malloc(part+1);if(!c->text){free(c);return -1;}
    memcpy(c->text,s+pos,part);c->text[part]=0;c->len=part;
    *tail=c;tail=&c->next;pos+=part;
    if(pos==n)break;
  }
  if(b->kind==LINKED_LINES && n && s[n-1]=='\n') {
    chunk_t *c=calloc(1,sizeof *c);
    if(!c)return -1;
    c->text=malloc(1);
    if(!c->text){free(c);return -1;}
    c->text[0]=0;
    *tail=c;
  }
  return 0;
}
static int array_build(buffer_t *b,const char *s,size_t n) {
  size_t i,count=1,start=0,k=0;
  for(i=0;i<n;i++)if(s[i]=='\n')count++;
  if(count>SIZE_MAX/sizeof *b->lines)return -1;
  b->lines=calloc(count,sizeof *b->lines);
  b->line_lengths=calloc(count,sizeof *b->line_lengths);
  if(!b->lines||!b->line_lengths)return -1;
  b->nlines=count;
  for(i=0;i<=n;i++)if(i==n || s[i]=='\n') {
    size_t end=i<n?i+1:i;
    b->lines[k]=malloc(end-start+1);if(!b->lines[k])return -1;
    memcpy(b->lines[k],s+start,end-start);b->lines[k][end-start]=0;
    b->line_lengths[k++]=end-start;start=end;
  }
  return 0;
}
static int gap_grow(buffer_t *b,size_t need) {
  char *p;size_t cap=b->cap?b->cap:32,extra;
  if(need<=b->cap)return 0;
  while(cap<need){if(cap>SIZE_MAX/2)return -1;cap*=2;}
  extra=cap-b->cap;p=realloc(b->data,cap);if(!p)return -1;
  b->data=p;
  if(b->cap)memmove(p+b->gap+b->gaplen+extra,p+b->gap+b->gaplen,b->len-b->gap);
  b->gaplen+=extra;b->cap=cap;return 0;
}
static void gap_move(buffer_t *b,size_t pos) {
  if(pos<b->gap)memmove(b->data+pos+b->gaplen,b->data+pos,b->gap-pos);
  else if(pos>b->gap)memmove(b->data+b->gap,b->data+b->gap+b->gaplen,pos-b->gap);
  b->gap=pos;
}
static int reindex(buffer_t *b) {
  size_t i,count=1,k=1,*idx;
  for(i=0;i<b->len;i++)if(b->data[i+(i>=b->gap?b->gaplen:0)]=='\n')count++;
  if(count>SIZE_MAX/sizeof *idx)return -1;
  idx=malloc(count*sizeof *idx);if(!idx)return -1;
  idx[0]=0;
  for(i=0;i<b->len;i++)if(b->data[i+(i>=b->gap?b->gaplen:0)]=='\n')idx[k++]=i+1;
  free(b->index);b->index=idx;b->nindex=count;return 0;
}
static int build(buffer_t *b,const char *s,size_t n) {
  b->len=n;
  switch(b->kind) {
    case GAP:case INDEXED_GAP:
      if(gap_grow(b,n+1))return -1;
      memcpy(b->data,s,n);b->gap=n;b->gaplen=b->cap-n;
      return b->kind==INDEXED_GAP?reindex(b):0;
    case ROPE:case LINKED_LINES:return chunks_build(b,s,n);
    case ARRAY_LINES:return array_build(b,s,n);
    case PIECE:
      b->original=malloc(n+1);if(!b->original)return -1;
      memcpy(b->original,s,n);b->original[n]=0;
      if(n){b->pieces=calloc(1,sizeof *b->pieces);if(!b->pieces)return -1;b->pieces->len=n;}
      return 0;
    default:return -1;
  }
}
static void copy_bytes(const buffer_t *b,char *out) {
  size_t off=0,i;chunk_t *c;piece_t *p;
  switch(b->kind) {
    case GAP:case INDEXED_GAP:
      memcpy(out,b->data,b->gap);
      memcpy(out+b->gap,b->data+b->gap+b->gaplen,b->len-b->gap);break;
    case MAPPED:if(b->len)memcpy(out,b->data,b->len);break;
    case ROPE:case LINKED_LINES:
      for(c=b->chunks;c;c=c->next){memcpy(out+off,c->text,c->len);off+=c->len;}break;
    case ARRAY_LINES:
      for(i=0;i<b->nlines;i++){memcpy(out+off,b->lines[i],b->line_lengths[i]);off+=b->line_lengths[i];}break;
    case PIECE:
      for(p=b->pieces;p;p=p->next){memcpy(out+off,(p->added?b->added:b->original)+p->offset,p->len);off+=p->len;}break;
  }
}
static buffer_t *unwrap(const TS_Value *v,TS_Error *e,const char *f){return ts_std_handle(v,&tag,e,f);}
static TS_Status wrap(TS_Value *r,buffer_t *b,TS_Error *e){if(ts_value_make_handle(r,b,destroy,"<buffer>",&tag)==TS_OK)return TS_OK;destroy(b);return error(e,TS_ERR_NOMEM,"out of memory");}
static TS_Status create(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){const char *kind,*text="";buffer_t *b;int k;(void)u;if(ts_std_argc(v,n,1,2,e,"new")!=TS_OK||ts_std_str(a,&kind,e,"new")!=TS_OK)return TS_ERR_INVAL;if(n==2&&ts_std_str(a+1,&text,e,"new")!=TS_OK)return TS_ERR_INVAL;for(k=0;kinds[k]&&strcmp(kind,kinds[k]);k++);if(k==MAPPED||!kinds[k])return error(e,TS_ERR_INVAL,"new: unknown or file-backed kind");b=calloc(1,sizeof *b);if(!b)return error(e,TS_ERR_NOMEM,"out of memory");b->kind=k;if(build(b,text,strlen(text))){destroy(b);return error(e,TS_ERR_NOMEM,"out of memory");}return wrap(r,b,e);}
static TS_Status map_file(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){const char *path;struct stat st;buffer_t *b;int fd;(void)u;if(ts_std_argc(v,n,1,1,e,"map_file")!=TS_OK||ts_std_str(a,&path,e,"map_file")!=TS_OK)return TS_ERR_INVAL;fd=open(path,O_RDONLY);if(fd<0)return error(e,TS_ERR_INVAL,"map_file: open failed");if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(uintmax_t)st.st_size>SIZE_MAX){close(fd);return error(e,TS_ERR_INVAL,"map_file: invalid file size");}b=calloc(1,sizeof *b);if(!b){close(fd);return error(e,TS_ERR_NOMEM,"out of memory");}b->kind=MAPPED;b->len=(size_t)st.st_size;if(b->len){b->data=mmap(NULL,b->len,PROT_READ,MAP_PRIVATE,fd,0);if(b->data==MAP_FAILED){free(b);close(fd);return error(e,TS_ERR_INVAL,"map_file: mmap failed");}}close(fd);return wrap(r,b,e);}
/* Piece-table edit: append-only add store and immutable original. Build a
 * fresh descriptor chain first so allocation failures preserve the buffer. */
static int piece_append(piece_t ***tail,int added,size_t offset,size_t len){piece_t *p;if(!len)return 0;p=calloc(1,sizeof *p);if(!p)return -1;p->added=added;p->offset=offset;p->len=len;**tail=p;*tail=&p->next;return 0;}
static int piece_edit(buffer_t *b,size_t pos,size_t del,const char *text,size_t add){piece_t *p,*newpieces=NULL,**tail=&newpieces;size_t at=0,old_add=b->added_len;int inserted=0;char *grow=NULL;
  if(add){if(old_add>SIZE_MAX-add)return -1;grow=realloc(b->added,old_add+add);if(!grow)return -1;b->added=grow;memcpy(b->added+old_add,text,add);}
  for(p=b->pieces;p;p=p->next){
    size_t end=at+p->len;
    size_t prefix=pos<=at?0:pos>=end?p->len:pos-at;
    size_t suffix=pos+del<=at?0:pos+del>=end?p->len:pos+del-at;
    if(piece_append(&tail,p->added,p->offset,prefix))goto oom;
    if(!inserted && pos<=end){
      if(piece_append(&tail,1,old_add,add))goto oom;
      inserted=1;
    }
    if(suffix<p->len && piece_append(&tail,p->added,p->offset+suffix,p->len-suffix))goto oom;
    at=end;
  }
  if(!inserted && piece_append(&tail,1,old_add,add))goto oom;
  free_pieces(b->pieces);b->pieces=newpieces;b->added_len=old_add+add;b->len=b->len-del+add;return 0;
oom:free_pieces(newpieces);return -1;
}
static TS_Status mutate(buffer_t *b,size_t pos,size_t del,const char *text,TS_Value *r,TS_Error *e){size_t add=strlen(text),newlen;buffer_t fresh;char *tmp;
  if(b->kind==MAPPED)return error(e,TS_ERR_INVAL,"edit: read-only mapped buffer");
  if(add>INT64_MAX-(b->len-del))return error(e,TS_ERR_INVAL,"edit: size overflow");
  newlen=b->len-del+add;
  if(b->kind==PIECE){if(piece_edit(b,pos,del,text,add))return error(e,TS_ERR_NOMEM,"out of memory");}
  else if(b->kind==GAP||b->kind==INDEXED_GAP){
    /* Grow before moving the gap so a failed allocation leaves contents intact. */
    if(gap_grow(b,newlen+1))return error(e,TS_ERR_NOMEM,"out of memory");
    gap_move(b,pos);b->gaplen+=del;b->len-=del;
    memcpy(b->data+b->gap,text,add);b->gap+=add;b->gaplen-=add;b->len+=add;
    if(b->kind==INDEXED_GAP&&reindex(b))return error(e,TS_ERR_NOMEM,"out of memory");
  } else {
    tmp=malloc(newlen+1);if(!tmp)return error(e,TS_ERR_NOMEM,"out of memory");
    /* Temporarily serialize to preserve the old backend on allocation failure. */
    char *old=malloc(b->len+1);if(!old){free(tmp);return error(e,TS_ERR_NOMEM,"out of memory");}
    copy_bytes(b,old);memcpy(tmp,old,pos);memcpy(tmp+pos,text,add);
    memcpy(tmp+pos+add,old+pos+del,b->len-pos-del);tmp[newlen]=0;free(old);
    memset(&fresh,0,sizeof fresh);fresh.kind=b->kind;
    if(build(&fresh,tmp,newlen)){clear_storage(&fresh);free(tmp);return error(e,TS_ERR_NOMEM,"out of memory");}
    free(tmp);clear_storage(b);*b=fresh;
  }
  ts_std_ret_int(r,(int64_t)b->len);return TS_OK;
}
static TS_Status edit(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){buffer_t *b;int64_t pos,del=0;const char *text="";int mode=(int)(intptr_t)u;size_t arity=mode==2?3:mode==1?4:3;
  if(ts_std_argc(v,n,arity,arity,e,"edit")!=TS_OK)return TS_ERR_INVAL;
  b=unwrap(a,e,"edit");if(!b||ts_std_int(a+1,&pos,e,"edit")!=TS_OK)return TS_ERR_INVAL;
  if(mode&&ts_std_int(a+2,&del,e,"edit")!=TS_OK)return TS_ERR_INVAL;
  if(mode!=2&&ts_std_str(a+(mode?3:2),&text,e,"edit")!=TS_OK)return TS_ERR_INVAL;
  if(pos<0||(uint64_t)pos>b->len||del<0||(uint64_t)del>b->len-(size_t)pos)return error(e,TS_ERR_INVAL,"edit: invalid range");
  return mutate(b,(size_t)pos,(size_t)del,text,r,e);
}
static TS_Status length(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){buffer_t *b;(void)u;if(ts_std_argc(v,n,1,1,e,"len")!=TS_OK)return TS_ERR_INVAL;b=unwrap(a,e,"len");if(!b)return TS_ERR_INVAL;ts_std_ret_int(r,(int64_t)b->len);return TS_OK;}
static TS_Status extract(const buffer_t *b,size_t start,size_t len,TS_Value *r,TS_Error *e){char *tmp=malloc(b->len+1);TS_Status st;if(!tmp)return error(e,TS_ERR_NOMEM,"out of memory");copy_bytes(b,tmp);st=ts_std_ret_strn(r,tmp+start,len,e);free(tmp);return st;}
static TS_Status slice(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){buffer_t *b;int64_t start,count;(void)u;if(ts_std_argc(v,n,3,3,e,"slice")!=TS_OK)return TS_ERR_INVAL;b=unwrap(a,e,"slice");if(!b||ts_std_int(a+1,&start,e,"slice")!=TS_OK||ts_std_int(a+2,&count,e,"slice")!=TS_OK)return TS_ERR_INVAL;if(start<0||(uint64_t)start>b->len||count<0||(uint64_t)count>b->len-(size_t)start)return error(e,TS_ERR_INVAL,"slice: invalid range");return extract(b,(size_t)start,(size_t)count,r,e);}
static TS_Status contents(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){buffer_t *b;(void)u;if(ts_std_argc(v,n,1,1,e,"contents")!=TS_OK)return TS_ERR_INVAL;b=unwrap(a,e,"contents");if(!b)return TS_ERR_INVAL;return extract(b,0,b->len,r,e);}
static TS_Status line(TS_VM *v,void *u,const TS_Value *a,size_t n,TS_Value *r,TS_Error *e){buffer_t *b;int64_t which;size_t start=0,end,i,k=0;char *tmp;(void)u;if(ts_std_argc(v,n,2,2,e,"line")!=TS_OK)return TS_ERR_INVAL;b=unwrap(a,e,"line");if(!b||ts_std_int(a+1,&which,e,"line")!=TS_OK)return TS_ERR_INVAL;if(which<0){ts_std_ret_nil(r);return TS_OK;}if(b->kind==INDEXED_GAP){if((uint64_t)which>=b->nindex){ts_std_ret_nil(r);return TS_OK;}start=b->index[which];end=(size_t)which+1<b->nindex?b->index[which+1]-1:b->len;return extract(b,start,end-start,r,e);}
  if(b->kind==ARRAY_LINES){if((uint64_t)which>=b->nlines){ts_std_ret_nil(r);return TS_OK;}start=0;for(i=0;i<(size_t)which;i++)start+=b->line_lengths[i];end=start+b->line_lengths[which];if(end>start&&b->lines[which][end-start-1]=='\n')end--;return extract(b,start,end-start,r,e);}
  tmp=malloc(b->len+1);if(!tmp)return error(e,TS_ERR_NOMEM,"out of memory");copy_bytes(b,tmp);end=b->len;
  for(i=0;i<b->len;i++)if(tmp[i]=='\n'){if(k==(size_t)which){end=i;break;}k++;start=i+1;}
  if(k!=(size_t)which){free(tmp);ts_std_ret_nil(r);return TS_OK;}
  TS_Status st=ts_std_ret_strn(r,tmp+start,end-start,e);free(tmp);return st;
}
static const TS_FuncDef funcs[]={{"new",create,NULL},{"map_file",map_file,NULL},{"insert",edit,NULL},{"replace",edit,(void *)1},{"delete",edit,(void *)2},{"len",length,NULL},{"line",line,NULL},{"slice",slice,NULL},{"contents",contents,NULL},{NULL,NULL,NULL}};
const TS_Module ts_std_buffer_module={"std.buffer",funcs};
