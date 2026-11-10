// Physical page allocator: struct page array (mem_map) plus a buddy system.
//
// This follows the end of the Linux boot path in the memory-init diagram:
//
//   kinit1()   memblock_alloc(mem_map) + first memblock_free_all() pass
//              (only RAM covered by entry.S's bootstrap map, [end, EARLYTOP))
//   kvminit()  page tables are allocated from that first pass
//   kinit2()   second pass, [EARLYTOP, PHYSTOP), once kvmmake has mapped it
//
// Every 4 KiB frame below PHYSTOP has a struct page.  Frames that are never
// handed to the allocator (kernel image, mem_map itself) stay PG_reserved.
// Free memory lives in free_area[order] as naturally aligned 2^order blocks.
//
// kalloc()/kfree() keep their old one-page contract and are now order-0
// wrappers; alloc_pages(order) returns 2^order physically contiguous pages.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include <kernel/spinlock.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/page.h>

void boot_uart_mark(int c);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct page *mem_map;
uint64 max_pfn;

static struct {
  struct spinlock lock;
  struct free_area free_area[MAX_ORDER];
  uint64 managed_pages;         // pages ever handed to the buddy allocator
} zone;

// ---------------------------------------------------------------------------
// struct page <-> address helpers

struct page *
pfn_to_page(uint64 pfn)
{
  return mem_map + pfn;
}

uint64
page_to_pfn(struct page *pg)
{
  return (uint64)(pg - mem_map);
}

struct page *
virt_to_page(void *kva)
{
  return pfn_to_page(V2P(kva) >> PGSHIFT);
}

void *
page_address(struct page *pg)
{
  return P2V(page_to_pfn(pg) << PGSHIFT);
}

// ---------------------------------------------------------------------------
// free lists

static void
list_init(struct list_head *h)
{
  h->next = h->prev = h;
}

static int
list_empty(struct list_head *h)
{
  return h->next == h;
}

static void
list_add(struct list_head *n, struct list_head *h)
{
  n->next = h->next;
  n->prev = h;
  h->next->prev = n;
  h->next = n;
}

static void
list_del(struct list_head *n)
{
  n->prev->next = n->next;
  n->next->prev = n->prev;
  n->next = n->prev = 0;
}

#define lru_to_page(l) ((struct page *)(l))   // lru is the first member

static void
add_to_free_area(struct page *pg, int order)
{
  pg->flags |= PG_buddy;
  pg->order = order;
  list_add(&pg->lru, &zone.free_area[order].free_list);
  zone.free_area[order].nr_free++;
}

static void
del_from_free_area(struct page *pg, int order)
{
  list_del(&pg->lru);
  pg->flags &= ~PG_buddy;
  zone.free_area[order].nr_free--;
}

// Put the free block [pfn, pfn + 2^order) back, merging it with its buddy
// for as long as the buddy is a whole free block of the same order
// (Linux __free_one_page).  Caller holds zone.lock.
static void
__free_one_page(uint64 pfn, int order)
{
  while(order < MAX_ORDER - 1){
    uint64 buddy_pfn = pfn ^ (1UL << order);
    struct page *buddy;

    if(buddy_pfn >= max_pfn)
      break;
    buddy = pfn_to_page(buddy_pfn);
    if((buddy->flags & PG_buddy) == 0 || buddy->order != order)
      break;
    del_from_free_area(buddy, order);
    pfn &= ~(1UL << order);       // merged block starts at the lower half
    order++;
  }
  add_to_free_area(pfn_to_page(pfn), order);
}

// ---------------------------------------------------------------------------
// allocation

// Allocate 2^order physically contiguous pages (Linux alloc_pages).  The
// block is taken from the smallest non-empty free_area at or above order;
// the unused upper halves are split off and returned to lower lists.
struct page *
alloc_pages(int order)
{
  struct page *pg = 0;
  int o;

  if(order < 0 || order >= MAX_ORDER)
    return 0;

  acquire(&zone.lock);
  for(o = order; o < MAX_ORDER; o++){
    if(list_empty(&zone.free_area[o].free_list))
      continue;
    pg = lru_to_page(zone.free_area[o].free_list.next);
    del_from_free_area(pg, o);
    while(o > order){             // expand(): give back the upper halves
      o--;
      add_to_free_area(pg + (1UL << o), o);
    }
    break;
  }
  release(&zone.lock);

  if(pg == 0)
    return 0;
  pg->refcount = 1;
  pg->order = order;
  if(order > 0)
    pg->flags |= PG_head;
  memset(page_address(pg), 5, PGSIZE << order); // fill with junk
  return pg;
}

