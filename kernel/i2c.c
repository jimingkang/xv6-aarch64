// BCM2837 BSC (Broadcom Serial Controller) I2C master, polled.
//
// Register layout and bits follow Linux drivers/i2c/busses/i2c-bcm2835.c.
// Only BSC0 is used today: on the Pi 3B+ the camera connector's I2C is BSC0
// routed to GPIO 44/45 with ALT1 (bcm283x-rpi-i2c0mux_0_44.dtsi).
//
// Transfers are short (camera registers are 3-byte writes and 2+1-byte
// reads), so the FIFO is polled with a timeout instead of using interrupts.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "mbox.h"
#include "i2c.h"

#define GPIO_BASE   (PERIPHERAL_BASE + 0x200000UL)
#define BSC0_BASE   (PERIPHERAL_BASE + 0x205000UL)
#define BSC1_BASE   (PERIPHERAL_BASE + 0x804000UL)

#define BSC_C     0x00
#define BSC_S     0x04
#define BSC_DLEN  0x08
#define BSC_A     0x0c
#define BSC_FIFO  0x10
#define BSC_DIV   0x14
#define BSC_DEL   0x18

#define C_READ   (1U << 0)
#define C_CLEAR  (1U << 4)          // bits 4 and 5 both clear the FIFO
#define C_ST     (1U << 7)
#define C_INTT   (1U << 9)
#define C_I2CEN  (1U << 15)

#define S_TA     (1U << 0)
#define S_DONE   (1U << 1)
#define S_TXW    (1U << 2)
#define S_TXD    (1U << 4)
#define S_RXD    (1U << 5)
#define S_ERR    (1U << 8)          // slave did not ACK
#define S_CLKT   (1U << 9)          // clock stretch timeout

#define I2C_HZ       100000U
#define FIFO_DEPTH   16
#define XFER_TIMEOUT_US 20000
#define ABORT_TIMEOUT_US 20000

static struct spinlock i2c_lock;
static int i2c_ready;
static uint32 last_status[2];
static uint32 i2c0_core_hz = 250000000U;

static uint64
bsc_base(int bus)
{
  return bus == 1 ? BSC1_BASE : BSC0_BASE;
}

static inline uint32
rd(int bus, uint32 off)
{
  return *(volatile uint32 *)(bsc_base(bus) + off);
}

static inline void
wr(int bus, uint32 off, uint32 v)
{
  *(volatile uint32 *)(bsc_base(bus) + off) = v;
}

// Complete all preceding Device-memory writes before asking the BSC state
// machine to start.  Device-nGnRnE mappings already preserve ordering, but an
// explicit barrier also makes this assumption independent of the early page
// table attributes while the camera code is being brought up.
static inline void
bsc_order(void)
{
  asm volatile("dsb sy" ::: "memory");
}

static uint32
bsc0_program_rate(uint32 hz)
{
  uint32 div, fedl, redl;

  if(hz == 0)
    hz = I2C_HZ;
  div = (i2c0_core_hz + hz - 1) / hz;
  if(div & 1U)
    div++;                          // hardware ignores bit 0: round up
  if(div < 2)
    div = 2;
  if(div > 0xfffe)
    div = 0xfffe;
  fedl = div / 16;
  redl = div / 4;
  if(fedl == 0)
    fedl = 1;
  if(redl == 0)
    redl = 1;
  wr(0, BSC_DIV, div);
  wr(0, BSC_DEL, (fedl << 16) | redl);
  bsc_order();
  return i2c0_core_hz / div;
}

// Return the BSC state machine to idle.  Real BCM2837 needs its clocked state
// machine left enabled long enough to emit NACK+STOP.  If I2CEN is removed
// while TA is still set, TA can remain frozen at 1 (QEMU does not model this).
// First clear/abort with I2CEN retained, wait for idle, then leave the engine
// disabled and clear the sticky status bits like Linux i2c-bcm2835.
static void
bsc_abort(int bus)
{
  uint64 deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * ABORT_TIMEOUT_US / 1000000;

  if(rd(bus, BSC_S) & S_TA)
    wr(bus, BSC_C, C_I2CEN | C_CLEAR);
  while((rd(bus, BSC_S) & S_TA) && r_cntvct_el0() < deadline)
    asm volatile("yield" ::: "memory");
  wr(bus, BSC_C, C_CLEAR);          // disabled, FIFO empty for next request
  wr(bus, BSC_S, S_CLKT | S_ERR | S_DONE);
}

