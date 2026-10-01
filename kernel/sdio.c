// Minimal MMC/SDIO core.  Host-controller drivers provide mmc_host_ops;
// this layer enumerates functions and binds them to drivers on sdio_bus.

#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "device.h"
#include "sdio.h"

#define CMD0   0
#define CMD3   3
#define CMD5   5
#define CMD7   7
#define CMD52 52
#define CMD53 53

#define CCCR_IO_ENABLE       0x02
#define CCCR_IO_READY        0x03
#define CCCR_INT_ENABLE      0x04
#define CCCR_BUS_INTERFACE   0x07
#define FBR_BASE(fn)         ((fn) * 0x100)
#define FBR_CLASS(fn)        (FBR_BASE(fn) + 0x00)
#define FBR_CIS(fn)          (FBR_BASE(fn) + 0x09)
#define CISTPL_END           0xff
#define CISTPL_MANFID        0x20

#define SDIO_CONTAINER(ptr, type, member) \
  ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

static struct mmc_host *hosts[MMC_MAX_HOSTS];
static int nhost;
static struct sdio_card cards[MMC_MAX_HOSTS];
static struct sdio_func functions[MMC_MAX_HOSTS][SDIO_MAX_FUNCS];
static char host_names[MMC_MAX_HOSTS][DEVICE_NAME_MAX];
static char card_names[MMC_MAX_HOSTS][DEVICE_NAME_MAX];
static char function_names[MMC_MAX_HOSTS][SDIO_MAX_FUNCS][DEVICE_NAME_MAX];
static struct bus_type mmc_bus = {
  .name = "mmc",
};

static struct sdio_func *
find_card_func(struct mmc_host *host, uint16 vendor, uint16 device)
{
  for(int i = 0; i < host->function_count; i++){
    struct sdio_func *func = host->functions[i];
    if(func && func->vendor == vendor && func->device == device)
      return func;
  }
  return 0;
}

static int
sdio_match(struct device *dev, struct device_driver *driver)
{
  struct sdio_func *func = SDIO_CONTAINER(dev, struct sdio_func, dev);
  struct sdio_driver *sdrv = SDIO_CONTAINER(driver, struct sdio_driver, driver);
  const struct sdio_device_id *id;

  if(sdrv->id_table == 0)
    return 0;
  for(id = sdrv->id_table; id->vendor || id->device || id->function; id++){
    if((id->vendor == SDIO_ANY_ID || id->vendor == func->vendor) &&
       (id->device == SDIO_ANY_ID || id->device == func->device) &&
       (id->function == 0 || id->function == func->function))
      return 1;
  }
  return 0;
}

static int
sdio_probe(struct device *dev)
{
  struct sdio_func *func = SDIO_CONTAINER(dev, struct sdio_func, dev);
  struct sdio_driver *sdrv;
  if(dev->driver == 0)
    return -1;
  sdrv = SDIO_CONTAINER(dev->driver, struct sdio_driver, driver);
  return sdrv->probe ? sdrv->probe(func) : 0;
}

static void
sdio_remove(struct device *dev)
{
  struct sdio_func *func = SDIO_CONTAINER(dev, struct sdio_func, dev);
  struct sdio_driver *sdrv;
  if(dev->driver == 0)
    return;
  sdrv = SDIO_CONTAINER(dev->driver, struct sdio_driver, driver);
  if(sdrv->remove)
    sdrv->remove(func);
}

struct bus_type sdio_bus = {
  .name = "sdio",
  .match = sdio_match,
  .probe = sdio_probe,
  .remove = sdio_remove,
};

void
sdio_bus_init(void)
{
  memset(hosts, 0, sizeof(hosts));
  memset(cards, 0, sizeof(cards));
  memset(functions, 0, sizeof(functions));
  nhost = 0;
  bus_register(&mmc_bus);
  bus_register(&sdio_bus);
}

int
sdio_bus_exit(void)
{
  if(nhost != 0)
    return -1;
  if(bus_unregister(&sdio_bus) < 0)
    return -1;
  return bus_unregister(&mmc_bus);
}

