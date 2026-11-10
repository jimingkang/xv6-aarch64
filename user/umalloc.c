#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <uapi/limits.h>
#include <uapi/vm.h>
#include <sys/mman.h>

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
    uint kind;
    uint64 maplen;
    uint64 reserved;          // keep Header a multiple of 16 bytes
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;
static volatile int heap_lock;

// Follow ptmalloc's high-level policy without importing its arena/bin
// implementation: reuse an existing heap chunk first, grow the brk arena for
// small misses, and use a private anonymous mapping for large misses.  glibc's
// initial mmap threshold is 128 KiB and it raises the threshold after freeing
// a larger mmapped chunk; use the same bounded dynamic policy here.
#define MALLOC_HEAP_KIND       0x48454150U  // "HEAP"
#define MALLOC_MMAP_KIND       0x4d4d4150U  // "MMAP"
#define DEFAULT_MMAP_THRESHOLD (128U * 1024U)
#define MAX_MMAP_THRESHOLD     (32U * 1024U * 1024U)

static uint mmap_threshold = DEFAULT_MMAP_THRESHOLD;

static void
heap_acquire(void)
{
  for(;;){
    uint old, failed;
    uint one = 1;
    asm volatile("ldaxr %w0, [%1]"
                 : "=&r"(old) : "r"(&heap_lock) : "memory");
    if(old == 0){
      asm volatile("stxr %w0, %w1, [%2]"
                   : "=&r"(failed) : "r"(one), "r"(&heap_lock)
                   : "memory");
      if(failed == 0)
        return;
    } else {
      asm volatile("clrex" ::: "memory");
    }
    sched_yield();
  }
}

static void
heap_release(void)
{
  uint zero = 0;
  asm volatile("stlr %w0, [%1]" :: "r"(zero), "r"(&heap_lock) : "memory");
}

// The allocator lock is already held.  morecore() must use this helper rather
// than the public free(), otherwise extending the heap would lock recursively.
static void
free_locked(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  bp->s.kind = MALLOC_HEAP_KIND;
  bp->s.maplen = 0;
  bp->s.reserved = 0;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

void
free(void *ap)
{
  Header *hp;
  uint64 maplen;

  if(ap == 0)
    return;

  hp = (Header*)ap - 1;
  if(hp->s.kind == MALLOC_MMAP_KIND){
    maplen = hp->s.maplen;

    // As in ptmalloc's dynamic threshold mode, a successfully released
    // direct mapping teaches the allocator that similarly sized transient
    // allocations should be allowed to use the reusable brk arena next time.
    heap_acquire();
    if(maplen > mmap_threshold && maplen <= MAX_MMAP_THRESHOLD)
      mmap_threshold = maplen;
    heap_release();

    // munmap needs the mapping base, not the address returned to the caller.
    munmap(hp, maplen);
    return;
  }

  heap_acquire();
  free_locked(ap);
  heap_release();
}

static Header*
morecore(uint nu)
{
  char *p;
  Header *hp;
  uint64 bytes;

  if(nu < 4096)
    nu = 4096;
  bytes = (uint64)nu * sizeof(Header);
  // sbrk's historical xv6 ABI takes a signed int.
  if(bytes > 0x7fffffffU)
    return 0;
  p = sbrk((int)bytes);
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  hp->s.kind = MALLOC_HEAP_KIND;
  hp->s.maplen = 0;
  hp->s.reserved = 0;
  free_locked((void*)(hp + 1));
  return freep;
}

static void*
mmap_alloc(uint nbytes)
{
  Header *hp;
  uint64 need, maplen;

  need = (uint64)nbytes + sizeof(Header);
  if(need < nbytes || need > (uint64)-1 - (PGSIZE - 1))
    return 0;
  maplen = (need + PGSIZE - 1) & ~((uint64)PGSIZE - 1);

  hp = mmap(0, maplen, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(hp == MAP_FAILED)
    return 0;

  hp->s.ptr = 0;
  hp->s.size = nbytes;       // requested payload bytes, for realloc()
  hp->s.kind = MALLOC_MMAP_KIND;
  hp->s.maplen = maplen;
  hp->s.reserved = 0;
  return (void*)(hp + 1);
}

void*
malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;
  uint64 units;
  int tried_mmap = 0;
  void *mapped;

  units = ((uint64)nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;
  if(units > (uint)-1)
    return 0;
  nunits = units;

  heap_acquire();

retry:
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
    base.s.kind = MALLOC_HEAP_KIND;
    base.s.maplen = 0;
    base.s.reserved = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      p->s.kind = MALLOC_HEAP_KIND;
      p->s.maplen = 0;
      p->s.reserved = 0;
      freep = prevp;
      heap_release();
      return (void*)(p + 1);
    }
    if(p == freep){
      // Existing heap space has priority.  Only a miss at or above the
      // threshold becomes an independent mmap, and mmap failure falls back
      // to the original brk arena rather than failing malloc prematurely.
      if(!tried_mmap && nbytes >= mmap_threshold){
        tried_mmap = 1;
        heap_release();
        mapped = mmap_alloc(nbytes);
        if(mapped)
          return mapped;
        heap_acquire();
        goto retry;
      }
      if((p = morecore(nunits)) == 0){
        heap_release();
        return 0;
      }
    }
  }
}

void*
calloc(uint count, uint size)
{
  uint total;
  void *memory;
  if(size != 0 && count > (uint)-1 / size)
    return 0;
  total = count * size;
  memory = malloc(total);
  // A fresh anonymous mapping is already zero-filled.  Avoid touching every
  // page here so a large calloc retains mmap's demand-paging advantage.
  if(memory && ((Header*)memory - 1)->s.kind != MALLOC_MMAP_KIND)
    memset(memory, 0, total);
  return memory;
}

void*
realloc(void *old, uint size)
{
  Header *header;
  uint oldsize;
  void *memory;
  if(old == 0)
    return malloc(size);
  if(size == 0){
    free(old);
    return 0;
  }
  header = (Header*)old - 1;
  if(header->s.kind == MALLOC_MMAP_KIND)
    oldsize = header->s.size;
  else
    oldsize = (header->s.size - 1) * sizeof(Header);
  if(oldsize >= size)
    return old;
  memory = malloc(size);
  if(memory == 0)
    return 0;
  memmove(memory, old, oldsize);
  free(old);
  return memory;
}