static void
free_pages_check(struct page *pg, int order, char *who)
{
  uint64 pfn = page_to_pfn(pg);

  if(pg < mem_map || pfn >= max_pfn || (pfn & ((1UL << order) - 1)) != 0)
    panic(who);
  if(pg->flags & PG_reserved){
    printf("%s: pa %p is reserved\n", who, pfn << PGSHIFT);
    panic(who);
  }
  if(pg->flags & PG_buddy){
    printf("%s: double free of pa %p\n", who, pfn << PGSHIFT);
    panic(who);
  }
  if(order > 0 && ((pg->flags & PG_head) == 0 || pg->order != order)){
    printf("%s: pa %p freed with order %d, allocated with order %d\n",
           who, pfn << PGSHIFT, order, (pg->flags & PG_head) ? pg->order : 0);
    panic(who);
  }
  if(order == 0 && (pg->flags & PG_head)){
    printf("%s: pa %p is the head of an order-%d block\n",
           who, pfn << PGSHIFT, pg->order);
    panic(who);
  }
}

static void
free_pages_ok(struct page *pg, int order)
{
  pg->flags &= ~PG_head;
  pg->refcount = 0;
  // Fill with junk to catch dangling refs.
  memset(page_address(pg), 1, PGSIZE << order);

  acquire(&zone.lock);
  __free_one_page(page_to_pfn(pg), order);
  release(&zone.lock);
}

void
__free_pages(struct page *pg, int order)
{
  free_pages_check(pg, order, "__free_pages");
  if(pg->refcount > 1){
    printf("__free_pages: pa %p still has %d references\n",
           page_to_pfn(pg) << PGSHIFT, pg->refcount);
    panic("__free_pages");
  }
  free_pages_ok(pg, order);
}

void *
__get_free_pages(int order)
{
  struct page *pg = alloc_pages(order);
  return pg ? page_address(pg) : 0;
}

void
free_pages(void *kva, int order)
{
  if(kva == 0)
    return;
  if(((uint64)kva % PGSIZE) != 0 || (uint64)kva < KERNBASE ||
     (uint64)kva >= (uint64)P2V(max_pfn << PGSHIFT))
    panic("free_pages");
  __free_pages(virt_to_page(kva), order);
}

// ---------------------------------------------------------------------------
// reference counts (the hook copy-on-write fork will use)

// LL/SC add; the toolchain's outline-atomics helpers are not linked in.
static int
atomic_add_return(int *v, int delta)
{
  int result, fail;

  asm volatile(
    "1: ldaxr %w0, [%2]\n"
    "   add   %w0, %w0, %w3\n"
    "   stlxr %w1, %w0, [%2]\n"
    "   cbnz  %w1, 1b\n"
    : "=&r"(result), "=&r"(fail)
    : "r"(v), "r"(delta)
    : "memory");
  return result;
}

void
get_page(struct page *pg)
{
  if(atomic_add_return(&pg->refcount, 1) <= 1)
    panic("get_page: free page");
}

void
put_page(struct page *pg)
{
  int r = atomic_add_return(&pg->refcount, -1);
  int order = (pg->flags & PG_head) ? pg->order : 0;

  if(r < 0)
    panic("put_page: underflow");
  if(r == 0){
    free_pages_check(pg, order, "put_page");
    free_pages_ok(pg, order);
  }
}

int
page_count(struct page *pg)
{
  return *(volatile int *)&pg->refcount;
}

// ---------------------------------------------------------------------------
// one-page interface used throughout the kernel

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  return __get_free_pages(0);
}

// Free the page of physical memory pointed at by va,
// which normally should have been returned by a
// call to kalloc().
void
kfree(void *va)
{
  if(((uint64)va % PGSIZE) != 0 || (char*)va < end || (uint64)va >= (uint64)P2V(PHYSTOP))
    panic("kfree");
  free_pages(va, 0);
}

// ---------------------------------------------------------------------------
// boot-time setup (memblock_free_all equivalent)

// Hand [vstart, vend) to the buddy allocator in the largest naturally
// aligned blocks that fit, instead of one page at a time.
static void
free_range(void *vstart, void *vend)
{
  uint64 pfn = V2P(PGROUNDUP((uint64)vstart)) >> PGSHIFT;
  uint64 end_pfn = V2P(vend) >> PGSHIFT;

  if(end_pfn > max_pfn)
    end_pfn = max_pfn;
  while(pfn < end_pfn){
    int order = MAX_ORDER - 1;
    while(order > 0 &&
          ((pfn & ((1UL << order) - 1)) != 0 || pfn + (1UL << order) > end_pfn))
      order--;
    for(uint64 i = 0; i < (1UL << order); i++){
      struct page *pg = pfn_to_page(pfn + i);
      pg->flags &= ~PG_reserved;
      pg->refcount = 0;
    }
    acquire(&zone.lock);
    __free_one_page(pfn, order);
    zone.managed_pages += 1UL << order;
    release(&zone.lock);
    pfn += 1UL << order;
  }
}

