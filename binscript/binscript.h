#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct BS_VM BS_VM;
typedef struct { int code; size_t offset; char message[256]; } BS_Error;
BS_VM *bs_vm_create(BS_Error*); void bs_vm_free(BS_VM*);
int bs_vm_run(BS_VM*, const char*, char**, BS_Error*);
const char *bs_vm_compile_to_c(BS_VM*, const char*, const char*, BS_Error*);
#ifdef __cplusplus
}
#endif
