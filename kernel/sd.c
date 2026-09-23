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
  *reg(GPIO_BASE, 0x98) = 0x003f0000U; // GPIO48..53
  sd_delay_us(2);
  *reg(GPIO_BASE, 0x94) = 0;
  *reg(GPIO_BASE, 0x98) = 0;
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
  uint32 base_mhz = (rd(EMMC_CAP0) >> 8) & 0xff;
  uint32 base = base_mhz ? base_mhz * 1000000U : 100000000U;

  if(wait_mask(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT, 0, 1000000) < 0)
    return -1;

  uint32 c1 = rd(EMMC_CONTROL1);
  c1 &= ~C1_CLK_EN;
  wr(EMMC_CONTROL1, c1);
  sd_delay_us(10);

  c1 &= ~0xffe0U;
  c1 |= clock_divider(base, target) | C1_CLK_INTLEN;
  wr(EMMC_CONTROL1, c1);
  if(wait_mask(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 1000000) < 0)
    return -1;

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
      wr(EMMC_INTERRUPT, irq);
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
  wr(EMMC_BLKSIZECNT, SD_SECTOR_SIZE | (1U << 16));
  if(send_command(write ? CMD24 : CMD17, arg, 0) < 0)
    return -1;

  uint32 ready = write ? INT_WRITE_RDY : INT_READ_RDY;
  if(wait_interrupt(ready, 1000000) < 0)
    return -1;

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

  if(wait_interrupt(INT_DATA_DONE, 1000000) < 0)
    return -1;
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
  int result = transfer_sector(sector, (uchar *)buffer, write);
  release(&sdlock);
  return result;
}
