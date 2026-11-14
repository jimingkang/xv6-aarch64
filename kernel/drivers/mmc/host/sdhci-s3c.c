// S5PV210 HSMMC (SDHCI 2.0 compatible, Samsung "sdhci-s3c") SD card driver
// for the card the board booted from. Provides the block interface the
// FAT32/xv6fs layers use:
//
//   int sdsector(uint32 lba, void *buf, int write)   one 512-byte sector
//
// PIO transfers, polled (callers hold spinlocks), 4-bit bus. The card is
// re-initialised from scratch (CMD0, CMD8, ACMD41, CMD2/3/7, ACMD6), so it
// does not depend on what the iROM left behind. SCLK_MMC is the 24 MHz
// crystal: 375 kHz for identification (/64), 24 MHz for data (/1).
// Register usage follows the S5PV210 manual (HSMMC chapter), the SD
// Physical Layer spec, and U-Boot's s5p_sdhci.c control-register setup.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "sdhci-s3c.h"

#define BLKSIZE    0x04   // 16
#define BLKCNT     0x06   // 16
#define ARGUMENT   0x08
#define TRNMOD     0x0c   // 16
#define CMDREG     0x0e   // 16
#define RSPREG0    0x10
#define RSPREG1    0x14
#define RSPREG2    0x18
#define RSPREG3    0x1c
#define BDATA      0x20
#define PRNSTS     0x24
#define HOSTCTL    0x28   // 8
#define PWRCON     0x29   // 8
#define CLKCON     0x2c   // 16
#define TIMEOUTCON 0x2e   // 8
#define SWRST      0x2f   // 8
#define NORINTSTS  0x30   // 16
#define ERRINTSTS  0x32   // 16
#define NORINTSTSEN 0x34  // 16
#define ERRINTSTSEN 0x36  // 16
#define NORINTSIGEN 0x38  // 16
#define ERRINTSIGEN 0x3a  // 16
#define CONTROL2   0x80
#define CONTROL3   0x84
#define CONTROL4   0x8c

#define INT_CMD_DONE  (1U << 0)
#define INT_XFER_DONE (1U << 1)
#define INT_BUF_WR    (1U << 4)
#define INT_BUF_RD    (1U << 5)
#define INT_ERROR     (1U << 15)
#define PRN_CMD_INHIBIT (1U << 0)
#define PRN_DAT_INHIBIT (1U << 1)

// CMDREG response/flags
#define RSP_NONE   0x00
#define RSP_136    0x09     // R2: 136-bit, CRC
#define RSP_48     0x1a     // R1/R6/R7: 48-bit, CRC, index
#define RSP_48B    0x1b     // R1b: with busy
#define RSP_R3     0x02     // R3: 48-bit, no CRC/index
#define CMD_DATA   (1U << 5)

static uintptr base;
static struct sdhci_s3c_platdata *pdata;
static int ready, sdhc;
static uint32 rca;
static struct spinlock sd_lock;

static inline uint8  r8(uint32 o)  { return *(volatile uint8*)(base + o); }
static inline uint16 r16(uint32 o) { return *(volatile uint16*)(base + o); }
static inline uint32 r32(uint32 o) { return *(volatile uint32*)(base + o); }
static inline void w8(uint32 o, uint8 v)   { *(volatile uint8*)(base + o) = v; }
static inline void w16(uint32 o, uint16 v) { *(volatile uint16*)(base + o) = v; }
static inline void w32(uint32 o, uint32 v) { *(volatile uint32*)(base + o) = v; }

static uint64
deadline_us(uint32 us)
{
  return r_cntvct_el0() + (uint64)r_cntfrq_el0() * us / 1000000;
}
static int expired(uint64 d) { return r_cntvct_el0() > d; }
static void udelay(uint32 us) { uint64 d = deadline_us(us); while(!expired(d)) ; }

static int
reset(uint8 what)
{
  w8(SWRST, what);
  uint64 d = deadline_us(100000);
  while(r8(SWRST) & what)
    if(expired(d))
      return -1;
  return 0;
}

