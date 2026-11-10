#ifndef XV6_SLAB_H
#define XV6_SLAB_H

#include <kernel/types.h>
#include <kernel/spinlock.h>

// Fixed-size kernel object cache, modelled after Linux kmem_cache.
//
// kalloc() remains the page allocator.  A cache obtains backing pages from
// kalloc(), divides them into aligned objects, and keeps freed objects on an
// intrusive free list (the first machine word is used only while free).
// Empty slab pages are intentionally retained by the cache; this is the
// small xv6 equivalent of a Linux slab cache without a shrinker.
struct kmem_cache {
  struct spinlock lock;
  const char *name;
  void *freelist;
  uint object_size;
  uint stride;
  uint alignment;
  uint pages;
  uint total;
  uint inuse;
};

int   kmem_cache_init(struct kmem_cache *cache, const char *name,
                      uint size, uint alignment);
void *kmem_cache_alloc(struct kmem_cache *cache);
void  kmem_cache_free(struct kmem_cache *cache, void *object);

#endif
