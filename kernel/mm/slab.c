// Slab allocator: kmem_cache and kmalloc() (Linux mm/slub.c, much reduced).
//
// See slab.h for the design.  Every slab is an order-0 page from the buddy
// allocator whose struct page carries the slab's metadata, so objects can
// be freed by address alone and empty slabs can be given back.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/page.h>
#include <kernel/slab.h>

static struct spinlock cache_list_lock;
static struct kmem_cache *cache_list;
static int cache_list_ready;

static struct kmem_cache kmalloc_caches[KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1];
static const char *kmalloc_names[] = {
  "kmalloc-32", "kmalloc-64", "kmalloc-128", "kmalloc-256",
  "kmalloc-512", "kmalloc-1024", "kmalloc-2048",
};

#define lru_to_page(l) ((struct page *)(l))   // lru is the first member

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
  cache->objs_per_slab = PGSIZE / cache->stride;
  list_init(&cache->partial);
  initlock(&cache->lock, (char *)name);

  if(!cache_list_ready){
    initlock(&cache_list_lock, "slab_caches");
    cache_list_ready = 1;
  }
  acquire(&cache_list_lock);
  cache->next = cache_list;
  cache_list = cache;
  release(&cache_list_lock);
  return 0;
}

// A fresh slab: one page, every object on the page's own freelist.
// Linked in reverse so the first object handed out is at offset 0.
static struct page *
new_slab(struct kmem_cache *cache)
{
  struct page *pg = alloc_pages(0);
  char *base;

  if(pg == 0)
    return 0;
  base = page_address(pg);
  pg->flags |= PG_slab;
  pg->slab_cache = cache;
  pg->freelist = 0;
  pg->inuse = 0;
  for(uint i = 0; i < cache->objs_per_slab; i++){
    void *obj = base + (cache->objs_per_slab - 1 - i) * cache->stride;
    *(void **)obj = pg->freelist;
    pg->freelist = obj;
  }
  return pg;
}

static void
discard_slab(struct page *pg)
{
  pg->flags &= ~PG_slab;
  pg->slab_cache = 0;
  pg->freelist = 0;
  pg->inuse = 0;
  __free_pages(pg, 0);
}

void *
kmem_cache_alloc(struct kmem_cache *cache)
{
  struct page *pg;
  void *object;

  if(cache == 0 || cache->stride == 0)
    panic("kmem_cache_alloc");

  acquire(&cache->lock);
  if(list_empty(&cache->partial)){
    // Allocate outside the cache lock: the page allocator may drain the
    // per-CPU lists, and slab users may hold other locks.
    release(&cache->lock);
    if((pg = new_slab(cache)) == 0)
      return 0;
    acquire(&cache->lock);
    list_add(&pg->lru, &cache->partial);
    cache->nr_partial++;
    cache->pages++;
    cache->total += cache->objs_per_slab;
  }
  pg = lru_to_page(cache->partial.next);
  object = pg->freelist;
  pg->freelist = *(void **)object;
  pg->inuse++;
  if(pg->freelist == 0){        // now full: off the partial list
    list_del(&pg->lru);
    cache->nr_partial--;
  }
  cache->inuse++;
  release(&cache->lock);

  memset(object, 0, cache->object_size);
  return object;
}

void
kmem_cache_free(struct kmem_cache *cache, void *object)
{
  struct page *pg;
  uint offset;

  if(cache == 0 || object == 0 || cache->stride == 0)
    panic("kmem_cache_free");
  pg = virt_to_page(object);
  offset = (uint64)object & (PGSIZE - 1);
  if((pg->flags & PG_slab) == 0 || pg->slab_cache != cache ||
     offset % cache->stride != 0 || offset / cache->stride >= cache->objs_per_slab){
    printf("kmem_cache_free: %p is not an object of %s\n", object, cache->name);
    panic("kmem_cache_free");
  }

  // Poison the object, preserving the first word for the free-list link.
  memset(object, 1, cache->object_size);
  acquire(&cache->lock);
  if(pg->inuse == 0 || cache->inuse == 0)
    panic("kmem_cache_free: underflow");
  if(pg->freelist == 0){        // was full: back on the partial list
    list_add(&pg->lru, &cache->partial);
    cache->nr_partial++;
  }
  *(void **)object = pg->freelist;
  pg->freelist = object;
  pg->inuse--;
  cache->inuse--;
  if(pg->inuse == 0 && cache->nr_partial > 1){
    // Empty, and the cache has other room: give the page back.
    list_del(&pg->lru);
    cache->nr_partial--;
    cache->pages--;
    cache->total -= cache->objs_per_slab;
    release(&cache->lock);
    discard_slab(pg);
    return;
  }
  release(&cache->lock);
}

void
kmalloc_init(void)
{
  for(int i = 0; i <= KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT; i++)
    if(kmem_cache_init(&kmalloc_caches[i], kmalloc_names[i],
                       1U << (KMALLOC_MIN_SHIFT + i), 1U << (KMALLOC_MIN_SHIFT + i)) < 0)
      panic("kmalloc_init");
}

