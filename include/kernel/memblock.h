#ifndef XV6_MEMBLOCK_H
#define XV6_MEMBLOCK_H

// Boot-time physical memory map (Linux mm/memblock.c, reduced).
//
//   memblock.memory     RAM that exists, from the device tree /memory node
//                       (or a built-in default when no DTB was passed)
//   memblock.reserved   ranges that must never reach the page allocator:
//                       the kernel image, the DTB itself, /memreserve/ and
//                       /reserved-memory entries, fixed carve-outs, and
//                       everything memblock_alloc() has handed out
//
// Before the buddy allocator exists, kalloc() takes pages from
// memblock_alloc(): the final kernel page table and mem_map are built that
// way.  page_alloc_init() then gives every byte of memory that is not reserved to
// the buddy allocator (memblock_free_all) and memblock retires.

#include <kernel/types.h>

#define MEMBLOCK_MAX_REGIONS 32

struct memblock_region {
  uint64 base;
  uint64 size;
};

struct memblock_type {
  int cnt;
  uint64 total;                 // sum of region sizes
  char *name;
  struct memblock_region regions[MEMBLOCK_MAX_REGIONS];
};

struct memblock {
  uint64 current_limit;         // memblock_alloc() stays below this PA
  struct memblock_type memory;
  struct memblock_type reserved;
};

extern struct memblock memblock;
extern char *memblock_source;   // "DTB" or "built-in"
extern char memblock_model[64]; // DTB root "model", if any

void   memblock_add(uint64 base, uint64 size);
void   memblock_remove(uint64 base, uint64 size);
void   memblock_reserve(uint64 base, uint64 size);
uint64 memblock_alloc(uint64 size, uint64 align);   // PA, 0 on failure
void   memblock_set_current_limit(uint64 limit);
uint64 memblock_start_of_DRAM(void);
uint64 memblock_end_of_DRAM(void);
int    memblock_is_memory(uint64 pa);
int    memblock_overlaps_memory(uint64 base, uint64 size);
void   memblock_free_all(void (*give)(uint64 start, uint64 end));
void   memblock_init(uint64 dtb_pa);
void   memblock_dump(void);

#endif
