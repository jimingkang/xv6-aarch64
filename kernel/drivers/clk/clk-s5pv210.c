// S5PV210 clock controller (0xE0100000): read-only view of the PLLs and
// bus dividers that BL1/U-Boot programmed, for clk_get_rate().
//   pclk_psys  - PWM timers, UARTs (66.7 MHz with MPLL 667 MHz)
//   hclk_dsys  - FIMD (166.75 MHz)
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/clk.h"

#define FIN 24000000U
static uintptr base;
#define REG(o) (*(volatile uint32*)(base + (o)))
#define APLL_CON0 0x100
#define MPLL_CON  0x108
#define CLK_SRC0  0x200
#define CLK_DIV0  0x300

static uint apll(void) {
  uint v = REG(APLL_CON0), m = (v>>16)&0x3ff, p = (v>>8)&0x3f, s = v&7;
  if(!(REG(CLK_SRC0)&1)) return FIN;          // APLL_SEL = FINPLL
  return (p && s) ? (uint)((uint64)FIN*m/(p<<(s-1))) : 0;
}
static uint mpll(void) {
  uint v = REG(MPLL_CON), m = (v>>16)&0x3ff, p = (v>>8)&0x3f, s = v&7;
  if(!(REG(CLK_SRC0)&(1U<<4))) return FIN;    // MPLL_SEL = FINPLL
  return p ? (uint)((uint64)FIN*m/(p<<s)) : 0;
}
static uint ratio(int shift, uint mask) { return ((REG(CLK_DIV0)>>shift)&mask) + 1; }

uint
clk_get_rate(const char *name)
{
  if(!base)
    return 0;
  if(strncmp(name, "pclk_psys", 10) == 0){
    uint mout = (REG(CLK_SRC0)&(1U<<24)) ? apll()/ratio(4, 7) : mpll();
    return mout/ratio(24, 15)/ratio(28, 7);
  }
  if(strncmp(name, "hclk_dsys", 10) == 0){
    uint mout = (REG(CLK_SRC0)&(1U<<20)) ? apll()/ratio(4, 7) : mpll();
    return mout/ratio(16, 15);
  }
  return 0;
}

static int
clk_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  printf("clk: APLL %d MPLL %d pclk_psys %d hclk_dsys %d Hz\n",
         apll(), mpll(), clk_get_rate("pclk_psys"), clk_get_rate("hclk_dsys"));
  return 0;
}

static struct device_driver clk_driver = { .name = "s5pv210-clock", .bus = &platform_bus, .probe = clk_probe };
static void clk_init(void) { platform_driver_register(&clk_driver); }
core_initcall("clk-s5pv210", clk_init);
