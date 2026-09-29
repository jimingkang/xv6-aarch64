// BCM2835/BCM2837 native SDHOST driver.
//
// The native controller drives the external SD card on GPIO48..53, leaving
// the Arasan SDHCI controller available for the on-board BCM43455 SDIO Wi-Fi.
// This xv6 driver deliberately uses polling PIO: it is small, deterministic,
// and preserves the existing sdsector() block-device interface.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "device.h"

#define SDHOST_BASE     (PERIPHERAL_BASE + 0x00202000UL)
#define GPIO_BASE       (PERIPHERAL_BASE + 0x00200000UL)

#define SDCMD           0x00
#define SDARG           0x04
#define SDTOUT          0x08
#define SDCDIV          0x0c
#define SDRSP0          0x10
#define SDHSTS          0x20
#define SDVDD           0x30
#define SDEDM           0x34
#define SDHCFG          0x38
#define SDHBCT          0x3c
#define SDDATA          0x40
#define SDHBLC          0x50

#define CMD_NEW         0x8000
#define CMD_FAIL        0x4000
#define CMD_BUSY        0x0800
#define CMD_NORESP      0x0400
#define CMD_LONGRESP    0x0200
#define CMD_WRITE       0x0080
#define CMD_READ        0x0040

#define HSTS_BUSY       0x400
#define HSTS_BLOCK      0x200
#define HSTS_REW_TO     0x080
#define HSTS_CMD_TO     0x040
#define HSTS_CRC16      0x020
#define HSTS_CRC7       0x010
#define HSTS_FIFO       0x008
#define HSTS_ERROR      (HSTS_REW_TO | HSTS_CMD_TO | HSTS_CRC16 | \
                         HSTS_CRC7 | HSTS_FIFO)
#define HSTS_CLEAR      0x7f8

#define HCFG_WIDE_EXT   (1U << 2)
#define HCFG_WIDE_INT   (1U << 1)
#define HCFG_REL_CMD    (1U << 0)
#define HCFG_SLOW_CARD  (1U << 3)

#define EDM_FSM_MASK    0xf
#define EDM_IDENT       0x0
#define EDM_DATA        0x1
#define EDM_READWAIT    0x4
#define EDM_WRSTART1    0xa
#define EDM_FORCE_DATA  (1U << 19)
#define EDM_RD_SHIFT    14
#define EDM_WR_SHIFT    9
#define EDM_THRESH_MASK 0x1f
#define EDM_FIFO_SHIFT  4
#define EDM_FIFO_MASK   0x1f

#define SD_SECTOR_SIZE  512
#define SDHOST_CLOCK    250000000U
#define SD_INIT_CLOCK   400000U
#define SD_DATA_CLOCK   25000000U

enum response_type {
  RESP_NONE,
  RESP_SHORT,
  RESP_LONG,
  RESP_BUSY,
};

static struct spinlock sdlock;
static uint32 sd_rca;
static int sd_sdhc;
static int sd_ready;

static inline volatile uint32 *
mmio(uint64 base, uint32 offset)
{
  return (volatile uint32 *)(base + offset);
}

static inline uint32
rd(uint32 offset)
{
  return *mmio(SDHOST_BASE, offset);
}

static inline void
wr(uint32 offset, uint32 value)
{
  *mmio(SDHOST_BASE, offset) = value;
  asm volatile("dmb sy" ::: "memory");
}

static void
delay_us(uint32 us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  while(r_cntvct_el0() - start < ticks)
    asm volatile("yield" ::: "memory");
}

static int
wait_cmd_idle(uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  while(rd(SDCMD) & CMD_NEW){
    if(r_cntvct_el0() - start >= ticks)
      return -1;
  }
  return 0;
}

static void
gpio_alt0(int pin)
{
  uint32 offset = (pin / 10) * 4;
  uint32 shift = (pin % 10) * 3;
  volatile uint32 *fsel = mmio(GPIO_BASE, offset);
  uint32 value = *fsel;
  value &= ~(7U << shift);
  value |= 4U << shift;
  *fsel = value;
}

static void
sdhost_gpio_init(void)
{
  for(int pin = 48; pin <= 53; pin++)
    gpio_alt0(pin);

  // BCM2837 legacy pull-control sequence: no pull on clock, pull-up on CMD
  // and DAT0..3.  The firmware normally configured these already, but the
  // kernel must not depend on that boot-time state.
  *mmio(GPIO_BASE, 0x94) = 2;
  delay_us(2);
  *mmio(GPIO_BASE, 0x9c) = 0x003e0000U;
  delay_us(2);
  *mmio(GPIO_BASE, 0x94) = 0;
  *mmio(GPIO_BASE, 0x9c) = 0;
}

