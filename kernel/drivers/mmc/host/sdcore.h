#ifndef XV6_SDCORE_H
#define XV6_SDCORE_H
// Environment the SD protocol code needs from its user (BL1 or kernel).
struct sdc_env {
  uint64 (*now)(void);                     // free-running counter
  uint32 ticks_per_us;                     // its rate (>= 1)
  void (*log)(const char *msg, uint32 v);  // diagnostics, may be 0
};
int sdc_init(uintptr regs, const struct sdc_env *env);   // 0: card ready
int sdc_transfer(uint32 lba, uint32 *buf, int write);    // one 512-byte block, word-aligned buf
int sdc_is_sdhc(void);
#endif
