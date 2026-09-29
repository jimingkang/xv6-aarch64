// BCM2837 Arasan SDHCI host used for the on-board SDIO Wi-Fi device.
// The external SD card is owned by bcm2835-sdhost; this controller is routed
// to GPIO34..39 and exported to the small MMC/SDIO core in sdio.c.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "memlayout.h"
#include "device.h"
#include "sdio.h"

#define EMMC_BASE       (PERIPHERAL_BASE + 0x00300000UL)
#define GPIO_BASE       (PERIPHERAL_BASE + 0x00200000UL)
#define MBOX_BASE       (PERIPHERAL_BASE + 0x0000b880UL)

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
#define SR_WRITE_AVAIL  (1U << 10)
#define SR_READ_AVAIL   (1U << 11)

#define C0_4BIT         (1U << 1)
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
#define CMD_RSPNS_48    (2U << 16)
#define CMD_CRCCHK_EN   (1U << 19)
#define CMD_IXCHK_EN    (1U << 20)
#define CMD_ISDATA      (1U << 21)
#define TM_DAT_DIR_CH   (1U << 4)
#define CMD_INDEX(n)    ((uint32)(n) << 24)

#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20
#define MBOX_EMPTY      0x40000000U
#define MBOX_FULL       0x80000000U
#define MBOX_PROP_CH    8U
#define MBOX_GET_CLOCK_RATE 0x00030002U
#define MBOX_CLOCK_EMMC 1U

#define SDIO_INIT_CLOCK 400000U
#define SDIO_DATA_CLOCK 25000000U

struct arasan_host {
  struct mmc_host mmc;
  struct device *dev;
  uint32 base_clock;
};

static struct arasan_host arasan;
static uint32 clock_message[16] __attribute__((aligned(64)));
static struct device *arasan_device;
static struct device_driver *arasan_driver;

static inline volatile uint32 *
reg(uint64 base, uint32 offset)
{
  return (volatile uint32 *)(base + offset);
}

static inline uint32
rd(uint32 offset)
{
  return *reg(EMMC_BASE, offset);
}

