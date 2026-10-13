#ifndef XV6_I2C_H
#define XV6_I2C_H

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
