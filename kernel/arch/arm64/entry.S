#include "aarch64.h" 
#include "memlayout.h"
#include <kernel/param.h>

        // QEMU's raspi3b firmware stub loads kernel8.img at 0x80000.
.section ".text"
.global _entry
_entry:
        // Remember the hardware CPU id.  Every core must first normalize its
        // exception level; secondary cores then wait until CPU0 has built the
        // bootstrap page tables.
        mrs x15, mpidr_el1
        and x15, x15, #0xff
        // LLD's custom armstub preserves EL3, while the firmware armstub and
        // QEMU normally enter in EL2.  Normalize either path to EL1h.
        mrs x0, CurrentEL
        cmp x0, #0xc
        b.ne 2f
        mov x0, #(1 << 31)       // HCR_EL2.RW: EL1 is AArch64
        msr hcr_el2, x0
        msr cntvoff_el2, xzr
        mov x0, #3               // permit EL1 physical counter/timer access
        msr cnthctl_el2, x0
        mov x0, #0x431           // SCR_EL3: RES1, RW, non-secure
        msr scr_el3, x0
        mov x0, #0x3c5           // EL1h, mask D/A/I/F during boot
        msr spsr_el3, x0
        adr x0, 3f
        msr elr_el3, x0
        eret
2:
        cmp x0, #0x8
        b.ne 3f
        mov x0, #(1 << 31)       // HCR_EL2.RW: EL1 is AArch64
        msr hcr_el2, x0
        msr cntvoff_el2, xzr
        mov x0, #3
        msr cnthctl_el2, x0
        mov x0, #0x3c5           // EL1h, mask D/A/I/F during boot
        msr spsr_el2, x0
        adr x0, 3f
        msr elr_el2, x0
        eret
3:
        // Match the known-good LLD boot path: do not inherit unknown
        // SCTLR_EL1 cache/MMU state from firmware or a custom armstub.
        ldr x0, =((3 << 28) | (3 << 22) | (1 << 20) | (1 << 11))
        msr sctlr_el1, x0
        isb

        cbnz x15, secondary_wait

        // The working LLD path accesses peripherals only after EL3 -> EL1.
        // Keep the marker before stacks/MMU, but never poll Mini UART at EL3.
        bl early_mini_uart
        b entry

entry:
        // clear .bss
        adrp x1, bss_start
        ldr w2, =bss_size
1:
        cbz w2, 2f
        str xzr, [x1], #8
        sub w2, w2, #1
        b 1b