static inline void
wr(uint32 offset, uint32 value)
{
  *reg(EMMC_BASE, offset) = value;
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
wait_mask(uint32 offset, uint32 mask, uint32 value, uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  do {
    if((rd(offset) & mask) == value)
      return 0;
  } while(r_cntvct_el0() - start < ticks);
  return -1;
}

static uint32
mailbox_clock(void)
{
  uint32 *m = clock_message;
  memset(m, 0, 64);
  m[0] = 8 * sizeof(uint32);
  m[2] = MBOX_GET_CLOCK_RATE;
  m[3] = 8;
  m[5] = MBOX_CLOCK_EMMC;
  uint32 request = ((uint32)V2P(m) & ~0xfU) | MBOX_PROP_CH;
  asm volatile("dc cvac, %0" :: "r"(m) : "memory");
  asm volatile("dsb sy" ::: "memory");

  uint64 limit = r_cntvct_el0() + r_cntfrq_el0();
  while(*reg(MBOX_BASE, MBOX_STATUS) & MBOX_FULL)
    if(r_cntvct_el0() >= limit)
      return 0;
  *reg(MBOX_BASE, MBOX_WRITE) = request;
  for(;;){
    while(*reg(MBOX_BASE, MBOX_STATUS) & MBOX_EMPTY)
      if(r_cntvct_el0() >= limit)
        return 0;
    uint32 response = *reg(MBOX_BASE, MBOX_READ);
    if((response & 0xf) == MBOX_PROP_CH)
      break;
  }
  asm volatile("dc ivac, %0\n\tdsb sy" :: "r"(m) : "memory");
  if(m[1] != 0x80000000U || (m[4] & 0x80000000U) == 0)
    return 0;
  return m[6];
}

static void
gpio_alt3(int pin)
{
  uint32 offset = (pin / 10) * 4;
  uint32 shift = (pin % 10) * 3;
  volatile uint32 *fsel = reg(GPIO_BASE, offset);
  uint32 value = *fsel;
  value &= ~(7U << shift);
  value |= 7U << shift;
  *fsel = value;
}

static void
gpio_init(void)
{
  for(int pin = 34; pin <= 39; pin++)
    gpio_alt3(pin);
  // No pull on CLK (34), pull-up CMD/DAT (35..39).
  *reg(GPIO_BASE, 0x94) = 2;
  delay_us(2);
  *reg(GPIO_BASE, 0x98) = 0x000000f8U;
  delay_us(2);
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
arasan_set_clock(struct mmc_host *mmc, uint32 hz)
{
  struct arasan_host *host = mmc->private;
  if(wait_mask(EMMC_STATUS, SR_CMD_INHIBIT | SR_DAT_INHIBIT,
               0, 1000000) < 0)
    return -1;
  uint32 c1 = rd(EMMC_CONTROL1) & ~C1_CLK_EN;
  wr(EMMC_CONTROL1, c1);
  delay_us(10);
  c1 &= ~0xffe0U;
  c1 |= clock_divider(host->base_clock, hz) | C1_CLK_INTLEN;
  wr(EMMC_CONTROL1, c1);
  if(wait_mask(EMMC_CONTROL1, C1_CLK_STABLE, C1_CLK_STABLE, 1000000) < 0)
    return -1;
  wr(EMMC_CONTROL1, c1 | C1_CLK_EN);
  delay_us(1000);
  return 0;
}

static int
arasan_set_bus_width(struct mmc_host *mmc, int width)
{
  (void)mmc;
  uint32 c0 = rd(EMMC_CONTROL0);
  if(width == 4)
    c0 |= C0_4BIT;
  else
    c0 &= ~C0_4BIT;
  wr(EMMC_CONTROL0, c0);
  return 0;
}

static int
wait_interrupt(uint32 wanted, uint32 timeout_us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * timeout_us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  do {
    uint32 irq = rd(EMMC_INTERRUPT);
    if(irq & (INT_ERROR | INT_ERROR_MASK))
      return -1;
    if(irq & wanted){
      wr(EMMC_INTERRUPT, wanted);
      return 0;
    }
  } while(r_cntvct_el0() - start < ticks);
  return -1;
}

static int
arasan_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
  (void)mmc;
  struct mmc_command *command = &mrq->cmd;
  struct mmc_data *data = mrq->data;
  uint32 inhibit = SR_CMD_INHIBIT | (data ? SR_DAT_INHIBIT : 0);
  if(wait_mask(EMMC_STATUS, inhibit, 0, 1000000) < 0)
    return -1;

  uint32 cmd = CMD_INDEX(command->opcode);
  switch(command->response_type){
  case MMC_RSP_NONE:
    cmd |= CMD_RSPNS_NONE;
    break;
  case MMC_RSP_R4:
    cmd |= CMD_RSPNS_48;
    break;
  default:
    cmd |= CMD_RSPNS_48 | CMD_CRCCHK_EN | CMD_IXCHK_EN;
    break;
  }
  if(data){
    if(data->block_size == 0 || data->block_size > 512 || data->blocks != 1)
      return -1;
    wr(EMMC_BLKSIZECNT, data->block_size | (data->blocks << 16));
    cmd |= CMD_ISDATA;
    if(data->flags == MMC_DATA_READ)
      cmd |= TM_DAT_DIR_CH;
  }

  wr(EMMC_INTERRUPT, 0xffffffffU);
  wr(EMMC_ARG1, command->arg);
  wr(EMMC_CMDTM, cmd);
  if(wait_interrupt(INT_CMD_DONE, 1000000) < 0)
    goto fail;
  command->response[0] = rd(EMMC_RESP0);

  if(data){
    uint32 ready = data->flags == MMC_DATA_WRITE ? INT_WRITE_RDY : INT_READ_RDY;
    uint32 available = data->flags == MMC_DATA_WRITE ? SR_WRITE_AVAIL : SR_READ_AVAIL;
    if(wait_interrupt(ready, 1000000) < 0)
      goto fail;
    uint32 staging[128];
    memset(staging, 0, sizeof(staging));
    if(data->flags == MMC_DATA_WRITE)
      memmove(staging, data->buffer, data->block_size);
    for(uint32 i = 0; i < (data->block_size + 3) / 4; i++){
      if(wait_mask(EMMC_STATUS, available, available, 1000000) < 0)
        goto fail;
      if(data->flags == MMC_DATA_WRITE)
        wr(EMMC_DATA, staging[i]);
      else
        staging[i] = rd(EMMC_DATA);
    }
    if(wait_interrupt(INT_DATA_DONE, 1000000) < 0)
      goto fail;
    if(data->flags == MMC_DATA_READ)
      memmove(data->buffer, staging, data->block_size);
  }
  return 0;

fail:
  printf("arasan-sdio: request CMD%d failed status=%x irq=%x c1=%x\n",
         command->opcode, rd(EMMC_STATUS), rd(EMMC_INTERRUPT),
         rd(EMMC_CONTROL1));
  wr(EMMC_INTERRUPT, 0xffffffffU);
  wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_CMD | C1_SRST_DATA);
  wait_mask(EMMC_CONTROL1, C1_SRST_CMD | C1_SRST_DATA, 0, 1000000);
  return -1;
}

