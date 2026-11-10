// Physical page allocator: struct page array (mem_map) plus a buddy system.
//
// This follows the Linux boot path in the memory-init diagram:
//
//   memblock_init()  RAM from the DTB /memory node, reserved ranges
//   kvminit()        final page table; kalloc() takes its pages from
//                    memblock_alloc() inside the bootstrap map
//   page_alloc_init()       mem_map from memblock_alloc(), then memblock_free_all()
//                    hands every free range to the buddy allocator
//
// Every 4 KiB frame below the end of RAM has a struct page.  Frames that are
// never handed to the allocator (kernel image, DTB, page tables, mem_map,
// carve-outs, holes between memory regions) stay PG_reserved.
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
#include <kernel/memblock.h>
#include <kernel/slab.h>

void boot_uart_mark(int c);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct page *mem_map;
uint64 max_pfn;
static int buddy_ready;         // page_alloc_init() done: kalloc() uses free_area[]

static struct {
  struct spinlock lock;
  struct free_area free_area[MAX_ORDER];
  uint64 nr_free;               // pages on free_area[] (NR_FREE_PAGES)
  uint64 managed_pages;         // pages ever handed to the buddy allocator
} zone;

// Per-CPU cache of free order-0 pages (Linux struct per_cpu_pages).  Most
// kalloc()/kfree() calls only touch the local list; zone.lock is taken once
// per PCP_BATCH pages.  Each list has its own lock, as in Linux >= 5.19,
// so drain_all_pages() can empty other CPUs' lists without IPIs; the lock
// is uncontended except during a drain.  Lock order: pcp.lock, zone.lock.
#define PCP_HIGH  64            // above this many pages, give a batch back
#define PCP_BATCH 16            // pages moved per zone.lock round trip

struct per_cpu_pages {
  struct spinlock lock;
  int count;
  struct list_head list;
} __attribute__((aligned(64)));

static struct per_cpu_pages pcp[NCPU];

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

#define lru_to_page(l) ((struct page *)(l))   // lru is the first member

static void
add_to_free_area(struct page *pg, int order)
{
  pg->flags |= PG_buddy;
  pg->order = order;
  list_add(&pg->lru, &zone.free_area[order].free_list);
  zone.free_area[order].nr_free++;
  zone.nr_free += 1UL << order;
}