2:
        ldr x9, =0x3f215000
        mov w12, #'B'            // BSS clear completed
        bl early_mini_uart_putc


        //call c function to set up the entry pagetable
        adrp x9, stack0
        add  x9, x9, :lo12:stack0
        add  x9, x9, #1, lsl #12     // stack0 + 4096
        mov  sp, x9
        // x0 = physical address of l1entrypgt
        adrp x0, l1entrypgt
        // x1 = physical address of l2entrypgt
        adrp x1, l2entrypgt
        // x2 = physical address of l1kpgt
        adrp x2, l1kpgt
        // x3 = physical address of l2kpgt
        adrp x3, l2kpgt
        // x4 = physical end of kernel image
        ldr x4, =EARLYTOP        // map all early allocator RAM, not just the image
        bl entrypgtinit
        


        // set up entry pagetable
        //
        // Phase 1.
        // map the kernel code identically.
        // Map the first 2MB both identically and at KERNBASE. The kernel
        // starts at 0x80000, which is inside this aligned block.
        // memory type is normal
        //
        // Phase 2.
        // map the kernel code.
        // map [0xffffff8040000000,VA(end)) to [0x40000000,PA(end))
        // memory type is normal.

        // Phase 1
        // map [0x40000000,PA(end)) to [0x40000000,PA(end))
        /*
       
        adrp x0, l2entrypgt

        mov x1, #0
        ldr x2, =V2P_WO(end)-1

        lsr x3, x1, #PXSHIFT(2)
        and x3, x3, #PXMASK   // PX(2, x1)
        lsr x4, x2, #PXSHIFT(2)
        and x4, x4, #PXMASK   // PX(2, x2)
        mov x5, #(PTE_AF | PTE_NORMAL | PTE_VALID) // entry attr
        orr x6, x1, x5      // block entry
l2epgt_loop:
        str x6, [x0, x3, lsl #3]  // l2entrypgt[l2idx] =x6 block entry
        add x3, x3, #1          // next index
        add x6, x6, #0x200000   // next block, block size is 2MB
        cmp x3, x4
        b.ls l2epgt_loop    // if start va idx <= end va idx

        adrp x0, l1entrypgt

        lsr x3, x1, #PXSHIFT(1)
        and x3, x3, #PXMASK     // start va level1 index

        mov x4, #(PTE_TABLE | PTE_VALID)  // entry attr
        adrp x5, l2entrypgt
        orr x6, x4, x5    // table entry

        str x6, [x0, x3, lsl #3]    // l1entrypgt[l1idx] = table entry

        // Phase 2
        // map [KERNBASE,KERNBASE+2MB) to physical [0,2MB)
        adrp x0, l2kpgt

        mov x1, #0            // start pa
        ldr x2, =V2P_WO(end)-1   // end pa
        mov x3, #KERNBASE
        add x4, x1, x3    // start va
        add x5, x2, x3    // end va

        lsr x6, x4, #PXSHIFT(2)
        and x6, x6, #PXMASK   // x6 = PX(2,x4)
        lsr x7, x5, #PXSHIFT(2)
        and x7, x7, #PXMASK   // x7 = PX(2,x5)
        mov x8, #(PTE_AF | PTE_NORMAL | PTE_VALID) // entry attr
        orr x9, x1, x8      // block entry
l2kpgt_loop:
        str x9, [x0, x6, lsl #3]  // l2entrypgt[l2idx] = block entry
        add x6, x6, #1          // next index
        add x9, x9, #0x200000   // next block, block size is 2MB
        cmp x6, x7
        b.ls l2kpgt_loop    // if start va idx <= end va idx

        // Keep the Mini UART MMIO window reachable through TTBR1 while the
        // bootstrap table is active.  0x3f215000 lies in the 2MB block
        // 0x3f200000..0x3f3fffff.
        ldr x10, =(KERNBASE + 0x3f200000)
        lsr x11, x10, #PXSHIFT(2)
        and x11, x11, #PXMASK
        ldr x12, =(0x3f200000 | PTE_AF | PTE_INDX(AI_DEVICE_nGnRnE_IDX) | PTE_VALID)
        str x12, [x0, x11, lsl #3]

        adrp x0, l1kpgt

        lsr x5, x4, #PXSHIFT(1)
        and x5, x5, #PXMASK  // x5 = PX(1,x4)

        mov x6, #(PTE_TABLE | PTE_VALID)  // entry attr
        adrp x7, l2kpgt
        orr x8, x6, x7    // table entry

        str x8, [x0, x5, lsl #3]    // l1kpgt[l1idx] = table entry

        // Real hardware: the boot loader may have run with the D-cache on,
        // so stale lines for the page tables, stack0 and .bss can survive
        // into EL1.  Everything above was written with SCTLR_EL1.C = 0 and
        // went straight to memory; once the MMU is enabled with cacheable
        // table walks (TCR IRGN/ORGN = 1) and SCTLR.C = 1, a stale line
        // would hide those writes and the first fetch after the MMU switch
        // faults.  QEMU does not model caches, so it never sees this.
        // Discard the lines for the whole image [_entry, end) to the PoC
        // (Linux head.S invalidates its early page tables the same way).
        // Only CPU0 does this, before releasing the secondary cores, so it
        // can never discard data a core has already written through its
        // cache.
        adrp x0, _entry              // physical start of the image
        adrp x1, end
        add x1, x1, :lo12:end        // physical end of .bss
        mrs x2, ctr_el0
        ubfx x2, x2, #16, #4         // CTR_EL0.DminLine = log2(words)
        mov x3, #4
        lsl x3, x3, x2               // smallest D-cache line in bytes
        sub x4, x3, #1
        bic x0, x0, x4               // align start down to a line
5:
        dc ivac, x0                  // invalidate this line to the PoC
        add x0, x0, x3
        cmp x0, x1
        b.lo 5b
        dsb sy
        isb
       */

        // Publish the completed bootstrap tables to cores already released
        // by an armstub.  stlr/ldar plus SEV provides the required ordering.
        adrp x10, secondary_release
        add x10, x10, :lo12:secondary_release
        mov w11, #1
        stlr w11, [x10]
        sev
        b entryothers

secondary_wait:
        adrp x10, secondary_release
        add x10, x10, :lo12:secondary_release
4:      ldar w11, [x10]
        cbnz w11, entryothers
        wfe
        b 4b

