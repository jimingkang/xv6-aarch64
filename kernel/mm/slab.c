// Small fixed-size kernel object allocator.
//
// The physical page allocator deliberately has a simple one-page API.  Using
// it directly for a 40-byte VMA or URB wastes nearly a whole page per object,
// so kmem_cache layers an intrusive fixed-size allocator over kalloc().

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/slab.h>

static uint
align_up(uint value, uint alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

int
kmem_cache_init(struct kmem_cache *cache, const char *name, uint size,
                uint alignment)
{
  if(cache == 0 || name == 0 || size == 0)
    return -1;
  if(alignment == 0)
    alignment = sizeof(uint64);
  if((alignment & (alignment - 1)) != 0 || alignment > PGSIZE)
    return -1;
  if(size < sizeof(void *))
    size = sizeof(void *);
  if(size > PGSIZE)
    return -1;

  memset(cache, 0, sizeof(*cache));
  cache->name = name;
  cache->object_size = size;
  cache->alignment = alignment;
  cache->stride = align_up(size, alignment);
  if(cache->stride == 0 || cache->stride > PGSIZE)
    return -1;
  initlock(&cache->lock, (char *)name);
  return 0;
}

void *
kmem_cache_alloc(struct kmem_cache *cache)
{
  void *object;

  if(cache == 0 || cache->stride == 0)
    panic("kmem_cache_alloc");

  acquire(&cache->lock);
  if(cache->freelist == 0){
    char *page = kalloc();
    if(page == 0){
      release(&cache->lock);
      return 0;
    }
    // PGSIZE is a multiple of every accepted alignment.  Link in reverse
    // order so the first allocation is also naturally aligned.
    for(uint offset = 0; offset + cache->stride <= PGSIZE;
        offset += cache->stride){
      void *entry = page + offset;
      *(void **)entry = cache->freelist;
      cache->freelist = entry;
      cache->total++;
    }
    cache->pages++;
  }
  object = cache->freelist;
  cache->freelist = *(void **)object;
  cache->inuse++;
  release(&cache->lock);

  memset(object, 0, cache->object_size);
  return object;
}

void
kmem_cache_free(struct kmem_cache *cache, void *object)
{
  if(cache == 0 || object == 0 || cache->stride == 0)
    panic("kmem_cache_free");

  // Poison the object, preserving the first word for the free-list link.
  memset(object, 1, cache->object_size);
  acquire(&cache->lock);
  if(cache->inuse == 0)
    panic("kmem_cache_free: underflow");
  *(void **)object = cache->freelist;
  cache->freelist = object;
  cache->inuse--;
  release(&cache->lock);
}
