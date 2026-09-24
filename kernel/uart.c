//
// Low-level driver routines for the BCM2837 AUX Mini UART.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "aux.h"

#define GPIO_BASE (PERIPHERAL_BASE + 0x200000L)
#define GPFSEL1    0x04
#define GPPUD      0x94
#define GPPUDCLK0  0x98

static inline volatile uint32 *
gpio_reg(uint32 offset)
{
  return (volatile uint32 *)(GPIO_BASE + offset);
}

static void
uart_delay(int count)
{
  while(count-- > 0)
    asm volatile("nop");
}

static void
uartputc_early(int c)
{
  while((REGS_AUX->mu_lsr & MU_LSR_TX_SPACE) == 0)
    ;
  REGS_AUX->mu_io = c;
}

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w; // write next to uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // read next from uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

extern volatile int panicked; // from printf.c

void uartstart();

void
uartinit(void)
{
  // Route GPIO14 (TXD1) and GPIO15 (RXD1) to the Mini UART using ALT5.
  uint32 fsel = *gpio_reg(GPFSEL1);
  fsel &= ~((7U << 12) | (7U << 15));
  fsel |= (2U << 12) | (2U << 15);
  *gpio_reg(GPFSEL1) = fsel;

  // Disable GPIO pulls using the BCM2837 legacy GPPUD sequence.
  *gpio_reg(GPPUD) = 0;
  uart_delay(150);
  *gpio_reg(GPPUDCLK0) = (1U << 14) | (1U << 15);
  uart_delay(150);
  *gpio_reg(GPPUDCLK0) = 0;
  asm volatile("dmb sy" ::: "memory");

  // Enable AUX Mini UART, configure 8N1, clear FIFOs, and set 115200 baud.
  // BAUD = core_freq / (8 * baud) - 1; config.txt fixes core_freq at 250 MHz.
  REGS_AUX->enables |= 1U;
  REGS_AUX->mu_control = 0;
  REGS_AUX->mu_ier = 0;
  REGS_AUX->mu_lcr = 3;
  REGS_AUX->mu_mcr = 0;
  REGS_AUX->mu_iir = 0xc6;
  REGS_AUX->mu_baud_rate = 270;
  REGS_AUX->mu_ier = MU_IER_RX_ENABLE;
  REGS_AUX->mu_control = 3;  // enable receiver and transmitter
  asm volatile("dmb sy" ::: "memory");

  // Earliest real-hardware diagnostic: this does not depend on printf,
  // interrupts, the console device, or the xv6 buffer cache.
  char *ready = "mini-uart: initialized\r\n";
  while(*ready)
    uartputc_early(*ready++);

  initlock(&uart_tx_lock, "uart");
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void
uartputc(int c)
{
  acquire(&uart_tx_lock);

  if(panicked){
    for(;;)
      ;
  }

  while(1){
    if(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      sleep(&uart_tx_r, &uart_tx_lock);
    } else {
      uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
      uart_tx_w += 1;
      uartstart();
      release(&uart_tx_lock);
      return;
    }
  }
}

// alternate version of uartputc() that doesn't 
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  // wait for ... TODO: comment */
  while((REGS_AUX->mu_lsr & MU_LSR_TX_SPACE) == 0)
    ;
  REGS_AUX->mu_io = c;

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uartstart()
{
  while(1){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      REGS_AUX->mu_ier &= ~MU_IER_TX_ENABLE;
      return;
    }
    
    if((REGS_AUX->mu_lsr & MU_LSR_TX_SPACE) == 0){
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      REGS_AUX->mu_ier |= MU_IER_TX_ENABLE;
      return;
    }
    
    int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
    uart_tx_r += 1;
    
    // maybe uartputc() is waiting for space in the buffer.
    wakeup(&uart_tx_r);
    
    REGS_AUX->mu_io = c;
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  if((REGS_AUX->mu_lsr & MU_LSR_DATA_READY) == 0)
    return -1;
  return REGS_AUX->mu_io & 0xff;
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from trap.c.
void
uartintr(void)
{
  // read and process incoming characters.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }

  // send buffered characters.
  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);

  // Mini UART interrupts are cleared by servicing RX/TX. uartstart() also
  // disables the TX-empty interrupt once the software queue is empty.
}
