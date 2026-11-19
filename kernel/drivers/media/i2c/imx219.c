// Sony IMX219 image sensor (Raspberry Pi Camera v2 / v2.1).
//
// A camsensor (camsensor.h) like ov5647.c: the Unicam bridge drives it only
// through imx219_ops, so the receiver, the DMA slots and /dev/video0 are
// shared with the OV5647 path unchanged.  Register tables follow Linux
// drivers/media/i2c/imx219.c (imx219_common_regs, mode_640_480_regs,
// raw10_framefmt_regs, imx219_start_streaming).  Differences:
//  - the values Linux applies through V4L2 controls (VTS, exposure, analogue
//    and digital gain, flips, test pattern) are written directly at
//    stream_on from a small cached state;
//  - the IMX219 has no on-chip AEC/AGC.  xv6 has no ISP either, so exposure
//    and gain stay at fixed defaults that suit an indoor scene.  They can be
//    changed at run time through /sys/devices/imx219/{exposure,gain} and
//    take effect at the next stream_on, like test_pattern.
//
// Board wiring on the Pi 3B+ is the same as for the v1.3 module: SCCB/CCI on
// GPIO 44/45, here at address 0x10, power enable on firmware expander GPIO 5
// (CAM_GPIO0), 24 MHz oscillator on the camera board, MIPI CSI-2 two data
// lanes, non-continuous clock (Linux imx219-overlay: clock-noncontinuous).
//
// The Pi 3 BSC0 controller is unreliable on this connector (see ov5647.c),
// so every transfer uses the 25 kHz GPIO software I2C on GPIO44/45.  The
// IMX219 accepts both repeated-START and STOP/START register reads, and
// auto-increments the register index for multi-byte writes.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/defs.h>
#include <kernel/mbox.h>
#include <kernel/i2c.h>
#include <kernel/camsensor.h>
#include <kernel/device.h>
#include <kernel/module.h>

#define IMX219_I2C_BUS   0
#define IMX219_ADDR      0x10
#define IMX219_PWR_GPIO  MBOX_EXPGPIO(5)
#define IMX219_CHIP_ID   0x0219

// Registers (Linux imx219.c names)
#define REG_CHIP_ID          0x0000   // 16 bit
#define REG_MODE_SELECT      0x0100
#define   MODE_STANDBY       0x00
#define   MODE_STREAMING     0x01
#define REG_SW_RESET         0x0103
#define REG_CSI_LANE_MODE    0x0114
#define   CSI_2_LANE_MODE    0x01
#define REG_ANALOG_GAIN      0x0157   // gain = 256 / (256 - code)
#define REG_DIGITAL_GAIN     0x0158   // 16 bit, 0x0100 = 1x
#define REG_EXPOSURE         0x015a   // 16 bit, lines
#define REG_VTS              0x0160   // 16 bit, frame length in lines
#define REG_ORIENTATION      0x0172   // bit0 hflip, bit1 vflip
#define REG_TEST_PATTERN     0x0600   // 16 bit

#define ANA_GAIN_MAX         232
#define EXPOSURE_MIN         4
#define EXPOSURE_OFFSET      4        // exposure <= VTS - 4
#define DGAIN_DEFAULT        0x0100

// 640x480 frame timing (Linux: IMX219_VTS_30FPS_640x480, line length 3448
// pixels at 182.4 MHz, so one line is ~18.9 us and a frame ~33 ms).
#define VTS_640x480          0x06e3

struct regval { uint16 addr; uint8 val; };

