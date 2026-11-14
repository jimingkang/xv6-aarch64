#ifndef XV6_MMAP_H
#define XV6_MMAP_H
// Kernel side of mmap(): the vma a driver's file_operations.mmap fills in.
#include "kernel/mman.h"
// Device mappings live in a fixed window above the heap/stack (user memory
// is [0, sz) with sz < 32 MiB) and below MAXVA. On ARM32 they are 1 MiB
// sections, so both ends and the physical frames are 1 MiB aligned.
#define MMAP_BASE 0x10000000UL
#define MMAP_END  0x18000000UL
#define MMAP_ALIGN 0x100000UL

struct vm_area_struct {
  uint64 vm_start;        // user address chosen by the core
  uint64 vm_end;
  uint64 vm_pgoff;        // offset argument / PGSIZE
  int vm_prot;            // PROT_READ | PROT_WRITE
  pagetable_t pagetable;  // the process being mapped
};

// Map [pa, pa+size) at va for the vma's process (driver mmap hook).
int remap_pfn_range(struct vm_area_struct *vma, uint64 va, uint64 pa, uint64 size, int prot);
uint64 mmap_find_area(pagetable_t p, uint64 len);
int munmap_area(pagetable_t p, uint64 va, uint64 len);
#endif
