#ifndef XV6_I2C_H
#define XV6_I2C_H

#include <kernel/device.h>

#define I2C_NAME_SIZE 32

struct i2c_adapter;
struct i2c_client;

struct i2c_algorithm {
  int (*write)(struct i2c_adapter *, uint8, const uint8 *, int);
  int (*read)(struct i2c_adapter *, uint8, uint8 *, int);
  int (*write_read)(struct i2c_adapter *, uint8, const uint8 *, int,
                    uint8 *, int);
};

struct i2c_adapter {
  struct device dev;
  char name[I2C_NAME_SIZE];
  int nr;
  const struct i2c_algorithm *algo;
  void *algo_data;
  int registered;
};

struct i2c_client {
  struct device dev;               // first: safe device -> client cast
  char name[I2C_NAME_SIZE];
  struct i2c_adapter *adapter;
  uint16 addr;
  int registered;
  void *data;
};

struct i2c_device_id {
  const char *name;
  uint64 driver_data;
};

struct i2c_driver {
  struct device_driver driver;     // first: safe driver -> i2c_driver cast
  const struct i2c_device_id *id_table;
  int (*probe)(struct i2c_client *);
  void (*remove)(struct i2c_client *);
};

int i2c_add_adapter(struct i2c_adapter *);
int i2c_del_adapter(struct i2c_adapter *);
struct i2c_adapter *i2c_get_adapter(int);
int i2c_register_client(struct i2c_client *, struct i2c_adapter *,
                        const char *, uint16);
int i2c_unregister_client(struct i2c_client *);
int i2c_add_driver(struct i2c_driver *);
int i2c_del_driver(struct i2c_driver *);
int i2c_client_write(struct i2c_client *, const uint8 *, int);
int i2c_client_read(struct i2c_client *, uint8 *, int);
int i2c_client_write_read(struct i2c_client *, const uint8 *, int,
                          uint8 *, int);

// BCM2837 BSC (I2C master), polled.

int i2c0_init_camera_pins(void);              // BSC0 on GPIO 44/45 (ALT1)
// Change the BSC0 bus rate and return the actual rate selected.  This is also
// used by camera probe recovery for modules whose rise time is too slow for
// the nominal 100 kHz bus.
uint32 i2c0_set_rate(uint32 hz);
int i2c_write(int bus, uint8 addr, const uint8 *buf, int len);
int i2c_read(int bus, uint8 addr, uint8 *buf, int len);
int i2c_write_read(int bus, uint8 addr, const uint8 *wbuf, int wlen,
                   uint8 *rbuf, int rlen);    // repeated START, read last
uint32 i2c_last_status(int bus);
void i2c0_diagnostics(uint32 *status, uint32 *divider, uint32 *pins,
                      uint32 *levels);
// Diagnostic only: probe an address by bit-banging GPIO44/45 open-drain.
// Returns 0 for ACK, -1 for NACK, -2 when SCL/SDA cannot be released high.
int i2c0_bitbang_probe(uint8 addr);
// Complete software-I2C transaction on GPIO44/45.  A STOP is emitted between
// write and read, matching the SCCB split-register access accepted by OV5647.
int i2c0_bitbang_write_read(uint8 addr, const uint8 *wbuf, int wlen,
                            uint8 *rbuf, int rlen);

#endif
