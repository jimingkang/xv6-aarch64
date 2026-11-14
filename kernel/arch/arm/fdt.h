struct fdt_info {
  int valid;
  uint32 size;
  uint64 mem_base, mem_size;
  char model[64];
  char compatible[96];
  char bootargs[128];
};
int fdt_parse(uintptr addr, struct fdt_info *info);
