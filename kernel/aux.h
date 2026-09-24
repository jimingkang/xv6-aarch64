#ifndef XV6_AUX_H
#define XV6_AUX_H

#include "types.h"
#include "memlayout.h"

// BCM2837 auxiliary peripheral register layout.  The Mini UART registers
// start at AUX base + 0x40; the reserved array preserves that exact offset.
struct aux_regs {
  volatile uint32 irq_status;       // 0x00
  volatile uint32 enables;          // 0x04
  volatile uint32 reserved[14];     // 0x08..0x3c
  volatile uint32 mu_io;            // 0x40
  volatile uint32 mu_ier;           // 0x44
  volatile uint32 mu_iir;           // 0x48
  volatile uint32 mu_lcr;           // 0x4c
  volatile uint32 mu_mcr;           // 0x50
  volatile uint32 mu_lsr;           // 0x54
  volatile uint32 mu_msr;           // 0x58
  volatile uint32 mu_scratch;       // 0x5c
  volatile uint32 mu_control;       // 0x60
  volatile uint32 mu_status;        // 0x64
  volatile uint32 mu_baud_rate;     // 0x68
};

#define REGS_AUX ((struct aux_regs *)MINI_UART)

#define MU_LSR_DATA_READY  (1U << 0)
#define MU_LSR_TX_SPACE    (1U << 5)
#define MU_IER_RX_ENABLE   (1U << 0)
#define MU_IER_TX_ENABLE   (1U << 1)

#endif