static void
del_from_free_area(struct page *pg, int order)
{
  list_del(&pg->lru);
  pg->flags &= ~PG_buddy;
  zone.free_area[order].nr_free--;
  zone.nr_free -= 1UL << order;
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

// Take a 2^order block from the smallest non-empty free_area at or above
// order, splitting off and returning the unused upper halves (Linux
// __rmqueue_smallest + expand).  Caller holds zone.lock.
static struct page *
__rmqueue(int order)
{
  for(int o = order; o < MAX_ORDER; o++){
    struct page *pg;
    if(list_empty(&zone.free_area[o].free_list))
      continue;
    pg = lru_to_page(zone.free_area[o].free_list.next);
    del_from_free_area(pg, o);
    while(o > order){
      o--;
      add_to_free_area(pg + (1UL << o), o);
    }
    return pg;
  }
  return 0;
}

static struct per_cpu_pages *
this_pcp_lock(void)
{
  struct per_cpu_pages *p;

  push_off();                   // cpuid() is stable only with IRQs off
  p = &pcp[cpuid()];
  acquire(&p->lock);
  pop_off();
  return p;
}

// Give count pages from the tail (coldest end) of p back to the buddy
// allocator (Linux free_pcppages_bulk).  Caller holds p->lock.
static void
free_pcppages_bulk(struct per_cpu_pages *p, int count)
{
  acquire(&zone.lock);
  while(count-- > 0 && !list_empty(&p->list)){
    struct page *pg = lru_to_page(p->list.prev);
    list_del(&pg->lru);
    pg->flags &= ~PG_pcp;
    p->count--;
    __free_one_page(page_to_pfn(pg), 0);
  }
  release(&zone.lock);
}

// One order-0 page, from this CPU's list; refill it with a batch from the
// buddy allocator when it runs dry (Linux rmqueue_pcplist).
static struct page *
rmqueue_pcplist(void)
{
  struct per_cpu_pages *p = this_pcp_lock();
  struct page *pg = 0;

  if(list_empty(&p->list)){
    acquire(&zone.lock);
    for(int i = 0; i < PCP_BATCH; i++){
      struct page *n = __rmqueue(0);
      if(n == 0)
        break;
      n->flags |= PG_pcp;
      list_add(&n->lru, p->list.prev);  // keep the batch in address order
      p->count++;
    }
    release(&zone.lock);
  }
  if(!list_empty(&p->list)){
    pg = lru_to_page(p->list.next);     // hottest page first
    list_del(&pg->lru);
    pg->flags &= ~PG_pcp;
    p->count--;
  }
  release(&p->lock);
  return pg;
}

// Return every per-CPU page to the buddy allocator, e.g. before giving up
// on an allocation that other CPUs' lists could satisfy.
void
drain_all_pages(void)
{
  for(int c = 0; c < NCPU; c++){
    acquire(&pcp[c].lock);
    free_pcppages_bulk(&pcp[c], pcp[c].count);
    release(&pcp[c].lock);
  }
}

static struct page *
rmqueue(int order)
{
  struct page *pg;

  if(order == 0 && (pg = rmqueue_pcplist()) != 0)
    return pg;
  acquire(&zone.lock);
  pg = __rmqueue(order);
  release(&zone.lock);
  return pg;
}

// Allocate 2^order physically contiguous pages (Linux alloc_pages).
struct page *
alloc_pages(int order)
{
  struct page *pg;

  if(order < 0 || order >= MAX_ORDER)
    return 0;
  if((pg = rmqueue(order)) == 0){
    // Free pages may be parked on other CPUs' lists, and pages there
    // cannot merge into larger blocks: drain and try once more.
    drain_all_pages();
    if((pg = rmqueue(order)) == 0)
      return 0;
  }
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
  if(pg->flags & (PG_buddy | PG_pcp)){
    printf("%s: double free of pa %p\n", who, pfn << PGSHIFT);
    panic(who);
  }
  if(pg->flags & PG_slab){
    printf("%s: pa %p is a slab page\n", who, pfn << PGSHIFT);
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

  if(order == 0){
    // Linux free_unref_page(): park it on this CPU's list.
    struct per_cpu_pages *p = this_pcp_lock();
    pg->flags |= PG_pcp;
    list_add(&pg->lru, &p->list);
    p->count++;
    if(p->count >= PCP_HIGH)
      free_pcppages_bulk(p, PCP_BATCH);
    release(&p->lock);
    return;
  }
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
  if(!buddy_ready){
    // Before page_alloc_init(): the final kernel page table is built from pages
    // memblock carves out of the bootstrap-mapped RAM.  They are reserved
    // for good (kernel page tables are never freed).
    uint64 pa = memblock_alloc(PGSIZE, PGSIZE);
    if(pa == 0)
      return 0;
    memset(P2V(pa), 5, PGSIZE);
    return P2V(pa);
  }
  return __get_free_pages(0);
}

// Free memory from kalloc() or kmalloc().  Like Linux kfree(), the page
// descriptor says who owns it: a slab object goes back to its kmem_cache,
// the head of a large kmalloc() block frees the whole block, and anything
// else must be one page from kalloc().
void
kfree(void *va)
{
  struct page *pg;

  if(!buddy_ready || (char*)va < end || V2P(va) >= (max_pfn << PGSHIFT))
    panic("kfree");
  pg = virt_to_page(va);
  if(pg->flags & PG_slab){
    kmem_cache_free(pg->slab_cache, va);
    return;
  }
  if(((uint64)va % PGSIZE) != 0)
    panic("kfree");
  if(pg->flags & PG_head){
    __free_pages(pg, pg->order);
    return;
  }
  free_pages(va, 0);
}

// ---------------------------------------------------------------------------
// boot-time setup (memblock_free_all equivalent)

// Hand physical [start, end) to the buddy allocator in the largest
// naturally aligned blocks that fit, instead of one page at a time.
// Called by memblock_free_all() for every range that is memory and not
// reserved.
static void
free_range(uint64 start, uint64 end)
{
  uint64 pfn = PGROUNDUP(start) >> PGSHIFT;
  uint64 end_pfn = end >> PGSHIFT;

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

// Linux mem_init(): with the final page table mapping all of RAM, place
// mem_map with memblock_alloc(), mark every frame reserved, then give each
// free memblock range to the buddy allocator.  From here on kalloc() uses
// free_area[] and memblock is no longer consulted.
void
page_alloc_init(void)
{
  uint64 size, pa;

  initlock(&zone.lock, "kmem");
  for(int c = 0; c < NCPU; c++){
    initlock(&pcp[c].lock, "pcp");
    list_init(&pcp[c].list);
    pcp[c].count = 0;
  }
  for(int o = 0; o < MAX_ORDER; o++){
    list_init(&zone.free_area[o].free_list);
    zone.free_area[o].nr_free = 0;
  }
  zone.managed_pages = 0;
  zone.nr_free = 0;

  max_pfn = memblock_end_of_DRAM() >> PGSHIFT;
  size = max_pfn * sizeof(struct page);
  memblock_set_current_limit(max_pfn << PGSHIFT);
  if((pa = memblock_alloc(PGROUNDUP(size), PGSIZE)) == 0)
    boot_halt("\npage_alloc_init: no memory for mem_map\n");
  mem_map = (struct page *)P2V(pa);

  boot_uart_mark('a');
  memset(mem_map, 0, size);
  for(uint64 pfn = 0; pfn < max_pfn; pfn++)
    mem_map[pfn].flags = PG_reserved;

  boot_uart_mark('b');
  memblock_free_all(free_range);
  buddy_ready = 1;
  boot_uart_mark('c');
}

// Number of free 4 KiB pages: the buddy lists plus the per-CPU lists.
// Counters, not a walk of the lists; a racy snapshot is fine for MemFree.
uint64
kfreepages(void)
{
  uint64 pages = *(volatile uint64 *)&zone.nr_free;
  for(int c = 0; c < NCPU; c++)
    pages += *(volatile int *)&pcp[c].count;
  return pages;
}

// Pages currently parked on per-CPU lists (for /proc/meminfo).
uint64
kpcppages(void)
{
  uint64 pages = 0;
  for(int c = 0; c < NCPU; c++)
    pages += *(volatile int *)&pcp[c].count;
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

  drain_all_pages();
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
  drain_all_pages();            // b went to this CPU's list
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