// Software I2C disconnects the BSC from the pins immediately afterwards, so
// it must not wait for a wedged BSC TA state.  Force the engine off, clear its
// FIFO/status, order the MMIO writes, then take ownership through GPIO FSEL.
static void
bsc_force_off(int bus)
{
  wr(bus, BSC_C, C_CLEAR);
  wr(bus, BSC_S, S_CLKT | S_ERR | S_DONE);
  bsc_order();
}

static void
gpio_fsel(int pin, uint32 fn)
{
  volatile uint32 *fsel = (volatile uint32 *)(GPIO_BASE + (pin / 10) * 4);
  uint32 shift = (pin % 10) * 3;
  *fsel = (*fsel & ~(7U << shift)) | (fn << shift);
}

static uint32
gpio_get_fsel(int pin)
{
  volatile uint32 *fsel = (volatile uint32 *)(GPIO_BASE + (pin / 10) * 4);
  return (*fsel >> ((pin % 10) * 3)) & 7;
}

#define FSEL_INPUT 0
#define FSEL_OUTPUT 1
#define FSEL_ALT0  4
#define FSEL_ALT1  5

static void
bb_delay(void)
{
  // Clone modules and long ribbon cables showed marginal ACK/data timing.
  // A 20 us half-cycle gives roughly 25 kHz and generous SCCB margin.
  uint64 end = r_cntvct_el0() + r_cntfrq_el0() / 50000; // about 20 us
  while(r_cntvct_el0() < end)
    asm volatile("yield" ::: "memory");
}

static int
gpio_level(int pin)
{
  volatile uint32 *lev = (volatile uint32 *)(GPIO_BASE + 0x34 +
                                             (pin / 32) * 4);
  return (*lev >> (pin % 32)) & 1;
}

// Open-drain GPIO: drive low by selecting output after setting its latch low;
// release high by selecting input and relying on the camera bus pull-up.
static void
bb_low(int pin)
{
  volatile uint32 *clr = (volatile uint32 *)(GPIO_BASE + 0x28 +
                                             (pin / 32) * 4);
  *clr = 1U << (pin % 32);
  gpio_fsel(pin, FSEL_OUTPUT);
  bb_delay();
}

static int
bb_release(int pin)
{
  uint64 deadline;

  gpio_fsel(pin, FSEL_INPUT);
  deadline = r_cntvct_el0() + r_cntfrq_el0() / 10000; // 100 us stretch
  while(!gpio_level(pin) && r_cntvct_el0() < deadline)
    asm volatile("yield" ::: "memory");
  bb_delay();
  return gpio_level(pin) ? 0 : -1;
}

static int
bb_start(void)
{
  if(bb_release(44) < 0 || bb_release(45) < 0)
    return -1;
  bb_low(44);                       // SDA falls while SCL is high
  bb_low(45);
  return 0;
}

static void
bb_stop(void)
{
  bb_low(44);
  if(bb_release(45) == 0)
    (void)bb_release(44);           // SDA rises while SCL is high
}

// Recover a target left in the middle of a byte by a failed BSC transaction.
// With SDA released, nine SCL pulses let any pending byte and ACK finish;
// STOP then returns every compliant I2C/SCCB target to its idle state.
static int
bb_recover_bus(void)
{
  gpio_fsel(44, FSEL_INPUT);
  gpio_fsel(45, FSEL_INPUT);
  bb_delay();

  // Do not clock an already-idle target before every transaction.  Recovery
  // clocks are required only when a target is holding SDA low because a
  // previous hardware-BSC request stopped part-way through a byte.
  if(!gpio_level(45) && bb_release(45) < 0)
    return -1;
  if(!gpio_level(44)){
    for(int i = 0; i < 9 && !gpio_level(44); i++){
      bb_low(45);
      if(bb_release(45) < 0)
        return -1;
    }
  }
  // A STOP also resets a target whose interrupted byte happened to leave SDA
  // high, a condition that cannot be distinguished electrically from idle.
  bb_stop();
  return gpio_level(44) && gpio_level(45) ? 0 : -1;
}

