#ifndef TS_STD_BUFFER_ALLOCATOR_H
#define TS_STD_BUFFER_ALLOCATOR_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Install before creating buffers. Keep context alive until every buffer
 * created with it is released. Pass NULL to restore libc allocation.
 * The allocator must return max_align_t-aligned storage. */
typedef struct {
  void *context;
  void *(*allocate)(void *context, size_t bytes);
  void (*release)(void *context, void *pointer, size_t bytes);
} TS_BufferAllocator;
void ts_std_buffer_set_allocator(const TS_BufferAllocator *allocator);
#ifdef __cplusplus
}
#endif
#endif