// SD clock = SCLK_MMC (24 MHz) / div, div a power of two (0 = /1).
static int
set_clock(uint32 div)
{
  w16(CLKCON, 0);
  uint16 sel = div > 1 ? div / 2 : 0;           // SDHCI 2.0: field N divides by 2N
  w16(CLKCON, (sel << 8) | 1);                 // internal clock on
  uint64 d = deadline_us(100000);
  while(!(r16(CLKCON) & 2))                    // stable
    if(expired(d))
      return -1;
  w16(CLKCON, r16(CLKCON) | 4);                // SD clock out
  udelay(1000);
  return 0;
}

// Issue a command; returns 0 and fills resp[0..3] (resp[0] for 48-bit).
static int
command(int idx, uint32 arg, uint16 flags, uint32 *resp)
{
  uint32 inhibit = PRN_CMD_INHIBIT | ((flags & CMD_DATA) || (flags & 3) == 3 ? PRN_DAT_INHIBIT : 0);
  uint64 d = deadline_us(100000);
  while(r32(PRNSTS) & inhibit)
    if(expired(d)){
      printf("sd: CMD%d: bus busy prnsts=%x\n", idx, r32(PRNSTS));
      return -1;
    }
  w16(NORINTSTS, 0xffff);
  w16(ERRINTSTS, 0xffff);
  if(!(flags & CMD_DATA))
    w16(TRNMOD, 0);
  w32(ARGUMENT, arg);
  w16(CMDREG, (uint16)(idx << 8) | flags);
  d = deadline_us(500000);
  for(;;){
    uint16 st = r16(NORINTSTS);
    if(st & INT_ERROR){
      uint16 e = r16(ERRINTSTS);
      w16(NORINTSTS, 0xffff); w16(ERRINTSTS, 0xffff);
      reset(2 | 4);                            // CMD and DAT lines
      return e & 1 ? -2 : -1;                  // -2: command timeout (no card / not supported)
    }
    if(st & INT_CMD_DONE)
      break;
    if(expired(d)){
      printf("sd: CMD%d timeout\n", idx);
      reset(2 | 4);
      return -1;
    }
  }
  w16(NORINTSTS, INT_CMD_DONE);
  if(resp){
    resp[0] = r32(RSPREG0); resp[1] = r32(RSPREG1);
    resp[2] = r32(RSPREG2); resp[3] = r32(RSPREG3);
  }
  if((flags & 3) == 3){                        // busy: wait for transfer complete
    d = deadline_us(500000);
    while(!(r16(NORINTSTS) & (INT_XFER_DONE | INT_ERROR)))
      if(expired(d))
        return -1;
    w16(NORINTSTS, INT_XFER_DONE);
  }
  return 0;
}

static int
app_command(int idx, uint32 arg, uint16 flags, uint32 *resp)
{
  uint32 r[4];
  if(command(55, rca << 16, RSP_48, r) < 0)
    return -1;
  return command(idx, arg, flags, resp);
}

static int
card_init(void)
{
  uint32 r[4];
  if(reset(1) < 0){                            // reset all
    printf("sd: controller reset failed\n");
    return -1;
  }
  // Samsung extensions (U-Boot s5p_sdhci_set_control_reg): pad drive,
  // async status clear, command conflict mask, RX feedback clock, clock out
  // hold, base clock = SCLK_MMC (SELBASECLK = 2).
  w32(CONTROL4, 3U << 16);
  w32(CONTROL2, (1U << 31) | (1U << 30) | (1U << 14) | (1U << 8) | (2U << 4));
  w32(CONTROL3, (1U << 15) | (1U << 7));
  w8(PWRCON, 0x0f);                            // 3.3 V, bus power on
  w8(TIMEOUTCON, 0x0e);
  w16(NORINTSTSEN, 0x01ff);
  w16(ERRINTSTSEN, 0x01ff);
  w16(NORINTSIGEN, 0);                         // polled
  w16(ERRINTSIGEN, 0);
  w8(HOSTCTL, 0);                              // 1-bit, normal speed
  if(set_clock(64) < 0){                       // 375 kHz
    printf("sd: clock did not stabilise\n");
    return -1;
  }
  udelay(10000);                               // >= 74 clocks before CMD0

  rca = 0;
  command(0, 0, RSP_NONE, 0);
  udelay(2000);
  int v2 = command(8, 0x1aa, RSP_48, r) == 0 && (r[0] & 0xfff) == 0x1aa;
  uint64 d = deadline_us(2000000);
  for(;;){
    if(app_command(41, (v2 ? (1U << 30) : 0) | 0x00ff8000, RSP_R3, r) < 0){
      printf("sd: ACMD41 failed (no card?)\n");
      return -1;
    }
    if(r[0] & (1U << 31))
      break;
    if(expired(d)){
      printf("sd: card stayed busy (ocr=%x)\n", r[0]);
      return -1;
    }
    udelay(10000);
  }
  sdhc = (r[0] >> 30) & 1;
  if(command(2, 0, RSP_136, r) < 0 || command(3, 0, RSP_48, r) < 0)
    return -1;
  rca = r[0] >> 16;
  if(command(7, rca << 16, RSP_48B, r) < 0)
    return -1;
  if(app_command(6, 2, RSP_48, r) == 0)        // 4-bit bus
    w8(HOSTCTL, r8(HOSTCTL) | 2);
  if(!sdhc)
    command(16, 512, RSP_48, r);
  if(set_clock(1) < 0)                         // 24 MHz
    return -1;
  printf("sd: %s card rca=%x, 4-bit, 24 MHz\n", sdhc ? "SDHC/SDXC" : "SDSC", rca);
  return 0;
}

