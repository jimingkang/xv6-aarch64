// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "aarch64.h"
#include "defs.h"

void freerange(void *vstart, void *vend);
void boot_uart_mark(int c);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 nfree;          // pages on freelist
  uint64 managed;        // pages ever handed to the allocator (MemTotal)
  uint64 lo, hi;         // physical range those pages came from
} kmem;

// Junk-filling pages catches use-after-free; it is skipped while the boot
// code hands the initial (never used) memory to the allocator.
static int kfree_nofill;

void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  boot_uart_mark('a');
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)vstart);
  int first = 1;
  kfree_nofill = 1;
  if(p + PGSIZE <= (char*)vend){
    uint64 lo = V2P(p), hi = V2P(PGROUNDDOWN((uint64)vend));
    if(kmem.managed == 0 || lo < kmem.lo) kmem.lo = lo;
    if(hi > kmem.hi) kmem.hi = hi;
    kmem.managed += (hi - lo) / PGSIZE;
  }
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE){
    if(first)
      boot_uart_mark('b');
    kfree(p);
    if(first){
      boot_uart_mark('c');
      first = 0;
    }
  }
  kfree_nofill = 0;
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *va)
{
  struct run *r;

  if(((uint64)va % PGSIZE) != 0 || (char*)va < end || (uint64)va >= (uint64)P2V(PHYSTOP))
    panic("kfree");

  // Fill with junk to catch dangling refs.
  if(kmem.freelist == 0)
    boot_uart_mark('d');
  if(!kfree_nofill)
    memset(va, 1, PGSIZE);
  if(kmem.freelist == 0)
    boot_uart_mark('e');

  r = (struct run*)va;

  acquire(&kmem.lock);
  if(kmem.freelist == 0)
    boot_uart_mark('f');
  r->next = kmem.freelist;
  kmem.freelist = r;
  kmem.nfree++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r){
    kmem.freelist = r->next;
    kmem.nfree--;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

uint64
kfreepages(void)
{
  uint64 pages;
  acquire(&kmem.lock);
  pages = kmem.nfree;
  release(&kmem.lock);
  return pages;
}

// Page-allocator statistics for /proc/meminfo and /proc/iomem: pages
// managed, pages free, and the physical range [lo, hi) they came from.
void
kmemstat(uint64 *managed, uint64 *nfree, uint64 *lo, uint64 *hi)
{
  acquire(&kmem.lock);
  *managed = kmem.managed;
  *nfree = kmem.nfree;
  *lo = kmem.lo;
  *hi = kmem.hi;
  release(&kmem.lock);
}