static int
mmc_command(struct mmc_host *host, uint32 opcode, uint32 arg,
            int response_type, uint32 *response)
{
  struct mmc_request mrq;
  memset(&mrq, 0, sizeof(mrq));
  mrq.cmd.opcode = opcode;
  mrq.cmd.arg = arg;
  mrq.cmd.response_type = response_type;
  if(host == 0 || host->ops == 0 || host->ops->request == 0 ||
     host->ops->request(host, &mrq) < 0)
    return -1;
  if(response)
    *response = mrq.cmd.response[0];
  return 0;
}

static int
cmd52_host(struct mmc_host *host, int function, int write, uint32 addr,
           uint8 *value)
{
  uint32 response, arg;
  if(function < 0 || function > 7 || addr > 0x1ffff || value == 0)
    return -1;
  arg = (write ? 1U << 31 : 0) | ((uint32)function << 28) |
        ((addr & 0x1ffff) << 9) | (write ? *value : 0);
  if(mmc_command(host, CMD52, arg, MMC_RSP_R5, &response) < 0 ||
     (response & 0xcb00))
    return -1;
  *value = response & 0xff;
  return 0;
}

int
sdio_cmd52(struct sdio_func *func, int write, uint32 addr, uint8 *value)
{
  if(func == 0)
    return -1;
  return cmd52_host(func->host, func->function, write, addr, value);
}

int
sdio_cmd53(struct sdio_func *func, int write, uint32 addr,
           int increment, void *buffer, int len)
{
  struct mmc_request mrq;
  struct mmc_data data;
  uint32 count;
  if(func == 0 || buffer == 0 || len <= 0 || len > 512 || addr > 0x1ffff)
    return -1;
  count = len == 512 ? 0 : len;
  memset(&mrq, 0, sizeof(mrq));
  memset(&data, 0, sizeof(data));
  mrq.cmd.opcode = CMD53;
  mrq.cmd.response_type = MMC_RSP_R5;
  mrq.cmd.arg = (write ? 1U << 31 : 0) |
                ((uint32)func->function << 28) |
                (increment ? 1U << 26 : 0) |
                ((addr & 0x1ffff) << 9) | count;
  data.buffer = buffer;
  data.block_size = len;
  data.blocks = 1;
  data.flags = write ? MMC_DATA_WRITE : MMC_DATA_READ;
  mrq.data = &data;
  if(func->host->ops->request(func->host, &mrq) < 0 ||
     (mrq.cmd.response[0] & 0xcb00))
    return -1;
  return 0;
}

int
sdio_enable_func(struct sdio_func *func)
{
  uint8 enable, ready;
  uint64 start, timeout;
  if(func == 0 || cmd52_host(func->host, 0, 0, CCCR_IO_ENABLE, &enable) < 0)
    return -1;
  enable |= 1U << func->function;
  if(cmd52_host(func->host, 0, 1, CCCR_IO_ENABLE, &enable) < 0)
    return -1;
  start = r_cntvct_el0();
  timeout = (uint64)r_cntfrq_el0() * 3;
  do {
    if(cmd52_host(func->host, 0, 0, CCCR_IO_READY, &ready) == 0 &&
       (ready & (1U << func->function)))
      return 0;
    asm volatile("yield" ::: "memory");
  } while(r_cntvct_el0() - start < timeout);
  return -1;
}

int
sdio_claim_irq(struct sdio_func *func)
{
  uint8 enable;
  if(func == 0 || cmd52_host(func->host, 0, 0,
                             CCCR_INT_ENABLE, &enable) < 0)
    return -1;
  enable |= 1U | (1U << func->function); // master + this function
  return cmd52_host(func->host, 0, 1, CCCR_INT_ENABLE, &enable);
}

