// reboot / halt via the BCM2835 power-management (PM) watchdog.
//
// The Pi has no ACPI or PSCI firmware service for reset; Linux's
// bcm2835_wdt driver does the same thing: arm the watchdog with a short
// timeout and select "full reset" in RSTC.  When the timer expires the
// whole SoC resets and the GPU firmware boots kernel8.img again.
//
// For halt, the firmware is told (via the partition number in RSTS) to boot
// partition 63, which the Raspberry Pi firmware treats as "stay halted".
// QEMU raspi3b emulates the PM block (bcm2835_powermgt), so reboot works
// there too.

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include "memlayout.h"
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <uapi/reboot.h>

#define PM_BASE                 (PERIPHERAL_BASE + 0x100000L)
#define PM_RSTC                 (PM_BASE + 0x1c)
#define PM_RSTS                 (PM_BASE + 0x20)
#define PM_WDOG                 (PM_BASE + 0x24)

#define PM_PASSWORD             0x5a000000
#define PM_RSTC_WRCFG_CLR       0xffffffcf
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020
#define PM_RSTS_HALT            0x00000555   // partition 63, bits spread

static inline uint32
pm_read(uint64 a)
{
  return *(volatile uint32 *)a;
}

static inline void
pm_write(uint64 a, uint32 v)
{
  *(volatile uint32 *)a = v;
}

static void __attribute__((noreturn))
pm_restart(int halt)
{
  uint32 v;

  intr_off();
  if(halt){
    v = pm_read(PM_RSTS);
    pm_write(PM_RSTS, v | PM_PASSWORD | PM_RSTS_HALT);
  }
  // ~10 ticks of the 65.5 kHz watchdog clock: practically immediate.
  pm_write(PM_WDOG, PM_PASSWORD | 10);
  v = pm_read(PM_RSTC);
  pm_write(PM_RSTC, (v & PM_RSTC_WRCFG_CLR) | PM_PASSWORD | PM_RSTC_WRCFG_FULL_RESET);
  for(;;)
    asm volatile("wfi");
}

// int reboot(int cmd)
//   cmd = REBOOT_CMD_RESTART  restart the board
//   cmd = REBOOT_CMD_HALT     halt (firmware stays in partition 63)
uint64
sys_reboot(void)
{
  int cmd;

  argint(0, &cmd);
  if(cmd != REBOOT_CMD_RESTART && cmd != REBOOT_CMD_HALT)
    return -1;

  // File writes are synchronous (log / ext2 in place); sdflush is the
  // card-level barrier that makes sure the last sectors are programmed.
  sdflush();

  printf("\nreboot: %s (pid %d)\n",
         cmd == REBOOT_CMD_HALT ? "system halted" : "restarting system",
         myproc()->pid);
  pm_restart(cmd == REBOOT_CMD_HALT);
}