static void
boot_halt(char *msg)
{
  for(; *msg; msg++)
    boot_uart_mark(*msg);
  for(;;)
    asm volatile("wfe");
}

// First pass, with only the bootstrap map live: place mem_map right after
// the kernel image, mark every frame reserved, then free what is left of
// [vstart, vend).
void
kinit1(void *vstart, void *vend)
{
  uint64 size;
  char *map_end;

  initlock(&zone.lock, "kmem");
  for(int o = 0; o < MAX_ORDER; o++){
    list_init(&zone.free_area[o].free_list);
    zone.free_area[o].nr_free = 0;
  }
  zone.managed_pages = 0;

  max_pfn = PHYSTOP >> PGSHIFT;
  size = max_pfn * sizeof(struct page);
  mem_map = (struct page *)PGROUNDUP((uint64)vstart);
  map_end = (char *)PGROUNDUP((uint64)mem_map + size);
  if((uint64)map_end > (uint64)vend)
    boot_halt("\nmem_map extends past EARLYTOP\n");

  boot_uart_mark('a');
  memset(mem_map, 0, size);
  for(uint64 pfn = 0; pfn < max_pfn; pfn++)
    mem_map[pfn].flags = PG_reserved;

  boot_uart_mark('b');
  free_range(map_end, vend);
  boot_uart_mark('c');
}

void
kinit2(void *vstart, void *vend)
{
  free_range(vstart, vend);
}

// Number of free 4 KiB pages.
uint64
kfreepages(void)
{
  uint64 pages = 0;
  acquire(&zone.lock);
  for(int o = 0; o < MAX_ORDER; o++)
    pages += zone.free_area[o].nr_free << o;
  release(&zone.lock);
  return pages;
}

uint64
kmanagedpages(void)
{
  return zone.managed_pages;
}

static char *
fmtnum(char *p, char *limit, uint64 v, int width)
{
  char tmp[24];
  int n = 0;

  do {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  } while(v && n < (int)sizeof(tmp));
  while(width-- > n && p < limit)
    *p++ = ' ';
  while(n > 0 && p < limit)
    *p++ = tmp[--n];
  return p;
}

// /proc/buddyinfo: free block count for each order, 4 KiB .. 4 MiB.
int
buddyinfo(char *buf, int n)
{
  char *p = buf, *limit = buf + n;
  uint64 nr[MAX_ORDER];
  char *hdr = "Node 0, zone   Normal ";

  acquire(&zone.lock);
  for(int o = 0; o < MAX_ORDER; o++)
    nr[o] = zone.free_area[o].nr_free;
  release(&zone.lock);

  for(; *hdr && p < limit; hdr++)
    *p++ = *hdr;
  for(int o = 0; o < MAX_ORDER; o++)
    p = fmtnum(p, limit, nr[o], 7);
  if(p < limit)
    *p++ = '\n';
  return p - buf;
}

// Boot self-test: split, alignment, contiguity and merge-back.
void
buddy_selftest(void)
{
  uint64 before = kfreepages();
  uint64 nr[MAX_ORDER];
  struct page *a, *b, *c;
  int o;

  acquire(&zone.lock);
  for(o = 0; o < MAX_ORDER; o++)
    nr[o] = zone.free_area[o].nr_free;
  release(&zone.lock);
  a = alloc_pages(3);
  b = alloc_pages(0);
  c = alloc_pages(MAX_ORDER - 1);
  if(a == 0 || b == 0 || c == 0)
    panic("buddy_selftest: alloc");
  if((page_to_pfn(a) & 7) != 0 || (page_to_pfn(c) & ((1UL << (MAX_ORDER - 1)) - 1)) != 0)
    panic("buddy_selftest: alignment");
  if(kfreepages() != before - 8 - 1 - (1UL << (MAX_ORDER - 1)))
    panic("buddy_selftest: accounting");
  get_page(b);
  put_page(b);                  // 2 -> 1, still allocated
  if(page_count(b) != 1)
    panic("buddy_selftest: refcount");
  put_page(b);                  // 1 -> 0, freed
  __free_pages(a, 3);
  free_pages(page_address(c), MAX_ORDER - 1);
  if(kfreepages() != before)
    panic("buddy_selftest: leak");
  // Every split must have merged back: the free lists look exactly as before.
  acquire(&zone.lock);
  for(o = 0; o < MAX_ORDER; o++)
    if(zone.free_area[o].nr_free != nr[o])
      break;
  release(&zone.lock);
  if(o != MAX_ORDER)
    panic("buddy_selftest: blocks did not merge back");
  printf("buddy: %d pages managed, %d free, mem_map %d KiB at pa %p\n",
         (int)zone.managed_pages, (int)before,
         (int)((max_pfn * sizeof(struct page)) >> 10), V2P(mem_map));
}