void *
kmalloc(uint64 size)
{
  int shift = KMALLOC_MIN_SHIFT, order = 0;
  void *p;

  if(size == 0)
    return 0;
  if(size > (1UL << KMALLOC_MAX_SHIFT)){
    // Linux kmalloc_large(): whole pages, freed by kfree() via PG_head.
    while((PGSIZE << order) < size)
      order++;
    if(order >= MAX_ORDER || (p = __get_free_pages(order)) == 0)
      return 0;
    memset(p, 0, PGSIZE << order);
    return p;
  }
  while((1UL << shift) < size)
    shift++;
  return kmem_cache_alloc(&kmalloc_caches[shift - KMALLOC_MIN_SHIFT]);
}

static char *
put(char *p, char *limit, const char *s, int width)
{
  int n = strlen(s);
  for(; *s && p < limit; s++)
    *p++ = *s;
  while(n++ < width && p < limit)
    *p++ = ' ';
  return p;
}

static char *
putu(char *p, char *limit, uint64 v, int width)
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

// /proc/slabinfo, in the column order of Linux's slabinfo 2.1.
int
slabinfo(char *buf, int n)
{
  char *p = buf, *limit = buf + n;

  p = put(p, limit, "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab> <num_slabs>\n", 0);
  acquire(&cache_list_lock);
  for(struct kmem_cache *c = cache_list; c && p < limit - 100; c = c->next){
    uint inuse, total, pages;
    acquire(&c->lock);
    inuse = c->inuse; total = c->total; pages = c->pages;
    release(&c->lock);
    p = put(p, limit, c->name, 18);
    p = putu(p, limit, inuse, 13);
    p = putu(p, limit, total, 11);
    p = putu(p, limit, c->object_size, 10);
    p = putu(p, limit, c->objs_per_slab, 13);
    p = putu(p, limit, 1, 15);
    p = putu(p, limit, pages, 12);
    if(p < limit)
      *p++ = '\n';
  }
  release(&cache_list_lock);
  return p - buf;
}

// Boot self-test: size classes, alignment, zeroing, kfree() routing, and
// that freed slabs go back to the buddy allocator.
void
slab_selftest(void)
{
  struct kmem_cache *c64 = &kmalloc_caches[6 - KMALLOC_MIN_SHIFT];
  uint pages0 = c64->pages, pages32 = kmalloc_caches[0].pages;
  uint64 free0;
  void **v, *small, *big;
  int n = 300;

  drain_all_pages();
  free0 = kfreepages();
  if((v = kmalloc(n * sizeof(void *))) == 0)   // > 2048: a whole page
    panic("slab_selftest: kmalloc large");
  if((virt_to_page(v)->flags & PG_slab) != 0 || ((uint64)v & (PGSIZE - 1)) != 0)
    panic("slab_selftest: large kmalloc came from a slab");
  for(int i = 0; i < n; i++){
    char *p = kmalloc(40);                     // kmalloc-64
    struct page *pg;
    if(p == 0 || ((uint64)p & 63) != 0)
      panic("slab_selftest: kmalloc-64 alignment");
    pg = virt_to_page(p);
    if((pg->flags & PG_slab) == 0 || pg->slab_cache != c64)
      panic("slab_selftest: slab page metadata");
    for(int j = 0; j < 64; j++)
      if(p[j] != 0)
        panic("slab_selftest: not zeroed");
    memset(p, 0xa5, 64);
    v[i] = p;
  }
  for(int i = 0; i < n; i++)
    for(int j = i + 1; j < i + 3 && j < n; j++)
      if(v[i] == v[j])
        panic("slab_selftest: object handed out twice");
  if(c64->pages < (n + 63) / 64)
    panic("slab_selftest: page count");
  small = kmalloc(1);                          // kmalloc-32
  big = kmalloc(10000);                        // order-2 block
  if(small == 0 || big == 0 || (virt_to_page(big)->flags & PG_head) == 0)
    panic("slab_selftest: kmalloc sizes");
  kfree(big);
  kfree(small);
  for(int i = 0; i < n; i++)
    kfree(v[i]);
  kfree(v);
  if(c64->inuse != 0 || c64->pages > (pages0 > 1 ? pages0 : 1))
    panic("slab_selftest: empty slabs were not released");
  drain_all_pages();
  // Every page went back except slabs the two caches chose to keep.
  if(kfreepages() + ((int)c64->pages - (int)pages0) +
     ((int)kmalloc_caches[0].pages - (int)pages32) != free0)
    panic("slab_selftest: pages leaked");
  printf("slab: %d kmalloc caches (32..2048 B), self-test ok, %d page(s) kept by kmalloc-64\n",
         KMALLOC_MAX_SHIFT - KMALLOC_MIN_SHIFT + 1, (int)c64->pages);
}
