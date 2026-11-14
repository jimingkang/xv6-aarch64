// S5PV210 HSMMC SD card driver (Samsung "sdhci-s3c") for the card the board
// booted from: platform glue, locking and the sdsector() block interface the
// FAT32/xv6fs layers use. The SD protocol itself is sdcore.c, shared with
// BL1, which reads the kernel through the same code.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "sdhci-s3c.h"
#include "sdcore.h"

static uintptr base;
static struct sdhci_s3c_platdata *pdata;
static int ready;
static struct spinlock sd_lock;
#define transfer sdc_transfer

static uint64 k_now(void) { return r_cntvct_el0(); }
static void k_log(const char *m, uint32 v) { printf("%s %x\n", m, v); }
static struct sdc_env kenv = { k_now, 0, k_log, 0 };
static int card_init(void)
{
  kenv.ticks_per_us = r_cntfrq_el0() / 1000000;
  if(kenv.ticks_per_us == 0) kenv.ticks_per_us = 1;
  return sdc_init(base, &kenv);
}

int
sdsector(uint32 lba, void *buf, int write)
{
  static uint32 bounce[128];
  if(!ready)
    return -1;
  acquire(&sd_lock);
  uint32 *p = ((uintptr)buf & 3) ? bounce : buf;
  if(write && p == bounce)
    memmove(bounce, buf, 512);
  int r = -1;
  for(int attempt = 0; attempt < 3 && r < 0; attempt++)
    r = transfer(lba, p, write);
  if(r == 0 && !write && p == bounce)
    memmove(buf, bounce, 512);
  release(&sd_lock);
  return r;
}

int sd_ready(void) { return ready; }

static int
sdhci_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  pdata = dev->driver_data;
  initlock(&sd_lock, "sd");
  if(pdata && pdata->setup)
    pdata->setup();
  printf("sd: hsmmc%d at %x\n", dev->id, (uint)base);
  if(card_init() < 0){
    printf("sd: no usable card\n");
    return 0;     // not fatal: the kernel falls back to its built-in RAM disk
  }
  ready = 1;
  return 0;
}

static struct device_driver sdhci_driver = { .name = "s3c-sdhci", .bus = &platform_bus, .probe = sdhci_probe };
static void sdhci_register(void) { platform_driver_register(&sdhci_driver); }
module_init_ord("sdhci-s3c", sdhci_register, 0);
