// OmniVision OV5647 image sensor (Raspberry Pi Camera v1.3).
//
// A camsensor (camsensor.h): the Unicam bridge drives it only through
// ov5647_ops.  Register tables and power/stream sequences follow Linux
// drivers/media/i2c/ov5647.c (ov5647_common_regs, ov5647_640x480_10bpp,
// ov5647_power_on, ov5647_stream_on/off).  Differences:
//  - the values Linux applies through V4L2 controls (HTS, VTS, exposure,
//    gain) are written directly from the mode;
//  - the sensor's own AEC/AGC is switched on (0x3503 = 0), because xv6 has
//    no ISP or user-space control loop to set exposure;
//  - AWB stays off, as in Linux: user space white-balances the raw image.
//
// Board wiring on the Pi 3B+: I2C BSC0 on GPIO 44/45, sensor address 0x36,
// power enable on firmware expander GPIO 5 (CAM_GPIO0), 25 MHz oscillator on
// the camera board, MIPI CSI-2 two data lanes, non-continuous clock.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "mbox.h"
#include "i2c.h"
#include "camsensor.h"

#define OV5647_I2C_BUS   0
#define OV5647_ADDR      0x36
#define OV5647_PWR_GPIO  MBOX_EXPGPIO(5)

#define REG_SW_STANDBY   0x0100
#define REG_SW_RESET     0x0103
#define REG_CHIPID_H     0x300a
#define REG_CHIPID_L     0x300b
#define REG_PAD_OUT      0x300d
#define REG_AEC_AGC      0x3503
#define REG_MIPI_CTRL00  0x4800
#define REG_MIPI_CTRL14  0x4814
#define REG_FRAME_OFF    0x4202
#define REG_TESTPATTERN  0x503d

#define MIPI_CTRL00_CLOCK_LANE_GATE     (1 << 5)
#define MIPI_CTRL00_LINE_SYNC_ENABLE    (1 << 4)
#define MIPI_CTRL00_BUS_IDLE            (1 << 2)
#define MIPI_CTRL00_CLOCK_LANE_DISABLE  (1 << 0)

struct regval { uint16 addr; uint8 val; };

static const struct regval sensor_oe_enable[] = {
  {0x3000, 0x0f}, {0x3001, 0xff}, {0x3002, 0xe4},
};

static const struct regval sensor_oe_disable[] = {
  {0x3000, 0x00}, {0x3001, 0x00}, {0x3002, 0x00},
};

static const struct regval common_regs[] = {
  {0x0100, 0x00}, {0x0103, 0x01}, {0x3034, 0x1a}, {0x3035, 0x21},
  {0x303c, 0x11}, {0x3106, 0xf5}, {0x3827, 0xec}, {0x370c, 0x03},
  {0x5000, 0x06}, {0x5003, 0x08}, {0x5a00, 0x08}, {0x3000, 0x00},
  {0x3001, 0x00}, {0x3002, 0x00}, {0x3016, 0x08}, {0x3017, 0xe0},
  {0x3018, 0x44}, {0x301c, 0xf8}, {0x301d, 0xf0}, {0x3a18, 0x00},
  {0x3a19, 0xf8}, {0x3c01, 0x80}, {0x3b07, 0x0c}, {0x3630, 0x2e},
  {0x3632, 0xe2}, {0x3633, 0x23}, {0x3634, 0x44}, {0x3636, 0x06},
  {0x3620, 0x64}, {0x3621, 0xe0}, {0x3600, 0x37}, {0x3704, 0xa0},
  {0x3703, 0x5a}, {0x3715, 0x78}, {0x3717, 0x01}, {0x3731, 0x02},
  {0x370b, 0x60}, {0x3705, 0x1a}, {0x3f05, 0x02}, {0x3f06, 0x10},
  {0x3f01, 0x0a}, {0x3a08, 0x01}, {0x3a0f, 0x58}, {0x3a10, 0x50},
  {0x3a1b, 0x58}, {0x3a1e, 0x50}, {0x3a11, 0x60}, {0x3a1f, 0x28},
  {0x4001, 0x02}, {0x4000, 0x09}, {0x3503, 0x03},
};