static int
bb_write_byte(uint8 byte)
{
  for(int bit = 7; bit >= 0; bit--){
    if(byte & (1U << bit)){
      if(bb_release(44) < 0)
        return -1;
    } else {
      bb_low(44);
    }
    if(bb_release(45) < 0)
      return -1;
    bb_low(45);
  }

  // ACK phase: release SDA without waiting for high, then sample while SCL
  // is high.  Low is ACK.
  gpio_fsel(44, FSEL_INPUT);
  bb_delay();
  if(bb_release(45) < 0)
    return -1;
  bb_delay();
  int ack = !gpio_level(44);
  bb_low(45);
  return ack ? 0 : -1;
}

static int
bb_read_byte(uint8 *byte, int ack)
{
  uint8 v = 0;

  gpio_fsel(44, FSEL_INPUT);
  for(int bit = 7; bit >= 0; bit--){
    if(bb_release(45) < 0)
      return -1;
    v |= gpio_level(44) << bit;
    bb_low(45);
  }
  // Master ACKs every byte except the final one, which is NACKed.
  if(ack)
    bb_low(44);
  else if(bb_release(44) < 0)
    return -1;
  if(bb_release(45) < 0)
    return -1;
  bb_low(45);
  gpio_fsel(44, FSEL_INPUT);
  *byte = v;
  return 0;
}

int
i2c0_bitbang_probe(uint8 addr)
{
  uint32 old_sda, old_scl;
  int ack = -1;

  if(!i2c_ready)
    return -2;
  acquire(&i2c_lock);
  bsc_force_off(0);
  old_sda = gpio_get_fsel(44);
  old_scl = gpio_get_fsel(45);

  // Bus idle, START: SDA falls while SCL is high.
  if(bb_release(44) < 0 || bb_release(45) < 0){
    ack = -2;
    goto out;
  }
  bb_low(44);
  bb_low(45);

  uint8 byte = addr << 1;           // address + write direction
  for(int bit = 7; bit >= 0; bit--){
    if(byte & (1U << bit)){
      if(bb_release(44) < 0){
        ack = -2;
        goto stop;
      }
    } else {
      bb_low(44);
    }
    if(bb_release(45) < 0){
      ack = -2;
      goto stop;
    }
    bb_low(45);
  }

  // Ninth clock: release SDA without waiting for it to rise.  The target is
  // supposed to hold SDA low for ACK, so bb_release() (which waits for high)
  // is deliberately wrong for this one phase and used to add a 100 us pause.
  gpio_fsel(44, FSEL_INPUT);
  bb_delay();
  if(bb_release(45) < 0){
    ack = -2;
    goto stop;
  }
  bb_delay();                       // sample near the middle of SCL high
  ack = gpio_level(44) ? -1 : 0;
  bb_low(45);

stop:
  // STOP: SDA rises while SCL is high.
  bb_low(44);
  if(bb_release(45) == 0)
    (void)bb_release(44);
out:
  gpio_fsel(44, old_sda);
  gpio_fsel(45, old_scl);
  release(&i2c_lock);
  return ack;
}

int
i2c0_bitbang_write_read(uint8 addr, const uint8 *wbuf, int wlen,
                        uint8 *rbuf, int rlen)
{
  uint32 old_sda, old_scl;
  int result = -1;

  if(!i2c_ready || wlen < 0 || rlen < 0 || (wlen && wbuf == 0) ||
     (rlen && rbuf == 0) || (wlen == 0 && rlen == 0))
    return -1;
  acquire(&i2c_lock);
  bsc_force_off(0);
  old_sda = gpio_get_fsel(44);
  old_scl = gpio_get_fsel(45);

  if(bb_recover_bus() < 0)
    goto out_stop;

  if(wlen){
    if(bb_start() < 0 || bb_write_byte(addr << 1) < 0)
      goto out_stop;
    for(int i = 0; i < wlen; i++)
      if(bb_write_byte(wbuf[i]) < 0)
        goto out_stop;
    bb_stop();                      // SCCB split write/read transaction
  }
  if(rlen){
    if(bb_start() < 0 || bb_write_byte((addr << 1) | 1) < 0)
      goto out_stop;
    for(int i = 0; i < rlen; i++)
      if(bb_read_byte(&rbuf[i], i + 1 < rlen) < 0)
        goto out_stop;
  }
  result = 0;
out_stop:
  bb_stop();
  gpio_fsel(44, old_sda);
  gpio_fsel(45, old_scl);
  release(&i2c_lock);
  return result;
}

