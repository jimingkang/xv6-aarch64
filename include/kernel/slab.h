#ifndef XV6_SLAB_H
#define XV6_SLAB_H

#include <kernel/types.h>
#include <kernel/spinlock.h>
#include <kernel/page.h>

// Fixed-size kernel object caches and kmalloc(), modelled after Linux SLUB.
//
// A slab is one page from the buddy allocator, cut into equal objects.  Its
// bookkeeping lives in the page's struct page, as in SLUB: PG_slab marks it,
// slab_cache points back at the cache, freelist chains the free objects
// (through their first word) and inuse counts the allocated ones.  So the
// whole page is objects, and kfree(ptr) finds the cache from the address.
//
// A cache keeps the slabs that still have free objects on its partial list.
// Full slabs are on no list.  When a slab's last object is freed the page
// goes back to the buddy allocator, unless it is the cache's only partial
// slab (one empty slab is kept so an alloc/free pair does not bounce a page
// in and out of the buddy allocator).
struct kmem_cache {
  struct spinlock lock;
  const char *name;
  uint object_size;
  uint stride;                  // object_size rounded up to alignment
  uint alignment;
  uint objs_per_slab;
  struct list_head partial;     // slabs with at least one free object
  uint nr_partial;
  uint pages;                   // slabs currently owned
  uint total;                   // objects in those slabs
  uint inuse;                   // objects handed out
  struct kmem_cache *next;      // all caches, for /proc/slabinfo
};

int   kmem_cache_init(struct kmem_cache *cache, const char *name,
                      uint size, uint alignment);
void *kmem_cache_alloc(struct kmem_cache *cache);
void  kmem_cache_free(struct kmem_cache *cache, void *object);

// kmalloc(): power-of-two size classes kmalloc-32 .. kmalloc-2048.  Larger
// requests get whole pages straight from the buddy allocator.  Memory is
// zeroed.  Free it with kfree().
#define KMALLOC_MIN_SHIFT 5
#define KMALLOC_MAX_SHIFT 11
void *kmalloc(uint64 size);
void  kmalloc_init(void);

// Fill buf with /proc/slabinfo; returns bytes written.
int   slabinfo(char *buf, int n);
void  slab_selftest(void);

#endif