// 640x480, 2x2 binned + skipped from 2560x1920, RAW10, 55 MHz pixel rate.
static const struct regval mode_640x480_10bpp[] = {
  {0x3036, 0x46}, {0x3821, 0x01}, {0x3820, 0x41}, {0x3612, 0x59},
  {0x3618, 0x00}, {0x3814, 0x35}, {0x3815, 0x35}, {0x3708, 0x64},
  {0x3709, 0x52}, {0x3800, 0x00}, {0x3801, 0x10}, {0x3802, 0x00},
  {0x3803, 0x00}, {0x3804, 0x0a}, {0x3805, 0x2f}, {0x3806, 0x07},
  {0x3807, 0x9f}, {0x3808, 0x02}, {0x3809, 0x80}, {0x380a, 0x01},
  {0x380b, 0xe0}, {0x3a09, 0x2e}, {0x3a0a, 0x00}, {0x3a0b, 0xfb},
  {0x3a0d, 0x02}, {0x3a0e, 0x01}, {0x4004, 0x02}, {0x4800, 0x34},
  {0x0100, 0x01},
};

// What Linux's control handler writes for this mode (ov5647_init_controls):
// HTS = 1852, VTS = 504 (~59 fps), exposure 500 lines, analogue gain 32.
static const struct regval mode_640x480_controls[] = {
  {0x380c, 0x07}, {0x380d, 0x3c},           // HTS 1852
  {0x380e, 0x01}, {0x380f, 0xf8},           // VTS 504
  {0x3500, 0x00}, {0x3501, 0x1f}, {0x3502, 0x40},   // exposure 500 << 4
  {0x350a, 0x00}, {0x350b, 0x20},           // gain 32 (2x)
  {0x5001, 0x00},                           // AWB off (raw output)
  {REG_AEC_AGC, 0x00},                      // AEC + AGC on in the sensor
};

struct ov5647_mode_regs {
  const struct regval *regs;        // mode table
  int nregs;
  const struct regval *ctrls;       // what Linux sets through controls
  int nctrls;
};

static const struct ov5647_mode_regs vga_regs = {
  mode_640x480_10bpp, NELEM(mode_640x480_10bpp),
  mode_640x480_controls, NELEM(mode_640x480_controls),
};

// Linux ov5647_modes[]: only modes whose frame fits CAM_MAX_FRAME_BYTES.
// 1296x972 (2x2 binned) would need 1.5 MB; add it here once the bridge's
// DMA area is larger.
static const struct cam_mode ov5647_modes[] = {
  {
    .name = "640x480 RAW10",
    .width = 640, .height = 480,
    .bytesperline = 800,            // 640 * 10 / 8
    .csi_dt = CSI2_DT_RAW10,
    .format = CAM_FMT_SBGGR10P,
    .priv = &vga_regs,
  },
};

// 0x503d values: off, colour bars, colour squares, random data.
static const uint8 test_pattern_val[] = { 0x00, 0x80, 0x82, 0x81 };

struct ov5647_state {
  int test_pattern;                 // cached control, applied at stream_on
};

static struct ov5647_state ov5647_state;
static int separate_read_reported;
// This Pi 3 camera path has repeatedly shown address ACKs in GPIO mode while
// BCM2837 BSC0 leaves the clone OV5647 in a partial SCCB transaction.  Start
// with the known-good software SCCB transport instead of disturbing the
// sensor with several failing BSC transfers before falling back.
static int ov5647_use_bitbang = 1;
static void delay_ms(int ms);

// Read a sensor with a 16-bit register index.  CCI normally uses a repeated
// START, but some third-party camera modules/SCCB bridges only work when the
// register-address write is terminated with STOP first.
static int
sensor_read16(uint8 addr, uint16 reg, uint8 *val, int len)
{
  uint8 b[2] = { reg >> 8, reg & 0xff };

  if(i2c_write_read(OV5647_I2C_BUS, addr, b, 2, val, len) == 0)
    return 0;
  if(i2c_write(OV5647_I2C_BUS, addr, b, 2) == 0 &&
     i2c_read(OV5647_I2C_BUS, addr, val, len) == 0)
    return 1;                       // worked with split STOP/START
  return -1;
}

static int
ov5647_write(uint16 reg, uint8 val)
{
  uint8 b[3] = { reg >> 8, reg & 0xff, val };

  if(ov5647_use_bitbang)
    return i2c0_bitbang_write_read(OV5647_ADDR, b, 3, 0, 0);
  return i2c_write(OV5647_I2C_BUS, OV5647_ADDR, b, 3);
}