static void
set_clock(uint32 hz)
{
  uint32 div = SDHOST_CLOCK / hz;
  if(div < 2)
    div = 2;
  if(SDHOST_CLOCK / div > hz)
    div++;
  div -= 2;
  if(div > 0x7ff)
    div = 0x7ff;
  wr(SDCDIV, div);
  wr(SDTOUT, (SDHOST_CLOCK / (div + 2)) / 2);
  delay_us(1000);
}

static void
reset_host(void)
{
  wr(SDVDD, 0);
  wr(SDCMD, 0);
  wr(SDARG, 0);
  wr(SDTOUT, 0xf00000);
  wr(SDCDIV, 0x7ff);
  wr(SDHSTS, HSTS_CLEAR);
  wr(SDHCFG, 0);
  wr(SDHBCT, 0);
  wr(SDHBLC, 0);

  uint32 edm = rd(SDEDM);
  edm &= ~((EDM_THRESH_MASK << EDM_RD_SHIFT) |
           (EDM_THRESH_MASK << EDM_WR_SHIFT));
  edm |= (4U << EDM_RD_SHIFT) | (4U << EDM_WR_SHIFT);
  wr(SDEDM, edm);
  delay_us(20000);
  wr(SDVDD, 1);
  delay_us(20000);
  set_clock(SD_INIT_CLOCK);
}

static int
send_command(uint32 opcode, uint32 arg, enum response_type response,
             int data_direction, uint32 *result)
{
  if(wait_cmd_idle(1000000) < 0)
    return -1;

  uint32 status = rd(SDHSTS);
  if(status)
    wr(SDHSTS, status);
  wr(SDARG, arg);

  uint32 cmd = opcode & 0x3f;
  if(response == RESP_NONE)
    cmd |= CMD_NORESP;
  else if(response == RESP_LONG)
    cmd |= CMD_LONGRESP;
  else if(response == RESP_BUSY)
    cmd |= CMD_BUSY;
  if(data_direction > 0)
    cmd |= CMD_READ;
  else if(data_direction < 0)
    cmd |= CMD_WRITE;

  wr(SDCMD, cmd | CMD_NEW);
  if(wait_cmd_idle(1000000) < 0)
    return -1;
  cmd = rd(SDCMD);
  status = rd(SDHSTS);
  if((cmd & CMD_FAIL) || (status & HSTS_ERROR))
    return -1;
  if(result)
    *result = rd(SDRSP0);
  return 0;
}

static int
send_app(uint32 opcode, uint32 arg, enum response_type response,
         uint32 *result)
{
  if(send_command(55, sd_rca << 16, RESP_SHORT, 0, 0) < 0)
    return -1;
  return send_command(opcode, arg, response, 0, result);
}

static int
wait_fifo(int write, uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  for(;;){
    uint32 status = rd(SDHSTS);
    uint32 words = (rd(SDEDM) >> EDM_FIFO_SHIFT) & EDM_FIFO_MASK;
    if(status & HSTS_ERROR)
      return -1;
    if((write && words < 16) || (!write && words > 0))
      return 0;
    if(r_cntvct_el0() - start >= ticks)
      return -1;
  }
}

static int
wait_data_idle(uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  for(;;){
    uint32 edm = rd(SDEDM);
    uint32 fsm = edm & EDM_FSM_MASK;
    if(fsm == EDM_IDENT || fsm == EDM_DATA)
      return 0;
    if(fsm == EDM_READWAIT || fsm == EDM_WRSTART1){
      wr(SDEDM, edm | EDM_FORCE_DATA);
      return 0;
    }
    if((rd(SDHSTS) & HSTS_ERROR) || r_cntvct_el0() - start >= ticks)
      return -1;
  }
}

static int
transfer_sector(uint32 sector, uchar *buffer, int write)
{
  uint32 arg = sd_sdhc ? sector : sector * SD_SECTOR_SIZE;
  wr(SDHBCT, SD_SECTOR_SIZE);
  wr(SDHBLC, 1);
  if(send_command(write ? 24 : 17, arg, RESP_SHORT,
                  write ? -1 : 1, 0) < 0)
    return -1;

  uint32 *word = (uint32 *)buffer;
  for(int i = 0; i < SD_SECTOR_SIZE / 4; i++){
    if(wait_fifo(write, 1000000) < 0)
      return -1;
    if(write)
      wr(SDDATA, word[i]);
    else
      word[i] = rd(SDDATA);
  }
  if(wait_data_idle(1000000) < 0)
    return -1;
  uint32 status = rd(SDHSTS);
  if(status & HSTS_ERROR)
    return -1;
  wr(SDHSTS, status);
  return 0;
}