void
sdio_release_irq(struct sdio_func *func)
{
  uint8 enable;
  if(func == 0 || cmd52_host(func->host, 0, 0,
                             CCCR_INT_ENABLE, &enable) < 0)
    return;
  enable &= ~(1U << func->function);
  if((enable & 0xfe) == 0)
    enable &= ~1U;
  cmd52_host(func->host, 0, 1, CCCR_INT_ENABLE, &enable);
}

static int
read_cis_id(struct mmc_host *host, int function, uint32 cis,
            uint16 *vendor, uint16 *device)
{
  uint8 tuple, len, value[4];
  for(int n = 0; n < 256; n++){
    if(cmd52_host(host, 0, 0, cis++, &tuple) < 0)
      return -1;
    if(tuple == CISTPL_END)
      break;
    if(tuple == 0)
      continue;
    if(cmd52_host(host, 0, 0, cis++, &len) < 0)
      return -1;
    if(tuple == CISTPL_MANFID && len >= 4){
      for(int i = 0; i < 4; i++)
        if(cmd52_host(host, 0, 0, cis + i, &value[i]) < 0)
          return -1;
      *vendor = value[0] | ((uint16)value[1] << 8);
      *device = value[2] | ((uint16)value[3] << 8);
      return 0;
    }
    cis += len;
  }
  (void)function;
  return -1;
}

static void
make_func_name(char *name, int host_index, int function)
{
  // Stable Linux-like name: sdio0:1, sdio0:2, ...
  name[0] = 's'; name[1] = 'd'; name[2] = 'i'; name[3] = 'o';
  name[4] = '0' + host_index;
  name[5] = ':';
  name[6] = '0' + function;
  name[7] = 0;
}

static void
make_mmc_name(char *name, int host_index, int card)
{
  name[0] = 'm'; name[1] = 'm'; name[2] = 'c';
  name[3] = '0' + host_index;
  if(card){
    name[4] = ':'; name[5] = '0'; name[6] = '0'; name[7] = '0'; name[8] = '1';
    name[9] = 0;
  } else {
    name[4] = 0;
  }
}