static int
ov5647_read(uint16 reg, uint8 *val)
{
  uint8 b[2] = { reg >> 8, reg & 0xff };

  if(ov5647_use_bitbang)
    return i2c0_bitbang_write_read(OV5647_ADDR, b, 2, val, 1);
  int r = sensor_read16(OV5647_ADDR, reg, val, 1);

  if(r >= 0){
    if(r == 1 && !separate_read_reported){
      // SCCB devices commonly accept this form even when I2C Sr fails.
      printf("ov5647: using SCCB split register reads (STOP then START)\n");
      separate_read_reported = 1;
    }
    return 0;
  }
  return -1;
}

// If GPIO bit-banging sees the sensor but BSC0 does not, retry after the pin
// mux transition and progressively lower the BSC clock.  This distinguishes
// an electrical rise-time/clock-source problem from a missing camera.  Keep
// the first working rate for all subsequent register programming.
static int
ov5647_retry_bsc_rates(uint8 *hi, uint8 *lo)
{
  static const uint32 rates[] = { 100000, 50000, 25000, 10000 };

  for(int i = 0; i < NELEM(rates); i++){
    uint32 actual = i2c0_set_rate(rates[i]);
    delay_ms(1);
    *hi = *lo = 0;
    if(ov5647_read(REG_CHIPID_H, hi) == 0 &&
       ov5647_read(REG_CHIPID_L, lo) == 0){
      printf("ov5647: BSC recovered at %d Hz, chip-id=%x%x\n",
             actual, *hi, *lo);
      return 0;
    }
    uint32 now, div;
    i2c0_diagnostics(&now, &div, 0, 0);
    printf("ov5647: BSC retry %d Hz failed status=%x now=%x div=%d\n",
           actual, i2c_last_status(OV5647_I2C_BUS), now, div);
  }
  return -1;
}

static int
ov5647_write_array(const struct regval *r, int n)
{
  for(int i = 0; i < n; i++){
    int result = -1;

    for(int attempt = 1; attempt <= 3; attempt++){
      result = ov5647_write(r[i].addr, r[i].val);
      if(result == 0)
        break;
      delay_ms(2);
    }
    if(result < 0){
      printf("ov5647: i2c write %x failed\n", r[i].addr);
      return -1;
    }
    // Software reset temporarily stops SCCB.  The next table entry must not
    // begin until the internal oscillator and register interface are ready.
    if(r[i].addr == REG_SW_RESET && (r[i].val & 1))
      delay_ms(10);
  }
  return 0;
}

static void
delay_ms(int ms)
{
  uint64 end = r_cntvct_el0() + (uint64)r_cntfrq_el0() * ms / 1000;
  while(r_cntvct_el0() < end)
    asm volatile("yield" ::: "memory");
}

// ---------------------------------------------------------------------------
// sensor_ops
// ---------------------------------------------------------------------------

static int
ov5647_stream_off(struct camsensor *s)
{
  (void)s;
  if(ov5647_write(REG_MIPI_CTRL00, MIPI_CTRL00_CLOCK_LANE_GATE |
                  MIPI_CTRL00_BUS_IDLE | MIPI_CTRL00_CLOCK_LANE_DISABLE) < 0)
    return -1;
  if(ov5647_write(REG_FRAME_OFF, 0x0f) < 0)
    return -1;
  return ov5647_write(REG_PAD_OUT, 0x01);
}

static void
print_mbox(char *what, struct mbox_result *r)
{
  // %p: the kernel printf prints %x as signed.
  printf("ov5647:   %s: %s (status %p, tag response %p = done %d len %d, "
         "first word %d, stale replies %d)\n", what, mbox_strerror(r->err),
         (uint64)r->status, (uint64)r->tag_resp, (r->tag_resp >> 31) & 1,
         r->tag_resp & 0x7fffffff, r->value0, r->stale);
}

