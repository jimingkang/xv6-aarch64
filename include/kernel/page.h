#ifndef XV6_PAGE_H
#define XV6_PAGE_H

// Physical page descriptors and the buddy allocator (Linux mm/page_alloc.c,
// reduced to one node and one zone).
//
//   mem_map[pfn]          one struct page per 4 KiB physical frame, placed
//                         by memblock_alloc() at the top of RAM
//   free_area[order]      list of free blocks of 2^order pages, each block
//                         naturally aligned (its first pfn is a multiple of
//                         2^order), linked through page->lru of its first page
//
// Allocation splits a larger block down to the requested order and puts the
// upper halves back on the lower lists; freeing merges a block with its
// "buddy" (pfn ^ (1 << order)) as long as the buddy is also free and whole.
// Order-0 pages are cached per CPU (Linux per_cpu_pages): kalloc()/kfree()
// usually touch only the local list and take zone.lock once per batch.
// kalloc()/kfree() remain the one-page interface used across the kernel;
// kfree() also accepts kmalloc() memory (see slab.h).

#include <kernel/types.h>

#define MAX_ORDER 11            // free_area[0..10]: 4 KiB .. 4 MiB blocks

struct list_head {
  struct list_head *next, *prev;
};

static inline void
list_init(struct list_head *h)
{
  h->next = h->prev = h;
}

static inline int
list_empty(struct list_head *h)
{
  return h->next == h;
}

static inline void
list_add(struct list_head *n, struct list_head *h)
{
  n->next = h->next;
  n->prev = h;
  h->next->prev = n;
  h->next = n;
}

static inline void
list_del(struct list_head *n)
{
  n->prev->next = n->next;
  n->next->prev = n->prev;
  n->next = n->prev = 0;
}

struct kmem_cache;

// 40 bytes per 4 KiB frame (Linux: 64).  Like Linux, the fields after
// flags mean different things depending on who owns the page.
struct page {
  struct list_head lru;         // PG_buddy: free_area[order] link
                                // PG_pcp:   per-CPU free list link
                                // PG_slab:  kmem_cache partial-list link
  uint8  flags;                 // PG_*
  uint8  order;                 // block order while PG_buddy or PG_head
  uint16 inuse;                 // PG_slab: objects handed out from this slab
  int    refcount;              // 0 free, 1 after alloc; get_page()/put_page()
  struct kmem_cache *slab_cache;// PG_slab: owning cache
  void  *freelist;              // PG_slab: first free object in this slab
};

#define PG_reserved 0x01        // never given to the buddy allocator
#define PG_buddy    0x02        // first page of a free block in free_area[]
#define PG_head     0x04        // first page of an allocated order > 0 block
#define PG_slab     0x08        // owned by a kmem_cache (kfree() routes there)
#define PG_pcp      0x10        // free, parked on a per-CPU list

struct free_area {
  struct list_head free_list;
  uint64 nr_free;               // number of free blocks of this order
};

extern struct page *mem_map;
extern uint64 max_pfn;          // one past the last pfn described by mem_map

struct page *pfn_to_page(uint64 pfn);
uint64       page_to_pfn(struct page *pg);
struct page *virt_to_page(void *kva);
void        *page_address(struct page *pg);

struct page *alloc_pages(int order);
void         __free_pages(struct page *pg, int order);
void        *__get_free_pages(int order);       // kernel VA, or 0
void         free_pages(void *kva, int order);

void         drain_all_pages(void);            // per-CPU lists -> buddy

void         get_page(struct page *pg);
void         put_page(struct page *pg);         // frees an order-0 page at 0
int          page_count(struct page *pg);

// Fill buf with a /proc/buddyinfo style line; returns bytes written.
int          buddyinfo(char *buf, int n);

#endif