int
i2c0_init_camera_pins(void)
{
  uint32 core = mbox_get_clock_rate(MBOX_CLOCK_CORE);
  uint32 actual;

  if(!i2c_ready){
    initlock(&i2c_lock, "i2c");
    i2c_ready = 1;
  }
  // BSC0 can also appear on GPIO 0/1 (ALT0, the HAT ID EEPROM).  Never
  // connect both pin pairs to the same controller.
  for(int pin = 0; pin <= 1; pin++)
    if(gpio_get_fsel(pin) == FSEL_ALT0)
      gpio_fsel(pin, FSEL_INPUT);
  gpio_fsel(44, FSEL_ALT1);         // SDA0
  gpio_fsel(45, FSEL_ALT1);         // SCL0

  if(core == 0)
    core = 250000000U;              // config.txt: core_freq=250
  i2c0_core_hz = core;
  // Match Linux i2c-bcm2835.  Firmware may leave non-default edge delays.
  actual = bsc0_program_rate(I2C_HZ);
  wr(0, BSC_C, C_I2CEN | C_CLEAR);
  wr(0, BSC_S, S_CLKT | S_ERR | S_DONE);
  printf("i2c0: GPIO44/45 ALT1, %d Hz (core %d), "
         "BCM2837 two-stage abort\n", actual, core);
  return 0;
}

uint32
i2c0_set_rate(uint32 hz)
{
  uint32 actual;

  if(!i2c_ready)
    return 0;
  acquire(&i2c_lock);
  bsc_abort(0);
  actual = bsc0_program_rate(hz);
  release(&i2c_lock);
  return actual;
}

// Wait for DONE; while waiting, feed (write) or drain (read) the FIFO.
static int
bsc_run(int bus, uint8 *rbuf, const uint8 *wbuf, int len)
{
  uint64 deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * XFER_TIMEOUT_US / 1000000;
  int i = 0;
  uint32 s;

  for(;;){
    s = rd(bus, BSC_S);
    if(s & (S_ERR | S_CLKT))
      break;
    if(wbuf)
      while(i < len && (rd(bus, BSC_S) & S_TXD))
        wr(bus, BSC_FIFO, wbuf[i++]);
    if(rbuf)
      while(i < len && (rd(bus, BSC_S) & S_RXD))
        rbuf[i++] = rd(bus, BSC_FIFO);
    if(s & S_DONE)
      break;
    if(r_cntvct_el0() >= deadline){
      s |= S_CLKT;
      break;
    }
  }
  if(rbuf)                          // bytes that arrived with DONE
    while(i < len && (rd(bus, BSC_S) & S_RXD))
      rbuf[i++] = rd(bus, BSC_FIFO);
  last_status[bus == 1 ? 1 : 0] = s;
  bsc_abort(bus);
  if(s & (S_ERR | S_CLKT))
    return -1;
  return i == len ? 0 : -1;
}

int
i2c_write(int bus, uint8 addr, const uint8 *buf, int len)
{
  int r, n;

  if(!i2c_ready || len <= 0 || len > 0xffff)
    return -1;
  acquire(&i2c_lock);
  bsc_abort(bus);
  wr(bus, BSC_A, addr);
  wr(bus, BSC_DLEN, len);
  n = len < FIFO_DEPTH ? len : FIFO_DEPTH;
  for(int i = 0; i < n; i++)        // prime the FIFO before START
    wr(bus, BSC_FIFO, buf[i]);
  bsc_order();
  wr(bus, BSC_C, C_I2CEN | C_ST);
  r = bsc_run(bus, 0, buf + n, len - n) ;
  release(&i2c_lock);
  return r;
}