entryothers:  // secondary CPU starts here
        // load pagetable
        adrp x0, l1entrypgt
        adrp x1, l1kpgt
        msr ttbr0_el1, x0
        msr ttbr1_el1, x1

        // setup tcr
        ldr x0, =(TCR_T0SZ(25)|TCR_IRGN0(1)|TCR_ORGN0(1)|TCR_SH0(3)|TCR_TG0(0)|TCR_T1SZ(25)|TCR_IRGN1(1)|TCR_ORGN1(1)|TCR_SH1(3)|TCR_TG1(2)|TCR_IPS(0))
        msr tcr_el1, x0

        // setup mair
        ldr x1, =((MT_DEVICE_nGnRnE<<(8*AI_DEVICE_nGnRnE_IDX)) | (MT_NORMAL_NC<<(8*AI_NORMAL_NC_IDX)) | (MT_NORMAL<<(8*AI_NORMAL_IDX)))
        msr mair_el1, x1

        isb

        ldr x1, =_start   // x1 = VA(_start)

        // enable paging
        ldr x9, =0x3f215000
        mov w12, #'P'            // temporary page tables/registers ready
        bl early_mini_uart_putc
        dsb ish
        tlbi vmalle1
        dsb ish
        isb
        ic iallu
        dsb sy
        isb
        mrs x0, sctlr_el1
        orr x0, x0, #1
        orr x0, x0, #(1 << 2)
        orr x0, x0, #(1 << 12)
        msr sctlr_el1, x0
        isb

        br x1       // jump to higher address (0xffffff8000000000~)

_start:
        // set up a stack for C.
        // stack0 is declared in start.c,
        // with a 4096-byte stack per CPU.
        // sp = stack0 + ((cpuid+1) * 4096)
        // cpuid = mpidr_el1 & 0xff
        ldr x0, =stack0
        mov x1, #1024*4
        mrs x2, mpidr_el1
        and x2, x2, #0x3
        add x2, x2, #1
        mul x1, x1, x2
        add x0, x0, x1
        mov sp, x0
        mov w0, #'H'             // high-half jump and stack are usable
        bl boot_uart_mark
        // jump to main()
        b main

        b .   // spin

.section ".data"
.align 3
secondary_release:
        .word 0

.section ".text"

// Configure GPIO14/15 and AUX Mini UART using physical addresses, then emit
// "E\r\n".  No stack, BSS, page table, interrupt, or C runtime is required.
early_mini_uart:
        mov x13, x30              // preserve caller LR without a stack
        // GPIO14/15 = ALT5 (function value 2).
        ldr x9, =0x3f200004       // GPFSEL1
        ldr w10, [x9]
        ldr w11, =0x0003f000
        bic w10, w10, w11
        ldr w11, =0x00012000
        orr w10, w10, w11
        str w10, [x9]

        // Disable pulls for GPIO14/15.
        ldr x9, =0x3f200094       // GPPUD
        str wzr, [x9]
        mov w10, #150
1:      subs w10, w10, #1
        b.ne 1b
        ldr x9, =0x3f200098       // GPPUDCLK0
        mov w10, #0xc000
        str w10, [x9]
        mov w10, #150
1:      subs w10, w10, #1
        b.ne 1b
        str wzr, [x9]

        // AUX Mini UART: enable, 8N1, clear FIFOs, 115200 @ 250 MHz.
        ldr x9, =0x3f215000
        ldr w10, [x9, #0x04]
        orr w10, w10, #1
        str w10, [x9, #0x04]
        str wzr, [x9, #0x60]
        str wzr, [x9, #0x44]
        mov w10, #3
        str w10, [x9, #0x4c]
        str wzr, [x9, #0x50]
        mov w10, #0xc6
        str w10, [x9, #0x48]
        mov w10, #270
        str w10, [x9, #0x68]
        mov w10, #3
        str w10, [x9, #0x60]
        dmb sy

        mov w12, #'E'
        bl early_mini_uart_putc
        mov w12, #'\r'
        bl early_mini_uart_putc
        mov w12, #'\n'
        bl early_mini_uart_putc
        mov x30, x13
        ret

early_mini_uart_putc:
1:      ldr w10, [x9, #0x54]
        tbz w10, #5, 1b
        str w12, [x9, #0x40]
        ret

.global boot_uart_mark
boot_uart_mark:
        mov w12, w0
        ldr x9, =(KERNBASE + 0x3f215000)
        b early_mini_uart_putc

.global psci_call
psci_call:
        hvc #0
        ret