// Power the module, check its ID and leave the lanes in LP-11 (stream off).
static int
ov5647_power_on(struct camsensor *s)
{
  struct mbox_result diag[2];
  uint8 hi = 0, lo = 0;
  int powered, power_state = -1, power_dir = -1, power_polarity = -1;
  int software_target_seen = 0;

  // Generate a clean power/reset edge even if boot firmware left the rail on.
  (void)mbox_set_expgpio(OV5647_PWR_GPIO, 0, 0);
  delay_ms(20);                    // allow module rails to discharge fully
  powered = mbox_set_expgpio(OV5647_PWR_GPIO, 1, diag) == 0;
  if(!powered){
    // Report exactly what the firmware said, then probe anyway: the
    // module may already be powered (or the firmware too old for these
    // tags, in which case the I2C probe below tells us more).
    printf("ov5647: firmware GPIO %d (camera power) failed, firmware rev %d\n",
           OV5647_PWR_GPIO, mbox_get_firmware_rev());
    print_mbox("SET_GPIO_CONFIG", &diag[0]);
    print_mbox("SET_GPIO_STATE", &diag[1]);
  }
  if(mbox_get_expgpio(OV5647_PWR_GPIO, &power_state) == 0){
    if(mbox_get_expgpio_config(OV5647_PWR_GPIO, &power_dir,
                               &power_polarity) == 0)
      printf("ov5647: CAM_GPIO0 direction=%s polarity=%s state=%d\n",
             power_dir ? "output" : "input",
             power_polarity ? "inverted" : "normal", power_state);
    else
      printf("ov5647: CAM_GPIO0 power state=%d (config unreadable)\n",
             power_state);
    if(power_state == 0)
      printf("ov5647: warning: camera power line remained low\n");
  }
  delay_ms(100);                    // overlay needs 20 ms; allow clone modules
  i2c0_init_camera_pins();
  if(ov5647_use_bitbang)
    printf("ov5647: probing with 25 kHz GPIO SCCB; BSC0 bypassed\n");
  if(ov5647_read(REG_CHIPID_H, &hi) < 0 || ov5647_read(REG_CHIPID_L, &lo) < 0){
    uint32 status36 = i2c_last_status(OV5647_I2C_BUS);
    uint32 now, div, pins, levels;
    uint8 imxid[2] = { 0, 0 };
    int bb36 = i2c0_bitbang_probe(0x36);

    // The GPIO probe cycles GPIO44/45 through GPIO mode and back to ALT1.
    // Keep the old BSC recovery path available for platforms that select the
    // hardware transport, but this Pi 3 starts in software-SCCB mode.
    if(!ov5647_use_bitbang && ov5647_retry_bsc_rates(&hi, &lo) == 0)
      goto sensor_identified;

    // Some OV5647/SCCB modules acknowledge correct GPIO waveforms but not
    // BCM2837 BSC transactions.  Confirm the actual chip ID in software and,
    // if valid, keep using software SCCB for the whole sensor lifetime.
    // Once BSC has failed, do not disturb the sensor with another hardware
    // sweep on every power-cycle retry.  Do not gate a full transaction on
    // an address-only probe either: marginal ACK sampling can disagree even
    // when the slower complete SCCB read succeeds.
    if(!ov5647_use_bitbang){
      ov5647_use_bitbang = 1;
      printf("ov5647: BSC0 unusable; preferring 25 kHz GPIO SCCB\n");
    }
    uint8 reg_hi[2] = { REG_CHIPID_H >> 8, REG_CHIPID_H & 0xff };
    uint8 reg_lo[2] = { REG_CHIPID_L >> 8, REG_CHIPID_L & 0xff };
    for(int attempt = 1; attempt <= 3; attempt++){
      hi = lo = 0;
      int rh = i2c0_bitbang_write_read(OV5647_ADDR, reg_hi, 2, &hi, 1);
      int rl = rh == 0 ?
        i2c0_bitbang_write_read(OV5647_ADDR, reg_lo, 2, &lo, 1) : -1;
      if(rh == 0 && rl == 0){
        software_target_seen = 1;
        printf("ov5647: software SCCB chip-id=%x%x attempt=%d\n",
               hi, lo, attempt);
        if(hi == 0x56 && lo == 0x47){
          printf("ov5647: using GPIO software SCCB; BSC0 bypassed\n");
          goto sensor_identified;
        }
        // A transaction-level ACK only proves that an electrical target was
        // present; a bit-slip can still return data such as 0x3631.  Reject
        // it and retry the complete register-address/read sequence.
        printf("ov5647: rejected unstable chip-id=%x%x\n", hi, lo);
        delay_ms(5);
        continue;
      }
      printf("ov5647: software SCCB ID read retry %d/3 (probe=%s)\n",
             attempt, bb36 == 0 ? "ACK" : "NACK");
      delay_ms(5);
    }

    // If address 0x36 completed real register transactions but returned an
    // unstable ID, do not follow it with a scan of every 7-bit address.  The
    // caller will perform another clean power cycle; extra address clocks at
    // this point only make recovery from marginal SCCB timing less reliable.
    if(software_target_seen){
      i2c0_diagnostics(&now, &div, &pins, &levels);
      printf("ov5647: address 0x36 responds but chip-id is unstable; "
             "power-cycle retry required (fsel=%x levels=%x)\n",
             pins, levels);
      goto fail;
    }

    // An address ACK proves that the camera-side SCCB target is powered.
    // Do not follow a failed register read with probes of every other address:
    // those clocks made the next clean power-cycle less reliable on this
    // module.  Let camera_bind() retry after removing power instead.
    if(bb36 == 0){
      i2c0_diagnostics(&now, &div, &pins, &levels);
      printf("ov5647: address 0x36 ACK but chip-id read failed; "
             "power-cycle retry required (fsel=%x levels=%x)\n",
             pins, levels);
      goto fail;
    }

    int imx219_probe = sensor_read16(0x10, 0x0000, imxid, 2);
    if(imx219_probe >= 0)
      printf("camera: address 0x10 ACK, chip-id=%x%x via %s; %s\n",
             imxid[0], imxid[1],
             imx219_probe ? "STOP/START" : "repeated-START",
             (imxid[0] == 0x02 && imxid[1] == 0x19) ?
               "Sony IMX219 detected" : "unknown sensor");
    else if(i2c_read(OV5647_I2C_BUS, 0x1a, imxid, 1) == 0)
      printf("camera: address 0x1a ACK; likely IMX477/IMX708/IMX296, "
             "not OV5647\n");
    else {
      int bb10 = i2c0_bitbang_probe(0x10);
      int bb1a = i2c0_bitbang_probe(0x1a);
      i2c0_diagnostics(&now, &div, &pins, &levels);
      printf("camera: BSC probe summary 0x36=NACK 0x10=NACK 0x1a=NACK\n");
      printf("camera: GPIO bitbang probe 0x36=%s 0x10=%s 0x1a=%s\n",
             bb36 == 0 ? "ACK" : (bb36 == -2 ? "BUS-STUCK" : "NACK"),
             bb10 == 0 ? "ACK" : (bb10 == -2 ? "BUS-STUCK" : "NACK"),
             bb1a == 0 ? "ACK" : (bb1a == -2 ? "BUS-STUCK" : "NACK"));
      int found = 0;
      for(int addr = 0x08; addr <= 0x77; addr++){
        if(addr == 0x10 || addr == 0x1a || addr == 0x36)
          continue;
        int probe = i2c0_bitbang_probe(addr);
        if(probe == 0){
          printf("camera: full GPIO I2C scan found ACK at 0x%x\n", addr);
          found++;
        } else if(probe == -2){
          printf("camera: full GPIO I2C scan bus stuck at 0x%x\n", addr);
          break;
        }
      }
      if(found == 0)
        printf("camera: full GPIO I2C scan found no additional devices\n");
      if(bb36 == -1 && bb10 == -1 && bb1a == -1 && found == 0)
        printf("camera: no electrical I2C response; BSC0 routing is "
               "GPIO44/45 on Pi 3, check ribbon contacts and module power\n");
      printf("ov5647: no reply at i2c0 0x%x (GPIO44/45), "
             "BSC raw=%x flags=%x now=%x div=%d fsel=%x levels=%x\n",
             OV5647_ADDR, status36, status36 & 0x7ffU, now, div, pins,
             levels);
    }
    goto fail;
  }
sensor_identified:
  if(hi != 0x56 || lo != 0x47){
    printf("ov5647: unexpected chip id %x%x\n", hi, lo);
    goto fail;
  }
  if(ov5647_use_bitbang)
    printf("ov5647: software SCCB power-on chip-id=%x%x\n", hi, lo);
  if(ov5647_write_array(sensor_oe_enable, NELEM(sensor_oe_enable)) < 0){
    printf("ov5647: output-enable sequence failed\n");
    goto fail;
  }
  if(ov5647_use_bitbang)
    printf("ov5647: sensor outputs enabled\n");
  if(ov5647_stream_off(s) < 0){
    printf("ov5647: initial stream-off sequence failed\n");
    goto fail;
  }
  if(ov5647_use_bitbang)
    printf("ov5647: power-on complete, lanes in LP-11\n");
  return 0;
fail:
  if(powered)
    mbox_set_expgpio(OV5647_PWR_GPIO, 0, 0);
  return -1;
}