int
i2c_read(int bus, uint8 addr, uint8 *buf, int len)
{
  int r;

  if(!i2c_ready || len <= 0 || len > 0xffff)
    return -1;
  acquire(&i2c_lock);
  bsc_abort(bus);
  wr(bus, BSC_A, addr);
  wr(bus, BSC_DLEN, len);
  bsc_order();
  wr(bus, BSC_C, C_I2CEN | C_ST | C_READ);
  r = bsc_run(bus, buf, 0, len);
  release(&i2c_lock);
  return r;
}

// Combined write/read with a repeated START.  The BCM2835 BSC has a state
// machine quirk: to generate Sr reliably the write FIFO must not be prefilled.
// Linux i2c-bcm2835 therefore starts an empty write, waits for TXW, fills the
// FIFO, and programs the final read immediately after the last write byte.
int
i2c_write_read(int bus, uint8 addr, const uint8 *wbuf, int wlen,
               uint8 *rbuf, int rlen)
{
  uint64 deadline;
  uint32 s = 0;
  int wi = 0, ri = 0, reading = 0, result = -1;

  if(!i2c_ready || wbuf == 0 || rbuf == 0 || wlen <= 0 || rlen <= 0 ||
     wlen > 0xffff || rlen > 0xffff)
    return -1;

  acquire(&i2c_lock);
  bsc_abort(bus);
  wr(bus, BSC_A, addr);
  wr(bus, BSC_DLEN, wlen);
  // INTT makes TXW usable as the hand-off point.  We poll the status bit;
  // no CPU interrupt is enabled by this driver.
  bsc_order();
  wr(bus, BSC_C, C_I2CEN | C_ST | C_INTT);

  deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * XFER_TIMEOUT_US / 1000000;
  for(;;){
    s = rd(bus, BSC_S);
    if(s & (S_ERR | S_CLKT))
      break;

    if(!reading){
      if(s & S_TXW){
        while(wi < wlen && (rd(bus, BSC_S) & S_TXD))
          wr(bus, BSC_FIFO, wbuf[wi++]);
        if(wi == wlen){
          // Switch to the final read while TA is still set: this creates Sr
          // rather than allowing the controller to emit STOP.
          wr(bus, BSC_A, addr);
          wr(bus, BSC_DLEN, rlen);
          bsc_order();
          wr(bus, BSC_C, C_I2CEN | C_ST | C_READ);
          reading = 1;
        }
      }
      if((s & S_DONE) && !reading)
        break;
    } else {
      while(ri < rlen && (rd(bus, BSC_S) & S_RXD))
        rbuf[ri++] = rd(bus, BSC_FIFO);
      if(s & S_DONE){
        while(ri < rlen && (rd(bus, BSC_S) & S_RXD))
          rbuf[ri++] = rd(bus, BSC_FIFO);
        if(ri == rlen)
          result = 0;
        break;
      }
    }

    if(r_cntvct_el0() >= deadline){
      s |= S_CLKT;
      break;
    }
  }

  last_status[bus == 1 ? 1 : 0] = s;
  bsc_abort(bus);
  release(&i2c_lock);
  return result;
}

uint32
i2c_last_status(int bus)
{
  return last_status[bus == 1 ? 1 : 0];
}

void
i2c0_diagnostics(uint32 *status, uint32 *divider, uint32 *pins,
                 uint32 *levels)
{
  if(status)
    *status = rd(0, BSC_S);
  if(divider)
    *divider = rd(0, BSC_DIV);
  if(pins)
    *pins = gpio_get_fsel(44) | (gpio_get_fsel(45) << 4);
  if(levels){
    // GPIO44/45 are bits 12/13 in GPLEV1 (GPIO_BASE + 0x38).
    uint32 lev1 = *(volatile uint32 *)(GPIO_BASE + 0x38);
    *levels = (lev1 >> 12) & 3U;
  }
}