void
sdinit(void)
{
  uint32 response = 0, ocr = 0;
  initlock(&sdlock, "sdhost");
  sdhost_gpio_init();
  reset_host();

  if(send_command(0, 0, RESP_NONE, 0, 0) < 0)
    panic("sdhost: CMD0");
  int v2 = send_command(8, 0x1aa, RESP_SHORT, 0, &response) == 0 &&
           (response & 0xfff) == 0x1aa;
  if(!v2)
    wr(SDHSTS, HSTS_CLEAR);

  for(int retry = 0; retry < 1000; retry++){
    uint32 arg = 0x00ff8000U | (v2 ? (1U << 30) : 0);
    if(send_app(41, arg, RESP_SHORT, &ocr) == 0 && (ocr & (1U << 31)))
      break;
    delay_us(1000);
  }
  if((ocr & (1U << 31)) == 0)
    panic("sdhost: ACMD41");
  sd_sdhc = (ocr & (1U << 30)) != 0;

  if(send_command(2, 0, RESP_LONG, 0, 0) < 0)
    panic("sdhost: CMD2");
  if(send_command(3, 0, RESP_SHORT, 0, &response) < 0)
    panic("sdhost: CMD3");
  sd_rca = response >> 16;
  if(sd_rca == 0)
    panic("sdhost: RCA");
  if(send_command(7, sd_rca << 16, RESP_BUSY, 0, 0) < 0)
    panic("sdhost: CMD7");
  if(!sd_sdhc && send_command(16, SD_SECTOR_SIZE, RESP_SHORT, 0, 0) < 0)
    panic("sdhost: CMD16");

  // In data mode this controller otherwise uses only the low three bits of
  // SDCDIV.  With a 250 MHz core and our divider value 8 those bits are zero,
  // producing an excessive clock and CRC16 failures.  SLOW_CARD forces the
  // full 11-bit identification divider to remain active in data mode.
  uint32 hcfg = HCFG_WIDE_INT | HCFG_REL_CMD | HCFG_SLOW_CARD;
  if(send_app(6, 2, RESP_SHORT, 0) == 0)
    hcfg |= HCFG_WIDE_EXT;
  wr(SDHCFG, hcfg);
  set_clock(SD_DATA_CLOCK);
  sd_ready = 1;
  printf("sdhost: card initialized (%s, %s bus)\n",
         sd_sdhc ? "SDHC" : "SDSC",
         (rd(SDHCFG) & HCFG_WIDE_EXT) ? "4-bit" : "1-bit");
}

int
sdsector(uint32 sector, void *buffer, int write)
{
  if(!sd_ready)
    return -1;
  acquire(&sdlock);
  int result = -1;
  for(int attempt = 1; attempt <= 3; attempt++){
    result = transfer_sector(sector, buffer, write);
    if(result == 0)
      break;
    printf("sdhost: I/O retry lba=%d write=%d attempt=%d cmd=%x hsts=%x edm=%x\n",
           sector, write, attempt, rd(SDCMD), rd(SDHSTS), rd(SDEDM));
    wr(SDHSTS, HSTS_CLEAR);
    uint32 edm = rd(SDEDM);
    wr(SDEDM, edm | EDM_FORCE_DATA);
    delay_us(1000);
  }
  release(&sdlock);
  return result;
}

static int
sdhost_probe(struct device *dev)
{
  (void)dev;
  sdinit();
  return 0;
}

void
sd_driver_init(void)
{
  static struct device dev = {
    .name = "bcm2835-sdhost",
    .id = 0,
    .resource = {
      { V2P_WO(SDHOST_BASE), V2P_WO(SDHOST_BASE) + 0xff,
        IORESOURCE_MEM, "BCM2835 SDHOST" },
      { V2P_WO(GPIO_BASE), V2P_WO(GPIO_BASE) + 0xff,
        IORESOURCE_MEM, "GPIO" },
    },
    .nresource = 2,
  };
  static struct device_driver drv = {
    .name = "bcm2835-sdhost",
    .probe = sdhost_probe,
  };
  platform_device_register(&dev);
  platform_driver_register(&drv);
}
