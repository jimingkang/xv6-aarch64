#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.

typedef long Align;

union header {
  struct {
    union header *ptr;
    uint size;
  } s;
  Align x;
};

typedef union header Header;

static Header base;
static Header *freep;
static volatile int heap_lock;

static void
heap_acquire(void)
{
#ifdef XV6_ARM32
  while(__sync_lock_test_and_set(&heap_lock, 1))
    sched_yield();
#else
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
#endif
}

static void
heap_release(void)
{
#ifdef XV6_ARM32
  __sync_lock_release(&heap_lock);
#else
  uint zero = 0;
  asm volatile("stlr %w0, [%1]" :: "r"(zero), "r"(&heap_lock) : "memory");
#endif
}

// The allocator lock is already held.  morecore() must use this helper rather
// than the public free(), otherwise extending the heap would lock recursively.
static void
free_locked(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
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
  if(ap == 0)
    return;
  heap_acquire();
  free_locked(ap);
  heap_release();
}

static Header*
morecore(uint nu)
{
  char *p;
  Header *hp;

  if(nu < 4096)
    nu = 4096;
  p = sbrk(nu * sizeof(Header));
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  free_locked((void*)(hp + 1));
  return freep;
}

void*
malloc(uint nbytes)
{
  Header *p, *prevp;
  uint nunits;

  heap_acquire();
  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
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
      freep = prevp;
      heap_release();
      return (void*)(p + 1);
    }
    if(p == freep)
      if((p = morecore(nunits)) == 0){
        heap_release();
        return 0;
      }
  }
}
