#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <sys/types.h>
#include <uapi/mman.h>

void *mmap(void *, size_t, int, int, int, off_t);
int munmap(void *, size_t);
int msync(void *, size_t, int);

#endif
