// S3C2410/S5PV210 watchdog: stopped at probe (WTCON = 0), as a boot loader
// may leave it running. No /dev/watchdog yet.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"

static int
wdt_probe(struct device *dev)
{
  volatile uint32 *wtcon = (uint32*)(uintptr)dev->resource[0].start;
  *wtcon = 0;   // timer and reset output off
  printf("watchdog: s3c2410-wdt at %x stopped\n", (uint)dev->resource[0].start);
  return 0;
}

static struct device_driver wdt_driver = { .name = "s3c2410-wdt", .bus = &platform_bus, .probe = wdt_probe };
static void wdt_init(void) { platform_driver_register(&wdt_driver); }
core_initcall("s3c2410-wdt", wdt_init);