// imx219_common_regs + the undocumented "Access Addresses 3000-5fff" unlock,
// PLL for a 24 MHz clock on two lanes (912 Mbit/s per lane), line length.
static const struct regval common_regs[] = {
  {0x0100, 0x00},                                   // standby
  {0x30eb, 0x05}, {0x30eb, 0x0c}, {0x300a, 0xff}, {0x300b, 0xff},
  {0x30eb, 0x05}, {0x30eb, 0x09},
  {0x0114, CSI_2_LANE_MODE},
  {0x0128, 0x00},                                   // D-PHY timing auto
  {0x012a, 0x18}, {0x012b, 0x00},                   // EXCK 24 MHz
  {0x0301, 0x05}, {0x0303, 0x01},                   // VTPXCK / VTSYCK div
  {0x0304, 0x03}, {0x0305, 0x03},                   // pre-PLL div
  {0x0306, 0x00}, {0x0307, 0x39},                   // video PLL x57
  {0x030b, 0x01}, {0x030c, 0x00}, {0x030d, 0x72},   // output PLL x114
  {0x455e, 0x00}, {0x471e, 0x4b}, {0x4767, 0x0f}, {0x4750, 0x14},
  {0x4540, 0x00}, {0x47b4, 0x14}, {0x4713, 0x30}, {0x478b, 0x10},
  {0x478f, 0x10}, {0x4793, 0x10}, {0x4797, 0x0e}, {0x479b, 0x0e},
  {0x0162, 0x0d}, {0x0163, 0x78},                   // line length 3448
};

// 640x480: centre crop 1280x960 of the 3280x2464 array, 2x2 analogue
// binning.  Crop starts on even rows/columns, so the Bayer order stays RGGB.
static const struct regval mode_640x480[] = {
  {0x0164, 0x03}, {0x0165, 0xe8},                   // x start 1000
  {0x0166, 0x08}, {0x0167, 0xe7},                   // x end   2279
  {0x0168, 0x02}, {0x0169, 0xf0},                   // y start 752
  {0x016a, 0x06}, {0x016b, 0xaf},                   // y end   1711
  {0x016c, 0x02}, {0x016d, 0x80},                   // output width  640
  {0x016e, 0x01}, {0x016f, 0xe0},                   // output height 480
  {0x0170, 0x01}, {0x0171, 0x01},                   // x/y odd increment
  {0x0174, 0x03}, {0x0175, 0x03},                   // 2x2 analogue binning
  {0x0624, 0x06}, {0x0625, 0x68},                   // test pattern width
  {0x0626, 0x04}, {0x0627, 0xd0},                   // test pattern height
};

// raw10_framefmt_regs: CSI-2 RAW10, 10-bit ADC, op pixel clock /10.
static const struct regval raw10_regs[] = {
  {0x018c, 0x0a}, {0x018d, 0x0a}, {0x0309, 0x0a},
};

struct imx219_mode_regs {
  const struct regval *regs;
  int nregs;
  uint16 vts;
};

static const struct imx219_mode_regs vga_regs = {
  mode_640x480, NELEM(mode_640x480), VTS_640x480,
};

// Only modes whose frame fits CAM_MAX_FRAME_BYTES (512 KB): 640x480 RAW10 is
// 800 bytes x 480 lines, the same buffer shape as the OV5647 VGA mode.
static const struct cam_mode imx219_modes[] = {
  {
    .name = "640x480 RAW10",
    .width = 640, .height = 480,
    .bytesperline = 800,            // 640 * 10 / 8
    .csi_dt = CSI2_DT_RAW10,
    .format = CAM_FMT_SRGGB10P,     // no flips: R G / G B
    .priv = &vga_regs,
  },
};

// 0x0600 values for CAM_CTRL_TEST_PATTERN 0..4 (Linux order): off, colour
// bars, solid colour, grey colour bars, PN9.
static const uint16 test_pattern_val[] = { 0, 2, 1, 3, 4 };

struct imx219_state {
  int test_pattern;                 // cached controls, applied at stream_on
  int exposure;                     // lines
  int gain;                         // analogue gain code 0..232
};

static struct imx219_state imx219_state = {
  .test_pattern = 0,
  .exposure = 1600,                 // ~30 ms of the 33 ms frame
  .gain = 128,                      // 256 / (256 - 128) = 2x
};

