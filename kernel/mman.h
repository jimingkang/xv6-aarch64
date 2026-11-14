#ifndef XV6_MMAN_H
#define XV6_MMAN_H
// mmap()/munmap() (Linux <sys/mman.h> subset), shared by kernel and user.
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_FAILED  ((void *)-1)

#endif