int
sdio_scan_host(struct mmc_host *host)
{
  uint32 response, ocr;
  uint8 byte;
  int host_index = -1, count, ready = 0;

  for(int i = 0; i < nhost; i++)
    if(hosts[i] == host)
      host_index = i;
  if(host_index < 0)
    return -1;

  mmc_command(host, CMD0, 0, MMC_RSP_NONE, 0);
  if(mmc_command(host, CMD5, 0, MMC_RSP_R4, &ocr) < 0)
    return -1;
  count = (ocr >> 28) & 7;
  if(count == 0)
    return -1;
  // Keep issuing the operational CMD5 until the card sets IORDY (bit 31).
  // The first answer also tells us how many I/O functions the card exposes.
  for(int retry = 0; retry < 1000; retry++){
    if(mmc_command(host, CMD5, ocr & 0x00ffffff,
                   MMC_RSP_R4, &response) < 0)
      return -1;
    if(response & (1U << 31)){
      ready = 1;
      break;
    }
  }
  if(!ready)
    return -1;
  if(mmc_command(host, CMD3, 0, MMC_RSP_R6, &response) < 0)
    return -1;
  host->rca = response >> 16;
  host->ocr = ocr;
  if(mmc_command(host, CMD7, (uint32)host->rca << 16, MMC_RSP_R1, 0) < 0)
    return -1;

  if(cmd52_host(host, 0, 0, CCCR_BUS_INTERFACE, &byte) == 0){
    byte = (byte & ~3U) | 2U;
    cmd52_host(host, 0, 1, CCCR_BUS_INTERFACE, &byte);
    if(host->ops->set_bus_width)
      host->ops->set_bus_width(host, 4);
  }

  if(count > SDIO_MAX_FUNCS)
    count = SDIO_MAX_FUNCS;
  host->card = &cards[host_index];
  memset(host->card, 0, sizeof(*host->card));
  make_mmc_name(card_names[host_index], host_index, 1);
  host->card->dev.name = card_names[host_index];
  host->card->dev.id = host_index;
  host->card->dev.parent = &host->dev;
  host->card->dev.bus = &mmc_bus;
  host->card->host = host;
  host->card->ocr = host->ocr;
  host->card->rca = host->rca;
  if(device_register(&host->card->dev) < 0){
    host->card = 0;
    return -1;
  }
  host->function_count = count;
  {
    struct sdio_func *func = 0;
    for(int fn = 1; fn <= count; fn++){
      struct sdio_func *f = &functions[host_index][fn - 1];
      uint32 cis = 0;
      memset(f, 0, sizeof(*f));
      f->host = host;
      f->function = fn;
      f->block_size = 64;
      f->card = host->card;
      if(cmd52_host(host, 0, 0, FBR_CLASS(fn), &f->class) < 0)
        continue;
      for(int i = 0; i < 3; i++){
        if(cmd52_host(host, 0, 0, FBR_CIS(fn) + i, &byte) < 0)
          break;
        cis |= (uint32)byte << (8 * i);
      }
      f->cis = cis;
      if(read_cis_id(host, fn, cis, &f->vendor, &f->device) < 0)
        continue;
      make_func_name(function_names[host_index][fn - 1], host_index, fn);
      f->dev.name = function_names[host_index][fn - 1];
      f->dev.id = fn;
      f->dev.parent = &host->card->dev;
      f->dev.bus = &sdio_bus;
      host->functions[fn - 1] = f;
      printf("sdio: %s vendor=%x device=%x class=%x\n",
             f->dev.name, f->vendor, f->device, f->class);
      device_register(&f->dev);
    }

    // Verify that the Broadcom functions belong to the card just registered.
    if((func = find_card_func(host, SDIO_VENDOR_BROADCOM, SDIO_DEVICE_BCM43430)) != 0 ||
       (func = find_card_func(host, SDIO_VENDOR_BROADCOM, SDIO_DEVICE_BCM43455)) != 0){
      for(int i = 0; i < host->function_count; i++){
        struct sdio_func *f = host->functions[i];
        if(f && f->vendor == func->vendor && f->device == func->device)
          f->card = host->card;
      }
      printf("sdio: card %s func1/2 grouped as one device vendor=%x device=%x\n",
             func->dev.name, func->vendor, func->device);
    }
  }
  return 0;
}


int
mmc_add_host(struct mmc_host *host)
{
  int index;
  if(host == 0 || host->ops == 0 || host->ops->request == 0 ||
     nhost >= MMC_MAX_HOSTS)
    return -1;
  index = nhost;
  make_mmc_name(host_names[index], index, 0);
  host->dev.name = host_names[index];
  host->dev.id = index;
  host->dev.parent = host->parent;
  host->dev.bus = &mmc_bus;
  if(device_register(&host->dev) < 0)
    return -1;
  host->registered = 1;
  hosts[nhost++] = host;
  return sdio_scan_host(host);
}

int
mmc_remove_host(struct mmc_host *host)
{
  int i, found = -1;
  if(host == 0)
    return -1;
  for(i = 0; i < nhost; i++)
    if(hosts[i] == host){
      found = i;
      break;
    }
  if(found < 0)
    return -1;
  for(i = 0; i < host->function_count; i++){
    if(host->functions[i]){
      device_unregister(&host->functions[i]->dev);
      host->functions[i] = 0;
    }
  }
  host->function_count = 0;
  if(host->card){
    device_unregister(&host->card->dev);
    host->card = 0;
  }
  if(host->registered){
    device_unregister(&host->dev);
    host->registered = 0;
  }
  for(i = found; i + 1 < nhost; i++)
    hosts[i] = hosts[i + 1];
  hosts[--nhost] = 0;
  return 0;
}

int
sdio_register_driver(struct sdio_driver *driver)
{
  if(driver == 0)
    return -1;
  driver->driver.bus = &sdio_bus;
  return driver_register(&driver->driver);
}

int
sdio_unregister_driver(struct sdio_driver *driver)
{
  if(driver == 0)
    return -1;
  return driver_unregister(&driver->driver);
}