static struct i2c_client imx219_client;

static void
delay_ms(int ms)
{
  uint64 end = r_cntvct_el0() + (uint64)r_cntfrq_el0() * ms / 1000;
  while(r_cntvct_el0() < end)
    asm volatile("yield" ::: "memory");
}

static int
imx219_read(uint16 reg, uint8 *val, int len)
{
  uint8 b[2] = { reg >> 8, reg & 0xff };

  return i2c0_bitbang_write_read(IMX219_ADDR, b, 2, val, len);
}

static int
imx219_write(uint16 reg, uint8 val)
{
  uint8 b[3] = { reg >> 8, reg & 0xff, val };

  for(int attempt = 1; attempt <= 3; attempt++){
    if(i2c0_bitbang_write_read(IMX219_ADDR, b, 3, 0, 0) == 0)
      return 0;
    delay_ms(2);
  }
  printf("imx219: i2c write %x failed\n", reg);
  return -1;
}

// 16-bit register: big-endian, the index auto-increments.
static int
imx219_write16(uint16 reg, uint16 val)
{
  uint8 b[4] = { reg >> 8, reg & 0xff, val >> 8, val & 0xff };

  for(int attempt = 1; attempt <= 3; attempt++){
    if(i2c0_bitbang_write_read(IMX219_ADDR, b, 4, 0, 0) == 0)
      return 0;
    delay_ms(2);
  }
  printf("imx219: i2c write %x failed\n", reg);
  return -1;
}

static int
imx219_write_array(const struct regval *r, int n)
{
  for(int i = 0; i < n; i++)
    if(imx219_write(r[i].addr, r[i].val) < 0)
      return -1;
  return 0;
}

// ---------------------------------------------------------------------------
// sensor_ops
// ---------------------------------------------------------------------------

static int
imx219_stream_off(struct camsensor *s)
{
  (void)s;
  return imx219_write(REG_MODE_SELECT, MODE_STANDBY);
}

// Power the module, check its ID and leave it in software standby, where
// the CSI-2 lanes sit in LP-11.
static int
imx219_power_on(struct camsensor *s)
{
  uint8 id[2];
  int probe;

  (void)s;
  // Clean power edge: XCLR on the v2 board follows CAM_GPIO0.
  (void)mbox_set_expgpio(IMX219_PWR_GPIO, 0, 0);
  delay_ms(50);
  if(mbox_set_expgpio(IMX219_PWR_GPIO, 1, 0) < 0)
    printf("imx219: firmware GPIO %d (camera power) failed\n",
           IMX219_PWR_GPIO);
  // Linux waits 6.2 ms + 32000 xclk after XCLR; clone boards need longer.
  delay_ms(30);
  i2c0_init_camera_pins();

  // One address probe first: with a v1.3 (OV5647) module attached nothing
  // answers here, and this driver gives up without clocking the bus further.
  probe = i2c0_bitbang_probe(IMX219_ADDR);
  if(probe != 0){
    printf("imx219: no reply at i2c0 0x%x (%s)\n", IMX219_ADDR,
           probe == -2 ? "bus stuck" : "NACK");
    goto fail;
  }
  for(int attempt = 1; attempt <= 3; attempt++){
    id[0] = id[1] = 0;
    if(imx219_read(REG_CHIP_ID, id, 2) == 0 &&
       ((id[0] << 8) | id[1]) == IMX219_CHIP_ID){
      printf("imx219: chip-id=%x%x (Sony IMX219) via GPIO SCCB\n",
             id[0], id[1]);
      if(imx219_write(REG_MODE_SELECT, MODE_STANDBY) < 0)
        goto fail;
      return 0;
    }
    printf("imx219: chip-id read %d/3 got %x%x, expected 0219\n",
           attempt, id[0], id[1]);
    delay_ms(5);
  }
fail:
  mbox_set_expgpio(IMX219_PWR_GPIO, 0, 0);
  return -1;
}

