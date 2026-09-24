// BCM2837 Arasan eMMC/SDHCI driver.
//
// This is a polling, PIO-only driver.  xv6 filesystem blocks are 1024 bytes,
// while an SD sector is 512 bytes, so sdrw() transfers two sectors per buf.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"

#define EMMC_BASE       (PERIPHERAL_BASE + 0x00300000UL)
#define GPIO_BASE       (PERIPHERAL_BASE + 0x00200000UL)

#define EMMC_BLKSIZECNT 0x04
#define EMMC_ARG1       0x08
#define EMMC_CMDTM      0x0c
#define EMMC_RESP0      0x10
#define EMMC_DATA       0x20
#define EMMC_STATUS     0x24
#define EMMC_CONTROL0   0x28
#define EMMC_CONTROL1   0x2c
#define EMMC_INTERRUPT  0x30
#define EMMC_IRPT_MASK  0x34
#define EMMC_IRPT_EN    0x38
#define EMMC_CONTROL2   0x3c
#define EMMC_CAP0       0x40

#define MBOX_BASE       (PERIPHERAL_BASE + 0x0000b880UL)
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20
#define MBOX_EMPTY      0x40000000U
#define MBOX_FULL       0x80000000U
#define MBOX_PROP_CH    8U
#define MBOX_GET_CLOCK_RATE 0x00030002U
#define MBOX_CLOCK_EMMC 1U
// The property-mailbox firmware on Pi 3 expects the low ARM physical address
// in the mailbox word.  This matches LLD's identity-mapped pointer.  Do not
// use the 0xc0000000 VideoCore RAM alias here; that alias receives no reply on
// the firmware used by this board.
#define ARM_TO_VC_BUS(pa) ((uint32)(pa))

#define SR_CMD_INHIBIT  (1U << 0)
#define SR_DAT_INHIBIT  (1U << 1)
#define SR_READ_AVAIL   (1U << 11)
#define SR_WRITE_AVAIL  (1U << 10)

#define C1_CLK_INTLEN   (1U << 0)
#define C1_CLK_STABLE   (1U << 1)
#define C1_CLK_EN       (1U << 2)
#define C1_SRST_HC      (1U << 24)
#define C1_SRST_CMD     (1U << 25)
#define C1_SRST_DATA    (1U << 26)

#define INT_CMD_DONE    (1U << 0)
#define INT_DATA_DONE   (1U << 1)
#define INT_WRITE_RDY   (1U << 4)
#define INT_READ_RDY    (1U << 5)
#define INT_ERROR       (1U << 15)
#define INT_ERROR_MASK  0xffff0000U

#define CMD_RSPNS_NONE  (0U << 16)
#define CMD_RSPNS_136   (1U << 16)
#define CMD_RSPNS_48    (2U << 16)
#define CMD_RSPNS_48B   (3U << 16)
#define CMD_CRCCHK_EN   (1U << 19)
#define CMD_IXCHK_EN    (1U << 20)
#define CMD_ISDATA      (1U << 21)
#define TM_DAT_DIR_CH   (1U << 4)

#define CMD_INDEX(n)    ((uint32)(n) << 24)
#define CMD0            (CMD_INDEX(0)  | CMD_RSPNS_NONE)
#define CMD2            (CMD_INDEX(2)  | CMD_RSPNS_136 | CMD_CRCCHK_EN)
#define CMD3            (CMD_INDEX(3)  | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD7            (CMD_INDEX(7)  | CMD_RSPNS_48B | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD8            (CMD_INDEX(8)  | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD16           (CMD_INDEX(16) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)
#define CMD17           (CMD_INDEX(17) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA | TM_DAT_DIR_CH)
#define CMD24           (CMD_INDEX(24) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN | CMD_ISDATA)
#define CMD41           (CMD_INDEX(41) | CMD_RSPNS_48)
#define CMD55           (CMD_INDEX(55) | CMD_RSPNS_48  | CMD_CRCCHK_EN | CMD_IXCHK_EN)

#define SD_SECTOR_SIZE  512
#define SD_INIT_CLOCK   400000
#define SD_NORMAL_CLOCK 25000000

static struct spinlock sdlock;
static uint32 sd_rca;
static int sd_sdhc;
static int sd_ready;
static int sd_io_phase;
static uint32 mbox_clock[16] __attribute__((aligned(64)));

static inline volatile uint32 *
reg(uint64 base, uint32 off)
{
  return (volatile uint32 *)(base + off);
}

static inline uint32
rd(uint32 off)
{
  return *reg(EMMC_BASE, off);
}

static inline void
wr(uint32 off, uint32 value)
{
  *reg(EMMC_BASE, off) = value;
  asm volatile("dmb sy" ::: "memory");
}

