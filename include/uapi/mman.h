#ifndef _UAPI_MMAN_H
#define _UAPI_MMAN_H

/* Same values as Linux include/uapi/asm-generic/mman-common.h. */
#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define PROT_EXEC      0x4

#define MAP_SHARED     0x01    /* stores reach the file / other mappers */
#define MAP_PRIVATE    0x02    /* private copy of the file's pages */
#define MAP_FIXED      0x10    /* exactly at addr, replacing old mappings */
#define MAP_ANONYMOUS  0x20    /* zero-filled memory, fd ignored */
#define MAP_ANON       MAP_ANONYMOUS
#define MAP_POPULATE   0x8000  /* fault every page in at mmap() time */
#define MAP_FAILED     ((void *)-1)

#define MS_ASYNC       1
#define MS_INVALIDATE  2
#define MS_SYNC        4

#endif
