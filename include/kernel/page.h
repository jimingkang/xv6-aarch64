#ifndef XV6_PAGE_H
#define XV6_PAGE_H

// Physical page descriptors and the buddy allocator (Linux mm/page_alloc.c,
// reduced to one node and one zone).
//
//   mem_map[pfn]          one struct page per 4 KiB physical frame, carved
//                         out of RAM right after the kernel image by kinit1()
//                         the way memblock_alloc() places Linux's mem_map
//   free_area[order]      list of free blocks of 2^order pages, each block
//                         naturally aligned (its first pfn is a multiple of
//                         2^order), linked through page->lru of its first page
//
// Allocation splits a larger block down to the requested order and puts the
// upper halves back on the lower lists; freeing merges a block with its
// "buddy" (pfn ^ (1 << order)) as long as the buddy is also free and whole.
// kalloc()/kfree() remain the one-page interface used across the kernel.

#include <kernel/types.h>

#define MAX_ORDER 11            // free_area[0..10]: 4 KiB .. 4 MiB blocks

struct list_head {
  struct list_head *next, *prev;
};

struct page {
  struct list_head lru;         // free_area[order] link while PG_buddy
  uint16 flags;                 // PG_*
  uint8  order;                 // block order while PG_buddy or PG_head
  uint8  _pad;
  int    refcount;              // 0 free, 1 after alloc; get_page()/put_page()
};

#define PG_reserved 0x0001      // never given to the buddy allocator
#define PG_buddy    0x0002      // first page of a free block in free_area[]
#define PG_head     0x0004      // first page of an allocated order > 0 block

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

void         get_page(struct page *pg);
void         put_page(struct page *pg);         // frees an order-0 page at 0
int          page_count(struct page *pg);

// Fill buf with a /proc/buddyinfo style line; returns bytes written.
int          buddyinfo(char *buf, int n);

#endif
