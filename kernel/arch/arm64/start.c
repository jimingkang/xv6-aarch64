#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/defs.h>

void _entry();
void main();
extern char end[];

// entry.S needs one stack per CPU.
__attribute__ ((aligned (16))) char stack0[4096 * NCPU];

// entry.S jumps here in supervisor mode (EL1) on stack0.
// in qemu-system-aarch64, default EL (Exeception Level) is 1.
void
start()
{
  main();
}

__attribute__((aligned(PGSIZE))) pte_t l1entrypgt[512];
__attribute__((aligned(PGSIZE))) pte_t l2entrypgt[512];
__attribute__((aligned(PGSIZE))) pte_t l1kpgt[512];
__attribute__((aligned(PGSIZE))) pte_t l2kpgt[512];

  // Phase 1: identity-map the physical blocks containing the kernel.  The
  // Pi firmware loads kernel8.img at 0x80000, inside the first aligned 2 MiB
  // block, so this mapping starts at physical/virtual address zero.
  // Phase 2: map the same physical blocks at the high kernel virtual base.
    // Keep Mini UART reachable while the bootstrap TTBR1 table is active.
  // PA 0x3f215000 lies in the 2 MiB block beginning at 0x3f200000.
void entrypgtinit(pte_t *l1entry, pte_t *l2entry,pte_t *l1kernel, pte_t *l2kernel,uint64 pa_end,
                  uint64 dtb_pa)
{
  const uint64 l2block = 1UL << PXSHIFT(2);
  for(uint64 pa = 0; pa < pa_end; pa += l2block) {
    l2entry[PX(2, pa)] = PA2PTE(pa) | PTE_AF | PTE_NORMAL | PTE_VALID;
  }
  l1entry[PX(1, 0)] = PA2PTE((uint64)l2entry) |PTE_TABLE | PTE_VALID;
  
  for(uint64 pa = 0; pa < pa_end; pa += l2block) {
    uint64 va = KERNBASE + pa;
    l2kernel[PX(2, va)] = PA2PTE(pa) | PTE_AF | PTE_NORMAL | PTE_VALID;
  }
  uint64 uart_pa = 0x3f200000UL;
  uint64 uart_va = KERNBASE + uart_pa;
  l2kernel[PX(2, uart_va)] = PA2PTE(uart_pa) |PTE_AF | PTE_DEVICE | PTE_VALID;

  // memblock_init() parses the DTB through the bootstrap TTBR1 map, before
  // the final page table exists (Linux maps it through the fixmap).  Map
  // the 2 MiB block holding it and the next one, in case it straddles.
  // Only plausible RAM addresses below the peripheral window qualify;
  // memblock_init() still checks the magic number.
  if(dtb_pa != 0 && (dtb_pa & 7) == 0 && dtb_pa < 0x3f000000UL){
    for(uint64 pa = dtb_pa & ~(l2block - 1); pa <= dtb_pa + l2block &&
        pa < 0x3f000000UL; pa += l2block){
      uint64 va = KERNBASE + pa;
      if(pa >= pa_end)
        l2kernel[PX(2, va)] = PA2PTE(pa) | PTE_AF | PTE_NORMAL | PTE_VALID;
    }
  }

  l1kernel[PX(1, KERNBASE)] = PA2PTE((uint64)l2kernel) |PTE_TABLE | PTE_VALID;
}
