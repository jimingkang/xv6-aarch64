#ifndef XV6_SDIO_H
#define XV6_SDIO_H

#include "device.h"

#define MMC_MAX_HOSTS       2
#define SDIO_MAX_FUNCS      7
#define SDIO_ANY_ID         0xffff

#define SDIO_VENDOR_BROADCOM       0x02d0
#define SDIO_DEVICE_BCM43430       0xa9a6
#define SDIO_DEVICE_BCM43455       0xa9bf

#define MMC_RSP_NONE        0
#define MMC_RSP_R1          1
#define MMC_RSP_R4          4
#define MMC_RSP_R5          5
#define MMC_RSP_R6          6

#define MMC_DATA_READ       1
#define MMC_DATA_WRITE      2

struct mmc_host;
struct sdio_card;

struct mmc_command {
  uint32 opcode;
  uint32 arg;
  uint32 response[4];
  int response_type;
};

struct mmc_data {
  void *buffer;
  uint32 block_size;
  uint32 blocks;
  int flags;
};

struct mmc_request {
  struct mmc_command cmd;
  struct mmc_data *data;
};

struct mmc_host_ops {
  int (*request)(struct mmc_host *host, struct mmc_request *mrq);
  int (*set_clock)(struct mmc_host *host, uint32 hz);
  int (*set_bus_width)(struct mmc_host *host, int width);
};

struct mmc_host {
  struct device dev;
  char *name;
  struct device *parent;
  const struct mmc_host_ops *ops;
  void *private;
  uint32 ocr;
  uint16 rca;
  int registered;
  struct sdio_card *card;
  int function_count;
  struct sdio_func *functions[SDIO_MAX_FUNCS];
};

struct sdio_card {
  struct device dev;
  struct mmc_host *host;
  uint32 ocr;
  uint16 rca;
};

struct sdio_func {
  struct device dev;
  struct mmc_host *host;
  uint16 vendor;
  uint16 device;
  uint8 function;
  uint8 class;
  uint16 block_size;
  uint32 cis;
  struct sdio_card *card;
};

struct sdio_device_id {
  uint16 vendor;
  uint16 device;
  uint8 function;
};

struct sdio_driver {
  struct device_driver driver;
  const struct sdio_device_id *id_table;
  int (*probe)(struct sdio_func *func);
  void (*remove)(struct sdio_func *func);
};

extern struct bus_type sdio_bus;

void sdio_bus_init(void);
int sdio_bus_exit(void);
int mmc_add_host(struct mmc_host *host);
int mmc_remove_host(struct mmc_host *host);
int sdio_scan_host(struct mmc_host *host);
int sdio_register_driver(struct sdio_driver *driver);
int sdio_unregister_driver(struct sdio_driver *driver);
int sdio_cmd52(struct sdio_func *func, int write, uint32 addr, uint8 *value);
int sdio_cmd53(struct sdio_func *func, int write, uint32 addr,
               int increment, void *buffer, int len);
int sdio_enable_func(struct sdio_func *func);

#endif