static void
imx219_power_off(struct camsensor *s)
{
  (void)imx219_stream_off(s);
  mbox_set_expgpio(IMX219_PWR_GPIO, 0, 0);
}

// Program the mode and the cached controls, then start streaming
// (Linux imx219_start_streaming + __v4l2_ctrl_handler_setup).
static int
imx219_stream_on(struct camsensor *s, const struct cam_mode *mode)
{
  struct imx219_state *st = s->priv;
  const struct imx219_mode_regs *m = mode->priv;
  int exposure = st->exposure;

  if(exposure > m->vts - EXPOSURE_OFFSET)
    exposure = m->vts - EXPOSURE_OFFSET;
  if(exposure < EXPOSURE_MIN)
    exposure = EXPOSURE_MIN;

  printf("imx219: programming %d common + %d mode registers\n",
         NELEM(common_regs), m->nregs);
  if(imx219_write_array(common_regs, NELEM(common_regs)) < 0 ||
     imx219_write_array(m->regs, m->nregs) < 0 ||
     imx219_write_array(raw10_regs, NELEM(raw10_regs)) < 0)
    return -1;
  if(imx219_write16(REG_VTS, m->vts) < 0 ||
     imx219_write16(REG_EXPOSURE, exposure) < 0 ||
     imx219_write(REG_ANALOG_GAIN, st->gain) < 0 ||
     imx219_write16(REG_DIGITAL_GAIN, DGAIN_DEFAULT) < 0 ||
     imx219_write(REG_ORIENTATION, 0x00) < 0 ||
     imx219_write16(REG_TEST_PATTERN, test_pattern_val[st->test_pattern]) < 0)
    return -1;
  if(imx219_write(REG_MODE_SELECT, MODE_STREAMING) < 0)
    return -1;
  printf("imx219: streaming started (exposure %d lines, gain code %d)\n",
         exposure, st->gain);
  return 0;
}

static int
imx219_set_ctrl(struct camsensor *s, int id, int value)
{
  struct imx219_state *st = s->priv;

  if(id == CAM_CTRL_TEST_PATTERN &&
     value >= 0 && value < NELEM(test_pattern_val)){
    st->test_pattern = value;
    return 0;
  }
  return -1;
}

static const struct sensor_ops imx219_ops = {
  .power_on = imx219_power_on,
  .power_off = imx219_power_off,
  .stream_on = imx219_stream_on,
  .stream_off = imx219_stream_off,
  .set_ctrl = imx219_set_ctrl,
};

static struct camsensor imx219_sensor = {
  .name = "IMX219",
  .ops = &imx219_ops,
  .modes = imx219_modes,
  .nmodes = NELEM(imx219_modes),
  .bus = { .data_lanes = 2, .noncontinuous_clk = 1, .vc = 0 },
  .warmup_frames = 5,               // no AE loop: only let the stream settle
  .priv = &imx219_state,
};

// ---------------------------------------------------------------------------
// sysfs: /sys/devices/imx219/{test_pattern,exposure,gain}.  Writes are
// cached and applied at the next stream_on.
// ---------------------------------------------------------------------------

static int
show_int(int v, char *buf, int size)
{
  char tmp[16];
  int n = 0;

  do {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  } while(v && n < (int)sizeof(tmp));
  if(n + 1 >= size)
    return -1;
  for(int i = 0; i < n; i++)
    buf[i] = tmp[n - 1 - i];
  buf[n] = '\n';
  return n + 1;
}

static int
parse_int(char *p, int *out)
{
  int v = 0, any = 0;

  while(*p == ' ' || *p == '\t')
    p++;
  while(*p >= '0' && *p <= '9' && v < 100000){
    v = v * 10 + (*p - '0');
    p++;
    any = 1;
  }
  if(!any)
    return -1;
  *out = v;
  return 0;
}