static const struct mmc_host_ops arasan_ops = {
  .request = arasan_request,
  .set_clock = arasan_set_clock,
  .set_bus_width = arasan_set_bus_width,
};

static int
arasan_probe(struct device *dev)
{
  gpio_init();
  wr(EMMC_CONTROL1, rd(EMMC_CONTROL1) | C1_SRST_HC);
  if(wait_mask(EMMC_CONTROL1, C1_SRST_HC, 0, 1000000) < 0){
    printf("arasan-sdio: host reset failed\n");
    return -1;
  }
  wr(EMMC_CONTROL2, 0);
  wr(EMMC_IRPT_EN, 0);
  wr(EMMC_IRPT_MASK, 0xffffffffU);
  wr(EMMC_INTERRUPT, 0xffffffffU);
  wr(EMMC_CONTROL1, (rd(EMMC_CONTROL1) & ~(0xfU << 16)) | (0xeU << 16));

  memset(&arasan, 0, sizeof(arasan));
  arasan.dev = dev;
  arasan.base_clock = mailbox_clock();
  if(arasan.base_clock == 0){
    uint32 mhz = (rd(EMMC_CAP0) >> 8) & 0xff;
    arasan.base_clock = mhz ? mhz * 1000000U : 200000000U;
  }
  arasan.mmc.name = "mmc1-arasan";
  arasan.mmc.parent = dev;
  arasan.mmc.ops = &arasan_ops;
  arasan.mmc.private = &arasan;
  dev->driver_data = &arasan;
  if(arasan_set_clock(&arasan.mmc, SDIO_INIT_CLOCK) < 0)
    return -1;

  printf("arasan-sdio: host base-clock=%d Hz, probing GPIO34-39\n",
         arasan.base_clock);
  if(mmc_add_host(&arasan.mmc) < 0){
    printf("arasan-sdio: no SDIO device found\n");
    return 0;
  }
  arasan_set_clock(&arasan.mmc, SDIO_DATA_CLOCK);
  return 0;
}

static void
arasan_remove(struct device *dev)
{
  struct arasan_host *host = dev ? dev->driver_data : 0;
  if(host && host->mmc.registered)
    mmc_remove_host(&host->mmc);
  wr(EMMC_IRPT_EN, 0);
  wr(EMMC_IRPT_MASK, 0);
}

void
arasan_sdio_driver_init(void)
{
  static struct device dev = {
    .name = "bcm2837-arasan-sdio",
    .id = 1,
    .resource = {
      { V2P_WO(EMMC_BASE), V2P_WO(EMMC_BASE) + 0xff,
        IORESOURCE_MEM, "Arasan SDHCI/SDIO" },
      { V2P_WO(GPIO_BASE), V2P_WO(GPIO_BASE) + 0xff,
        IORESOURCE_MEM, "GPIO34-39" },
      { V2P_WO(MBOX_BASE), V2P_WO(MBOX_BASE) + 0x3f,
        IORESOURCE_MEM, "property mailbox" },
    },
    .nresource = 3,
  };
  static struct device_driver drv = {
    .name = "bcm2837-arasan-sdio",
    .probe = arasan_probe,
    .remove = arasan_remove,
  };
  arasan_device = &dev;
  arasan_driver = &drv;
  platform_device_register(&dev);
  platform_driver_register(&drv);
}

void
arasan_sdio_driver_exit(void)
{
  if(arasan_driver)
    driver_unregister(arasan_driver);
  if(arasan_device)
    device_unregister(arasan_device);
  arasan_driver = 0;
  arasan_device = 0;
}