static int
transfer(uint32 lba, uint32 *buf, int write)
{
  uint32 r[4];
  w16(BLKSIZE, 512);
  w16(BLKCNT, 1);
  w16(TRNMOD, write ? 0 : (1U << 4));         // single block, PIO, read = card to host
  if(command(write ? 24 : 17, sdhc ? lba : lba * 512, RSP_48 | CMD_DATA, r) < 0)
    return -1;
  uint16 want = write ? INT_BUF_WR : INT_BUF_RD;
  uint64 d = deadline_us(500000);
  while(!(r16(NORINTSTS) & (want | INT_ERROR)))
    if(expired(d))
      return -1;
  if(r16(NORINTSTS) & INT_ERROR)
    goto err;
  w16(NORINTSTS, want);
  for(int i = 0; i < 128; i++){
    if(write) w32(BDATA, buf[i]);
    else buf[i] = r32(BDATA);
  }
  d = deadline_us(500000);
  while(!(r16(NORINTSTS) & (INT_XFER_DONE | INT_ERROR)))
    if(expired(d))
      return -1;
  if(r16(NORINTSTS) & INT_ERROR)
    goto err;
  w16(NORINTSTS, INT_XFER_DONE);
  return 0;
err:
  printf("sd: %s lba %d error %x\n", write ? "write" : "read", lba, r16(ERRINTSTS));
  w16(NORINTSTS, 0xffff); w16(ERRINTSTS, 0xffff);
  reset(2 | 4);
  return -1;
}

int
sdsector(uint32 lba, void *buf, int write)
{
  static uint32 bounce[128];
  if(!ready)
    return -1;
  acquire(&sd_lock);
  uint32 *p = ((uintptr)buf & 3) ? bounce : buf;
  if(write && p == bounce)
    memmove(bounce, buf, 512);
  int r = -1;
  for(int attempt = 0; attempt < 3 && r < 0; attempt++)
    r = transfer(lba, p, write);
  if(r == 0 && !write && p == bounce)
    memmove(buf, bounce, 512);
  release(&sd_lock);
  return r;
}

int sd_ready(void) { return ready; }

static int
sdhci_probe(struct device *dev)
{
  base = (uintptr)dev->resource[0].start;
  pdata = dev->driver_data;
  initlock(&sd_lock, "sd");
  if(pdata && pdata->setup)
    pdata->setup();
  printf("sd: hsmmc%d at %x\n", dev->id, (uint)base);
  if(card_init() < 0){
    printf("sd: no usable card\n");
    return 0;     // not fatal: the kernel falls back to its built-in RAM disk
  }
  ready = 1;
  return 0;
}

static struct device_driver sdhci_driver = { .name = "s3c-sdhci", .bus = &platform_bus, .probe = sdhci_probe };
static void sdhci_register(void) { platform_driver_register(&sdhci_driver); }
module_init_ord("sdhci-s3c", sdhci_register, 0);