static int
test_pattern_show(struct device *dev, struct device_attribute *attr, char *buf,
                  int size)
{
  return show_int(imx219_state.test_pattern, buf, size);
}

static int
test_pattern_store(struct device *dev, struct device_attribute *attr, char *buf,
                   int size)
{
  int v;

  if(parse_int(buf, &v) < 0 ||
     imx219_set_ctrl(&imx219_sensor, CAM_CTRL_TEST_PATTERN, v) < 0)
    return -1;
  return size;
}

static int
exposure_show(struct device *dev, struct device_attribute *attr, char *buf,
              int size)
{
  return show_int(imx219_state.exposure, buf, size);
}

static int
exposure_store(struct device *dev, struct device_attribute *attr, char *buf,
               int size)
{
  int v;

  if(parse_int(buf, &v) < 0 || v < EXPOSURE_MIN ||
     v > VTS_640x480 - EXPOSURE_OFFSET)
    return -1;
  imx219_state.exposure = v;
  return size;
}

static int
gain_show(struct device *dev, struct device_attribute *attr, char *buf,
          int size)
{
  return show_int(imx219_state.gain, buf, size);
}

static int
gain_store(struct device *dev, struct device_attribute *attr, char *buf,
           int size)
{
  int v;

  if(parse_int(buf, &v) < 0 || v > ANA_GAIN_MAX)
    return -1;
  imx219_state.gain = v;
  return size;
}

static DEVICE_ATTR_RW(test_pattern);
static DEVICE_ATTR_RW(exposure);
static DEVICE_ATTR_RW(gain);

static struct attribute *imx219_attrs[] = {
  &dev_attr_test_pattern.attr,
  &dev_attr_exposure.attr,
  &dev_attr_gain.attr,
  0,
};

static int
imx219_dev_probe(struct i2c_client *client)
{
  (void)client;
  imx219_sensor.priv = &imx219_state;
  return camsensor_register(&imx219_sensor);
}

static void
imx219_dev_remove(struct i2c_client *client)
{
  (void)client;
  camsensor_unregister(&imx219_sensor);
  imx219_sensor.priv = 0;
}

static const struct i2c_device_id imx219_ids[] = {
  { "imx219", 0 },
  { 0, 0 },
};

static struct i2c_driver imx219_i2c_driver = {
  .driver = { .name = "imx219" },
  .id_table = imx219_ids,
  .probe = imx219_dev_probe,
  .remove = imx219_dev_remove,
};

static void
imx219_init(void)
{
  struct i2c_adapter *adap = i2c_get_adapter(IMX219_I2C_BUS);

  imx219_client.dev.class = "video4linux";
  imx219_client.dev.driver_attrs = imx219_attrs;
  // Register the driver first, as Linux's i2c_add_driver() users do, then add
  // the board-described client.  Besides making probe ordering explicit, this
  // avoids the old short-circuit expression silently skipping driver
  // registration when the adapter/client setup fails.
  if(adap == 0){
    printf("imx219: i2c adapter %d is not registered\n", IMX219_I2C_BUS);
    return;
  }
  if(i2c_add_driver(&imx219_i2c_driver) < 0){
    printf("imx219: failed to register I2C driver\n");
    return;
  }
  if(i2c_register_client(&imx219_client, adap, "imx219", IMX219_ADDR) < 0){
    printf("imx219: failed to register I2C client at %d-0x%x\n",
           IMX219_I2C_BUS, IMX219_ADDR);
    i2c_del_driver(&imx219_i2c_driver);
    return;
  }
  printf("imx219: registered candidate on i2c%d at 0x%x\n",
         IMX219_I2C_BUS, IMX219_ADDR);
}

// Order 4: registers before ov5647 (order 5), so camera_bind() tries the
// IMX219 first.  Its probe is a single address byte at 0x10, so a v1.3
// module costs one short NACK before the OV5647 driver takes over.
module_init_ord("imx219", imx219_init, 4);
