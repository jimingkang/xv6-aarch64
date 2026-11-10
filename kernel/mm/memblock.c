// memblock: the boot-time physical memory map (Linux mm/memblock.c, reduced).
//
// Two sorted, non-overlapping region arrays: memory (what RAM exists) and
// reserved (what must not be handed out).  A range is free if it is in
// memory and not in reserved.  memblock_alloc() carves pages out of free
// ranges top-down and reserves them; memblock_free_all() gives every free
// range to the buddy allocator once mem_map exists.
//
// It runs before printf works and before any lock is needed (CPU0 alone),
// so it reports problems through boot_uart_mark() and is printed later by
// memblock_dump().

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/memblock.h>
#include <kernel/fdt.h>

void boot_uart_mark(int c);
extern char end[];

struct memblock memblock = {
  .current_limit = ~0ULL,
  .memory   = { .name = "memory" },
  .reserved = { .name = "reserved" },
};
char *memblock_source = "none";
char memblock_model[64];
static uint64 dtb_pa_used, dtb_size_used;

static void
memblock_halt(char *msg)
{
  for(; *msg; msg++)
    boot_uart_mark(*msg);
  for(;;)
    asm volatile("wfe");
}

static void
remove_at(struct memblock_type *t, int i)
{
  t->total -= t->regions[i].size;
  for(; i + 1 < t->cnt; i++)
    t->regions[i] = t->regions[i + 1];
  t->cnt--;
}

// Insert [base, base+size) and merge it with every region it overlaps or
// touches, keeping the array sorted.
static void
type_add(struct memblock_type *t, uint64 base, uint64 size)
{
  uint64 end = base + size;
  int i;

  if(size == 0)
    return;
  if(end < base)
    end = ~0ULL;
  // Absorb overlapping/adjacent regions into [base, end).
  for(i = 0; i < t->cnt; ){
    struct memblock_region *r = &t->regions[i];
    uint64 rend = r->base + r->size;
    if(rend < base || r->base > end){
      i++;
      continue;
    }
    if(r->base < base)
      base = r->base;
    if(rend > end)
      end = rend;
    remove_at(t, i);
  }
  if(t->cnt >= MEMBLOCK_MAX_REGIONS)
    memblock_halt("\nmemblock: too many regions\n");
  for(i = t->cnt; i > 0 && t->regions[i - 1].base > base; i--)
    t->regions[i] = t->regions[i - 1];
  t->regions[i].base = base;
  t->regions[i].size = end - base;
  t->cnt++;
  t->total += end - base;
}

// Cut [base, base+size) out of every region of t.
static void
type_remove(struct memblock_type *t, uint64 base, uint64 size)
{
  uint64 end = base + size;

  if(end < base)
    end = ~0ULL;
  for(int i = 0; i < t->cnt; ){
    struct memblock_region *r = &t->regions[i];
    uint64 rbase = r->base, rend = r->base + r->size;
    if(rend <= base || rbase >= end){
      i++;
      continue;
    }
    remove_at(t, i);
    if(rbase < base)
      type_add(t, rbase, base - rbase);
    if(rend > end)
      type_add(t, end, rend - end);
    i = 0;                      // the array changed under us; rescan
  }
}

void
memblock_add(uint64 base, uint64 size)
{
  type_add(&memblock.memory, base, size);
}

void
memblock_remove(uint64 base, uint64 size)
{
  type_remove(&memblock.memory, base, size);
}

void
memblock_reserve(uint64 base, uint64 size)
{
  type_add(&memblock.reserved, base, size);
}

void
memblock_set_current_limit(uint64 limit)
{
  memblock.current_limit = limit;
}

uint64
memblock_start_of_DRAM(void)
{
  return memblock.memory.cnt ? memblock.memory.regions[0].base : 0;
}

uint64
memblock_end_of_DRAM(void)
{
  struct memblock_type *t = &memblock.memory;
  return t->cnt ? t->regions[t->cnt - 1].base + t->regions[t->cnt - 1].size : 0;
}

int
memblock_is_memory(uint64 pa)
{
  return memblock_overlaps_memory(pa, 1);
}

int
memblock_overlaps_memory(uint64 base, uint64 size)
{
  struct memblock_type *t = &memblock.memory;
  for(int i = 0; i < t->cnt; i++)
    if(base < t->regions[i].base + t->regions[i].size &&
       base + size > t->regions[i].base)
      return 1;
  return 0;
}

// The first reserved region overlapping [base, end), or 0.
static struct memblock_region *
reserved_overlap(uint64 base, uint64 end)
{
  struct memblock_type *t = &memblock.reserved;
  for(int i = 0; i < t->cnt; i++)
    if(base < t->regions[i].base + t->regions[i].size &&
       end > t->regions[i].base)
      return &t->regions[i];
  return 0;
}

