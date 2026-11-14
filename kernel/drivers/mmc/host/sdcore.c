// SD card protocol on the S5PV210 HSMMC (SDHCI 2.0 + Samsung control
// registers), shared by BL1 and the kernel driver (sdhci-s3c.c): card
// identification (CMD0, CMD8, ACMD41, CMD2/3/7, ACMD6), 4-bit 24 MHz, and
// single-block PIO reads/writes. Polled; no allocation, no locks.
// SCLK_MMC must be the 24 MHz crystal (375 kHz identification = /64).
#include "kernel/types.h"
#include "sdcore.h"

#define LOG(m, v) do { if(env->log) env->log(m, v); } while(0)

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
static struct sdc_env *env;
static int sdhc;
static uint32 rca;

static inline uint8  r8(uint32 o)  { return *(volatile uint8*)(base + o); }
static inline uint16 r16(uint32 o) { return *(volatile uint16*)(base + o); }
static inline uint32 r32(uint32 o) { return *(volatile uint32*)(base + o); }
static inline void w8(uint32 o, uint8 v)   { *(volatile uint8*)(base + o) = v; }
static inline void w16(uint32 o, uint16 v) { *(volatile uint16*)(base + o) = v; }
static inline void w32(uint32 o, uint32 v) { *(volatile uint32*)(base + o) = v; }

static uint64
deadline_us(uint32 us)
{
  return env->now() + (uint64)env->ticks_per_us * us;   // no division: BL1 has no helpers
}
static int expired(uint64 d) { return env->now() > d; }
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
      LOG("sd: bus busy before CMD", idx);
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
      if(idx != 8 && idx != 0)                 // CMD8 may legitimately time out (SD 1.x)
        LOG("sd: error interrupt, CMD/errintsts", (uint32)idx << 16 | e);
      return e & 1 ? -2 : -1;                  // -2: command timeout (no card / not supported)
    }
    if(st & INT_CMD_DONE)
      break;
    if(expired(d)){
      LOG("sd: timeout on CMD", idx);
      reset(2 | 4);
      return -1;
    }
  }
  w16(NORINTSTS, INT_CMD_DONE);
  if(resp){
    resp[0] = r32(RSPREG0); resp[1] = r32(RSPREG1);
    resp[2] = r32(RSPREG2); resp[3] = r32(RSPREG3);
  }
  if((flags & 3) == 3){
    // R1b busy: done at Transfer Complete, or (controllers that do not
    // raise it without data) once DAT0 is released high.
    d = deadline_us(500000);
    while(!(r16(NORINTSTS) & (INT_XFER_DONE | INT_ERROR)) && !(r32(PRNSTS) & (1U << 20)))
      if(expired(d)){
        LOG("sd: busy never ended after CMD", idx);
        return -1;
      }
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

int
sdc_init(uintptr regs, struct sdc_env *e)
{
  uint32 r[4];
  base = regs;
  env = e;
  if(reset(1) < 0){                            // reset all
    LOG("sd: controller reset failed", 0);
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
    LOG("sd: clock did not stabilise", 0);
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
      LOG("sd: ACMD41 failed (no card?)", 0);
      return -1;
    }
    if(r[0] & (1U << 31))
      break;
    if(expired(d)){
      LOG("sd: card stayed busy, ocr", r[0]);
      return -1;
    }
    udelay(10000);
  }
  sdhc = (r[0] >> 30) & 1;
  LOG(v2 ? "sd: SD 2.0 card, ocr" : "sd: SD 1.x card, ocr", r[0]);
  if(command(2, 0, RSP_136, r) < 0){
    LOG("sd: CMD2 (ALL_SEND_CID) failed", 0);
    return -1;
  }
  if(command(3, 0, RSP_48, r) < 0){
    LOG("sd: CMD3 (SEND_RELATIVE_ADDR) failed", 0);
    return -1;
  }
  rca = r[0] >> 16;
  if(command(7, rca << 16, RSP_48B, r) < 0){
    LOG("sd: CMD7 (SELECT_CARD) failed, rca", rca);
    return -1;
  }
  if(app_command(6, 2, RSP_48, r) == 0)        // 4-bit bus
    w8(HOSTCTL, r8(HOSTCTL) | 2);
  if(!sdhc)
    command(16, 512, RSP_48, r);
  if(set_clock(1) < 0)                         // 24 MHz
    return -1;
  LOG(sdhc ? "sd: SDHC/SDXC card, 4-bit 24 MHz, rca" : "sd: SDSC card, 4-bit 24 MHz, rca", rca);
  return 0;
}

int
sdc_transfer(uint32 lba, uint32 *buf, int write)
{
  uint32 r[4];
  w16(BLKSIZE, 512);
  w16(BLKCNT, 1);
  w16(TRNMOD, write ? 0 : (1U << 4));         // single block, PIO, read = card to host
  int tr = env->trace > 0;
  if(tr) { env->trace--; LOG(write ? "sd: CMD24, lba" : "sd: CMD17, lba", lba); }
  if(command(write ? 24 : 17, sdhc ? lba : lba * 512, RSP_48 | CMD_DATA, r) < 0)
    return -1;
  if(tr) LOG("sd:   command done, r1", r[0]);
  uint16 want = write ? INT_BUF_WR : INT_BUF_RD;
  uint64 d = deadline_us(500000);
  while(!(r16(NORINTSTS) & (want | INT_ERROR)))
    if(expired(d)){
      LOG("sd: no data buffer, norintsts/prnsts", (uint32)r16(NORINTSTS) << 16 | (r32(PRNSTS) & 0xffff));
      reset(2 | 4);
      return -1;
    }
  if(tr) LOG("sd:   buffer ready, norintsts", r16(NORINTSTS));
  if(r16(NORINTSTS) & INT_ERROR)
    goto err;
  w16(NORINTSTS, want);
  for(int i = 0; i < 128; i++){
    if(write) w32(BDATA, buf[i]);
    else buf[i] = r32(BDATA);
  }
  if(tr) LOG("sd:   data copied, first word", buf[0]);
  d = deadline_us(500000);
  while(!(r16(NORINTSTS) & (INT_XFER_DONE | INT_ERROR)))
    if(expired(d)){
      LOG("sd: no transfer complete, norintsts", r16(NORINTSTS));
      reset(2 | 4);
      return -1;
    }
  if(r16(NORINTSTS) & INT_ERROR)
    goto err;
  w16(NORINTSTS, INT_XFER_DONE);
  if(tr) LOG("sd:   transfer complete", 0);
  return 0;
err:
  LOG(write ? "sd: write error, errintsts" : "sd: read error, errintsts", r16(ERRINTSTS));
  w16(NORINTSTS, 0xffff); w16(ERRINTSTS, 0xffff);
  reset(2 | 4);
  return -1;
}

int sdc_is_sdhc(void) { return sdhc; }