static void
ov5647_power_off(struct camsensor *s)
{
  uint8 v = 0;

  (void)s;
  ov5647_write_array(sensor_oe_disable, NELEM(sensor_oe_disable));
  if(ov5647_read(REG_SW_STANDBY, &v) == 0)
    ov5647_write(REG_SW_STANDBY, v & ~1);
  mbox_set_expgpio(OV5647_PWR_GPIO, 0, 0);
}

// Program the mode and the cached controls, then start streaming.
static int
ov5647_stream_on(struct camsensor *s, const struct cam_mode *mode)
{
  struct ov5647_state *st = s->priv;
  const struct ov5647_mode_regs *m = mode->priv;
  uint8 v;

  if(ov5647_use_bitbang)
    printf("ov5647: programming common registers (%d)\n",
           NELEM(common_regs));
  if(ov5647_write_array(common_regs, NELEM(common_regs)) < 0)
    return -1;
  if(ov5647_use_bitbang)
    printf("ov5647: programming mode registers (%d)\n", m->nregs);
  if(ov5647_write_array(m->regs, m->nregs) < 0)
    return -1;
  if(ov5647_use_bitbang)
    printf("ov5647: programming controls (%d)\n", m->nctrls);
  if(ov5647_write_array(m->ctrls, m->nctrls) < 0)
    return -1;
  if(ov5647_read(REG_MIPI_CTRL14, &v) < 0 ||
     ov5647_write(REG_MIPI_CTRL14, (v & ~(3 << 6)) | (s->bus.vc << 6)) < 0)
    return -1;
  if(ov5647_write(REG_TESTPATTERN, test_pattern_val[st->test_pattern]) < 0)
    return -1;
  if(ov5647_read(REG_SW_STANDBY, &v) < 0)
    return -1;
  if(!(v & 1) && ov5647_write(REG_SW_STANDBY, 0x01) < 0)
    return -1;
  v = MIPI_CTRL00_BUS_IDLE;
  if(s->bus.noncontinuous_clk)      // gate the clock lane between packets
    v |= MIPI_CTRL00_CLOCK_LANE_GATE | MIPI_CTRL00_LINE_SYNC_ENABLE;
  if(ov5647_write(REG_MIPI_CTRL00, v) < 0 ||
     ov5647_write(REG_FRAME_OFF, 0x00) < 0)
    return -1;
  if(ov5647_write(REG_PAD_OUT, 0x00) < 0)
    return -1;
  if(ov5647_use_bitbang)
    printf("ov5647: streaming started\n");
  return 0;
}

static int
ov5647_set_ctrl(struct camsensor *s, int id, int value)
{
  struct ov5647_state *st = s->priv;

  if(id == CAM_CTRL_TEST_PATTERN &&
     value >= 0 && value < NELEM(test_pattern_val)){
    st->test_pattern = value;
    return 0;
  }
  return -1;
}

static const struct sensor_ops ov5647_ops = {
  .power_on = ov5647_power_on,
  .power_off = ov5647_power_off,
  .stream_on = ov5647_stream_on,
  .stream_off = ov5647_stream_off,
  .set_ctrl = ov5647_set_ctrl,
};

static struct camsensor ov5647_sensor = {
  .name = "OV5647",
  .ops = &ov5647_ops,
  .modes = ov5647_modes,
  .nmodes = NELEM(ov5647_modes),
  .bus = { .data_lanes = 2, .noncontinuous_clk = 1, .vc = 0 },
  .warmup_frames = 30,              // ~0.5 s at 59 fps for AEC/AGC
  .priv = &ov5647_state,
};

void
ov5647_init(void)
{
  camsensor_register(&ov5647_sensor);
}