// Find and reserve size bytes, aligned, below current_limit, preferring the
// highest free address (Linux's default top-down policy keeps low memory,
// which DMA engines may need, for later).
uint64
memblock_alloc(uint64 size, uint64 align)
{
  struct memblock_type *t = &memblock.memory;

  if(size == 0 || align == 0 || (align & (align - 1)) != 0)
    return 0;
  for(int i = t->cnt - 1; i >= 0; i--){
    uint64 rbase = t->regions[i].base;
    uint64 top = rbase + t->regions[i].size;
    if(top > memblock.current_limit)
      top = memblock.current_limit;
    while(top > rbase && top - rbase >= size){
      uint64 cand = (top - size) & ~(align - 1);
      struct memblock_region *r;
      if(cand < rbase)
        break;
      if((r = reserved_overlap(cand, cand + size)) == 0){
        memblock_reserve(cand, size);
        return cand;
      }
      top = r->base;            // retry below the obstacle
    }
  }
  return 0;
}

// Call give(start, end) for every free range: memory minus reserved.
void
memblock_free_all(void (*give)(uint64 start, uint64 end))
{
  struct memblock_type *m = &memblock.memory, *r = &memblock.reserved;

  for(int i = 0; i < m->cnt; i++){
    uint64 start = m->regions[i].base;
    uint64 stop = start + m->regions[i].size;
    for(int j = 0; j < r->cnt && start < stop; j++){
      uint64 rb = r->regions[j].base, re = rb + r->regions[j].size;
      if(re <= start || rb >= stop)
        continue;
      if(rb > start)
        give(start, rb);
      start = re;
    }
    if(start < stop)
      give(start, stop);
  }
}

static void
dtb_memory(uint64 base, uint64 size)
{
  memblock_add(base, size);
}

static void
dtb_reserve(uint64 base, uint64 size)
{
  memblock_reserve(base, size);
}

// Build the memory map: RAM from the DTB (or the built-in layout), then the
// ranges the kernel must keep for itself.  Only the bootstrap map is live, so
// the DTB must lie in it: entry.S maps its 2 MiB block(s) for this purpose.
void
memblock_init(uint64 dtb_pa)
{
  void *dtb = 0;
  uint32 size = 0;

  if(dtb_pa != 0 && dtb_pa < LOCAL_BASE_PA)
    dtb = P2V(dtb_pa);
  if(dtb && (size = fdt_totalsize(dtb)) != 0){
    struct fdt_scan_ops ops = { dtb_memory, dtb_reserve,
                                memblock_model, sizeof(memblock_model) };
    if(fdt_scan(dtb, &ops) == 0 && memblock.memory.cnt > 0){
      memblock_source = "DTB";
      dtb_pa_used = dtb_pa;
      dtb_size_used = size;
      memblock_reserve(dtb_pa, size);
    } else {
      // A malformed tree: forget whatever it added.
      memblock.memory.cnt = memblock.reserved.cnt = 0;
      memblock.memory.total = memblock.reserved.total = 0;
    }
  }
  if(memblock.memory.cnt == 0){
    memblock_source = "built-in";
    memblock_add(0, PHYSTOP);
  }

  // The linear map lives in the first 1 GiB of KERNBASE and the BCM2837
  // peripherals start at 0x3f000000: nothing at or above them is RAM we use.
  memblock_remove(PERIPHERAL_BASE_PA, ~0ULL - PERIPHERAL_BASE_PA);

  // Page 0 (spin-table slots, armstub), the kernel image and its BSS.
  memblock_reserve(0, PGROUNDUP(V2P(end)));
  // Fixed carve-outs: ramdisk, camera DMA buffer, module arena.  Their 2 MiB
  // window is mapped with its own attributes by kvmmake().
  memblock_reserve(CARVEOUT_START, CARVEOUT_END - CARVEOUT_START);

  // kvminit() builds the final page table with pages from here, and only
  // the bootstrap map [0, EARLYTOP) is mapped yet.
  memblock_set_current_limit(EARLYTOP);
}

static void
dump_type(struct memblock_type *t)
{
  printf("  %s.cnt = %d, total = %d KiB\n", t->name, t->cnt, (int)(t->total >> 10));
  for(int i = 0; i < t->cnt; i++)
    printf("    [%d] %p-%p %d KiB\n", i, t->regions[i].base,
           t->regions[i].base + t->regions[i].size - 1,
           (int)(t->regions[i].size >> 10));
}

void
memblock_dump(void)
{
  printf("memblock: memory map from %s", memblock_source);
  if(dtb_pa_used)
    printf(" at pa %p (%d bytes)", dtb_pa_used, (int)dtb_size_used);
  if(memblock_model[0])
    printf(", model \"%s\"", memblock_model);
  printf("\n");
  dump_type(&memblock.memory);
  dump_type(&memblock.reserved);
}
