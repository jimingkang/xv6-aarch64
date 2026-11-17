#ifndef XV6_MEMSTAT_H
#define XV6_MEMSTAT_H
// A physical range shown in /proc/iomem (end inclusive, as in Linux).
struct iomem_region {
  uint64 start, end;
  char *name;
};
// Board hook: RAM banks and reserved carve-outs; returns the count.
int arch_iomem_regions(struct iomem_region *r, int max);
#endif
