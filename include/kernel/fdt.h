#ifndef XV6_FDT_H
#define XV6_FDT_H

// Minimal flattened device tree (DTB) reader: just enough to find RAM.

#include <kernel/types.h>

#define FDT_MAGIC 0xd00dfeed

struct fdt_scan_ops {
  void (*memory)(uint64 base, uint64 size);       // /memory reg entries
  void (*reserve)(uint64 base, uint64 size);      // /memreserve/, /reserved-memory
  char *model;                                    // root "model" (may be 0)
  int   model_len;
};

uint32 fdt_totalsize(const void *blob);           // 0 if not a valid DTB
int    fdt_scan(const void *blob, struct fdt_scan_ops *ops);

#endif
