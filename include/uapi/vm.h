#ifndef _UAPI_VM_H
#define _UAPI_VM_H

#include "types.h"

/* Values observable by VM conformance tests and user stack allocators. */
#define PGSIZE 4096
#define KERNBASE 0xffffff8000000000ULL
#define XV6_USER_MAXVA 0xffffffc000000000ULL
#define MAXVA XV6_USER_MAXVA

static inline uint64
r_sp(void)
{
  uint64 value;
  __asm__ volatile("mov %0, sp" : "=r" (value));
  return value;
}

#endif