static inline uint32
mbox_rd(uint32 off)
{
  return *reg(MBOX_BASE, off);
}

static inline void
mbox_wr(uint32 off, uint32 value)
{
  *reg(MBOX_BASE, off) = value;
  asm volatile("dmb sy" ::: "memory");
}

// The VideoCore property interface accesses this buffer without CPU cache
// coherency.  Clean the request before ringing the mailbox and invalidate the
// line before consuming the firmware response.
static inline void
cache_clean(void *p)
{
  asm volatile("dc cvac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static inline void
cache_invalidate(void *p)
{
  asm volatile("dc ivac, %0\n\tdsb sy" :: "r"(p) : "memory");
}

static uint32
firmware_emmc_clock(void)
{
  uint32 *m = mbox_clock;
  memset(m, 0, 64);
  m[0] = 8 * sizeof(uint32);
  m[1] = 0;                         // property request
  m[2] = MBOX_GET_CLOCK_RATE;
  m[3] = 8;                         // value buffer size
  m[4] = 0;                         // request value length
  m[5] = MBOX_CLOCK_EMMC;
  m[6] = 0;                         // returned rate
  m[7] = 0;                         // end tag

  uint32 pa = (uint32)V2P(m);
  uint32 bus = ARM_TO_VC_BUS(pa);
  uint32 request = (bus & ~0xfU) | MBOX_PROP_CH;
  cache_clean(m);

  uint64 limit = r_cntvct_el0() + r_cntfrq_el0();
  while(mbox_rd(MBOX_STATUS) & MBOX_FULL)
    if(r_cntvct_el0() >= limit){
      printf("sd: mailbox write timeout status=%x pa=%x bus=%x\n",
             mbox_rd(MBOX_STATUS), pa, bus);
      return 0;
    }
  mbox_wr(MBOX_WRITE, request);

  uint32 response = 0;
  uint32 last_response = 0;
  for(;;){
    while(mbox_rd(MBOX_STATUS) & MBOX_EMPTY)
      if(r_cntvct_el0() >= limit){
        printf("sd: mailbox read timeout status=%x pa=%x bus=%x last=%x\n",
               mbox_rd(MBOX_STATUS), pa, bus, last_response);
        return 0;
      }
    response = mbox_rd(MBOX_READ);
    last_response = response;
    // There is only one outstanding property request.  Some Pi firmware
    // revisions return a different RAM alias (PA/0x40000000/0xc0000000), so
    // matching the channel plus the response-buffer status is sufficient.
    if((response & 0xfU) == MBOX_PROP_CH)
      break;
  }

  cache_invalidate(m);
  if(m[1] != 0x80000000U || (m[4] & 0x80000000U) == 0 || m[6] == 0){
    printf("sd: mailbox bad response raw=%x code=%x taglen=%x id=%d rate=%d\n",
           response, m[1], m[4], m[5], m[6]);
    return 0;
  }
  printf("sd: firmware EMMC clock=%d Hz\n", m[6]);
  return m[6];
}

static void
sd_delay_us(uint32 us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  while(r_cntvct_el0() - start < ticks)
    asm volatile("yield" ::: "memory");
}

static int
wait_mask(uint32 off, uint32 mask, uint32 value, uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  do {
    if((rd(off) & mask) == value)
      return 0;
  } while(r_cntvct_el0() - start < ticks);
  return -1;
}

static void
gpio_alt3(int pin)
{
  uint32 off = (pin / 10) * 4;
  uint32 shift = (pin % 10) * 3;
  volatile uint32 *fsel = reg(GPIO_BASE, off);
  uint32 value = *fsel;
  value &= ~(7U << shift);
  value |= 7U << shift;       // ALT3
  *fsel = value;
}

static void
sd_gpio_init(void)
{
  for(int pin = 48; pin <= 53; pin++)
    gpio_alt3(pin);

  // Disable pulls using the BCM2837 GPPUD sequence.
  *reg(GPIO_BASE, 0x94) = 0;
  sd_delay_us(2);
  // GPIO48..53 are controlled by GPPUDCLK1, bits 16..21.  GPPUDCLK0
  // controls GPIO0..31 and was incorrectly selecting GPIO16..21 here.
  *reg(GPIO_BASE, 0x9c) = 0x003f0000U;
  sd_delay_us(2);
  *reg(GPIO_BASE, 0x94) = 0;
  *reg(GPIO_BASE, 0x9c) = 0;
}

static uint32
clock_divider(uint32 base, uint32 target)
{
  uint32 divisor = (base + target - 1) / target;
  uint32 power = 1;
  while(power < divisor && power < 0x400)
    power <<= 1;
  if(power > 0x3ff)
    power = 0x3ff;
  return ((power & 0xff) << 8) | ((power & 0x300) >> 2);
}

static int
set_clock(uint32 target)
{
  uint32 cap0 = rd(EMMC_CAP0);
  uint32 base_mhz = (cap0 >> 8) & 0xff;
  // Prefer the firmware value on both QEMU and hardware.  Besides being the
  // authoritative Pi clock source, this keeps the mailbox path testable in
  // QEMU instead of silently bypassing it when CAP0 happens to be populated.
  uint32 base = firmware_emmc_clock();
  if(base == 0 && base_mhz)
    base = base_mhz * 1000000U;
  if(base == 0){
    // On the Pi 3 firmware/armstub combination used by the real board the
    // property mailbox does not reply and CAP0 reports zero.  The firmware
    // runs the Arasan EMMC base clock at 250 MHz; use that known platform
    // value instead of the old, incorrect 100 MHz guess.
    base = 250000000U;
    printf("sd: no firmware/CAP0 clock; using Pi3 EMMC base=%d Hz\n", base);
  }

  if(wait_mask(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 0, 1000000) < 0){
    printf("sd: clock inhibit target=%d status=%x control1=%x\n",
           target, rd(EMMC_STATUS), rd(EMMC_CONTROL1));
    return -1;
  }

  uint32 c1 = rd(EMMC_CONTROL1);
  c1 &= ~C1_CLK_EN;
  wr(EMMC_CONTROL1, c1);
  sd_delay_us(10);

  c1 &= ~0xffe0U;
  c1 |= clock_divider(base, target) | C1_CLK_INTLEN;
  wr(EMMC_CONTROL1, c1);
  if(wait_mask(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 1000000) < 0){
    printf("sd: clock unstable target=%d base=%d cap0=%x status=%x control1=%x\n",
           target, base, cap0, rd(EMMC_STATUS), rd(EMMC_CONTROL1));
    return -1;
  }

  wr(EMMC_CONTROL1, c1 | C1_CLK_EN);
  sd_delay_us(1000);
  return 0;
}

static int
wait_interrupt(uint32 wanted, uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  do {
    uint32 irq = rd(EMMC_INTERRUPT);
    if(irq & (INT_ERROR | INT_ERROR_MASK)){
      // Preserve the error bits until the caller prints the complete host
      // state. The next command normally clears all interrupts, but a failed
      // xv6 block operation panics first.
      return -1;
    }
    if(irq & wanted){
      wr(EMMC_INTERRUPT, wanted);
      return 0;
    }
  } while(r_cntvct_el0() - start < ticks);
  return -1;
}

static int
send_command(uint32 cmd, uint32 arg, uint32 *response)
{
  uint32 inhibit = SR_CMD_INHIBIT;
  if((cmd & CMD_ISDATA) || ((cmd >> 16) & 3) == 3)
    inhibit |= SR_DAT_INHIBIT;
  if(wait_mask(EMMC_STATUS, inhibit, 0, 1000000) < 0)
    return -1;

  wr(EMMC_INTERRUPT, 0xffffffffU);
  wr(EMMC_ARG1, arg);
  wr(EMMC_CMDTM, cmd);
  if(wait_interrupt(INT_CMD_DONE, 1000000) < 0)
    return -1;
  if(response)
    *response = rd(EMMC_RESP0);
  return 0;
}

static int
send_app_command(uint32 cmd, uint32 arg, uint32 *response)
{
  uint32 ignored;
  if(send_command(CMD55, sd_rca << 16, &ignored) < 0)
    return -1;
  return send_command(cmd, arg, response);
}

static int
transfer_sector(uint32 sector, uchar *data, int write)
{
  uint32 arg = sd_sdhc ? sector : sector * SD_SECTOR_SIZE;
  sd_io_phase = 1; // command
  wr(EMMC_BLKSIZECNT, SD_SECTOR_SIZE | (1U << 16));
  if(send_command(write ? CMD24 : CMD17, arg, 0) < 0)
    return -1;

  sd_io_phase = 2; // buffer-ready interrupt
  uint32 ready = write ? INT_WRITE_RDY : INT_READ_RDY;
  if(wait_interrupt(ready, 1000000) < 0)
    return -1;

  sd_io_phase = 3; // PIO FIFO
  uint32 *words = (uint32 *)data;
  for(int i = 0; i < SD_SECTOR_SIZE / 4; i++){
    if(write){
      if(wait_mask(EMMC_STATUS, SR_WRITE_AVAIL, SR_WRITE_AVAIL, 1000000) < 0)
        return -1;
      wr(EMMC_DATA, words[i]);
    } else {
      if(wait_mask(EMMC_STATUS, SR_READ_AVAIL, SR_READ_AVAIL, 1000000) < 0)
        return -1;
      words[i] = rd(EMMC_DATA);
    }
  }

  sd_io_phase = 4; // transfer-complete interrupt
  if(wait_interrupt(INT_DATA_DONE, 1000000) < 0)
    return -1;
  sd_io_phase = 0;
  return 0;
}

// Recover the Arasan command/data state machines after a timeout or CRC
// error.  Without these resets STATUS can retain CMD/DAT_INHIBIT and every
// later request fails even though the card itself is still selected.
static int
recover_io(void)
{
  wr(EMMC_INTERRUPT, 0xffffffffU);
  wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
  if(wait_mask(EMMC_CONTROL1, C1_SRST_CMD | C1_SRST_DATA,
               0, 1000000) < 0)
    return -1;
  wr(EMMC_INTERRUPT, 0xffffffffU);
  sd_delay_us(1000);
  return 0;
}

void
sdinit(void)
{
  initlock(&sdlock, "sd");
  sd_gpio_init();

  wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_HC);
  if(wait_mask(EMMC_CONTROL1, C1_SRST_HC, 0, 1000000) < 0)
    panic("sd: host reset");

  wr(EMMC_CONTROL2, 0);
  wr(EMMC_IRPT_EN, 0);        // polling driver
  wr(EMMC_IRPT_MASK, 0xffffffffU);
  wr(EMMC_INTERRUPT, 0xffffffffU);
  wr(EMMC_CONTROL1, (rd(EMMC_CONTROL1) & ~(0xfU << 16)) | (0xeU << 16));

  if(set_clock(SD_INIT_CLOCK) < 0)
    panic("sd: init clock");
  sd_delay_us(2000);

  if(send_command(CMD0, 0, 0) < 0)
    panic("sd: CMD0");

  uint32 response = 0;
  int v2 = send_command(CMD8, 0x1aa, &response) == 0 &&
           (response & 0xfff) == 0x1aa;
  if(!v2){
    wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_CMD);
    wait_mask(EMMC_CONTROL1, C1_SRST_CMD, 0, 1000000);
    wr(EMMC_INTERRUPT, 0xffffffffU);
  }

  uint32 ocr = 0;
  for(int tries = 0; tries < 1000; tries++){
    uint32 arg = 0x00ff8000U | (v2 ? (1U << 30) : 0);
    if(send_app_command(CMD41, arg, &ocr) == 0 && (ocr & (1U << 31)))
      break;
    sd_delay_us(1000);
  }
  if((ocr & (1U << 31)) == 0)
    panic("sd: ACMD41");
  sd_sdhc = (ocr & (1U << 30)) != 0;

  if(send_command(CMD2, 0, 0) < 0)
    panic("sd: CMD2");
  if(send_command(CMD3, 0, &response) < 0)
    panic("sd: CMD3");
  sd_rca = response >> 16;
  if(sd_rca == 0)
    panic("sd: no RCA");
  if(send_command(CMD7, sd_rca << 16, &response) < 0)
    panic("sd: CMD7");
  if(!sd_sdhc && send_command(CMD16, SD_SECTOR_SIZE, &response) < 0)
    panic("sd: CMD16");
  if(set_clock(SD_NORMAL_CLOCK) < 0)
    panic("sd: normal clock");

  sd_ready = 1;
  printf("sd: card initialized (%s)\n", sd_sdhc ? "SDHC" : "SDSC");
}

int
sdsector(uint32 sector, void *buffer, int write)
{
  if(!sd_ready)
    return -1;

  acquire(&sdlock);
  int result = -1;
  for(int attempt = 1; attempt <= 3; attempt++){
    result = transfer_sector(sector, (uchar *)buffer, write);
    if(result == 0)
      break;

    uint32 status = rd(EMMC_STATUS);
    uint32 irq = rd(EMMC_INTERRUPT);
    uint32 control1 = rd(EMMC_CONTROL1);
    printf("sd: I/O retry lba=%d write=%d attempt=%d phase=%d status=%x irq=%x control1=%x\n",
           sector, write, attempt, sd_io_phase, status, irq, control1);
    if(recover_io() < 0){
      printf("sd: recovery reset timeout control1=%x\n", rd(EMMC_CONTROL1));
      break;
    }
  }
  if(result < 0){
    printf("sd: I/O failure lba=%d write=%d status=%x irq=%x control1=%x\n",
           sector, write, rd(EMMC_STATUS), rd(EMMC_INTERRUPT),
           rd(EMMC_CONTROL1));
  }
  release(&sdlock);
  return result;
}
