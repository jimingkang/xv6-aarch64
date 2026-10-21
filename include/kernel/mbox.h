#ifndef XV6_MBOX_H
#define XV6_MBOX_H

// VideoCore firmware property mailbox (channel 8), shared by drivers.

#define MBOX_TAG_GET_FIRMWARE_REV  0x00000001U
#define MBOX_TAG_GET_CLOCK_RATE    0x00030002U
#define MBOX_TAG_GET_GPIO_STATE    0x00030041U
#define MBOX_TAG_GET_GPIO_CONFIG   0x00030043U
#define MBOX_TAG_SET_DOMAIN_STATE  0x00038030U
#define MBOX_TAG_SET_GPIO_STATE    0x00038041U
#define MBOX_TAG_SET_GPIO_CONFIG   0x00038043U

#define MBOX_CLOCK_CORE  4U          // VPU core clock: also clocks BSC (I2C)

// Firmware power domains: Linux RPI_POWER_DOMAIN_* + 1 (raspberrypi-power.c).
#define MBOX_DOMAIN_UNICAM1  14U

// GPIO expander lines driven by the firmware are numbered from 128.
#define MBOX_EXPGPIO(n)      (128U + (n))

// Outcome of the most recent mbox_property(), for diagnostics.
#define MBOX_OK            0
#define MBOX_ERR_TIMEOUT   1   // firmware never answered
#define MBOX_ERR_REQUEST   2   // whole message rejected (status != 0x80000000)
#define MBOX_ERR_TAG       3   // tag response bit was not set; diagnostic only
                               // when the whole property message succeeded
#define MBOX_ERR_STATUS    4   // tag handled but its first word says error

struct mbox_result {
  int err;                     // MBOX_OK or MBOX_ERR_*
  uint32 status;               // message word 1
  uint32 tag_resp;             // tag word 2: 0x80000000 | response length
  uint32 value0;               // first value word of the response
  uint stale;                  // stale/foreign replies discarded so far
};

int    mbox_property(uint32 tag, uint32 *value, int value_bytes);
void   mbox_last_result(struct mbox_result *r);
char  *mbox_strerror(int err);
uint32 mbox_get_firmware_rev(void);
uint32 mbox_get_clock_rate(uint32 clock_id);
int    mbox_set_domain(uint32 domain, int on);
// diag, if not 0, receives the outcome of SET_GPIO_CONFIG and SET_GPIO_STATE.
int    mbox_set_expgpio(uint32 gpio, int value, struct mbox_result diag[2]);
// Read an expander output level.  Returns 0 on success and stores 0/1 in value.
int    mbox_get_expgpio(uint32 gpio, int *value);
int    mbox_get_expgpio_config(uint32 gpio, int *direction, int *polarity);

#endif
