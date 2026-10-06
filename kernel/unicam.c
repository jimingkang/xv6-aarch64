// BCM2837 Unicam CSI-2 receiver (CSI1) and /dev/video0: the camera bridge.
//
// Register programming follows Linux
// drivers/media/platform/broadcom/bcm2835-unicam.c (unicam_start_rx,
// unicam_disable, unicam_isr).  The image sensor is reached only through
// struct sensor_ops (camsensor.h): sensor drivers register themselves with
// camsensor_register(), camera_init() binds the first one that answers, and
// everything the receiver must know (lanes, clock mode, virtual channel,
// line length, CSI-2 data type, frame size) comes from its camsensor_bus
// and cam_mode.  This driver captures single frames:
//
//   read(/dev/video0)
//     power domain UNICAM1 on, CAM1 "lp" clock 100 MHz, sensor->power_on
//     Unicam started with the DMA pointers on a zero-size dummy buffer
//     sensor->stream_on(mode)
//     WARMUP  : let auto exposure settle (sensor->warmup_frames FS)
//     ARMED   : real buffer programmed; Unicam latches it at the next FS
//     CAPTURE : that frame is being written; dummy programmed for the next
//     DONE    : its frame end arrived -> stop everything, copy to the user
//
// As in Linux, new buffer addresses written to IBSA0/IBEA0 only take effect
// at the next frame start, so programming the dummy right after the real
// buffer's frame start leaves exactly one complete frame in the buffer.
//
// The state machine runs from the CSI1 interrupt and, as a fallback, from
// the reader every xv6 tick (100 ms).  Polling alone still yields a clean
// frame: whichever frame is in progress when the dummy is programmed is
// completed into the real buffer before the switch takes effect.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "file.h"
#include "defs.h"
#include "device.h"
#include "mbox.h"
#include "camsensor.h"

#define UNICAM_BASE   (PERIPHERAL_BASE + 0x801000UL)   // CSI1
#define UNICAM_CLKGATE (PERIPHERAL_BASE + 0x802004UL)  // CSI1 lane clocks
#define CM_BASE       (PERIPHERAL_BASE + 0x101000UL)

// Unicam registers (bcm2835-unicam-regs.h)
#define UNICAM_CTRL   0x000
#define UNICAM_STA    0x004
#define UNICAM_ANA    0x008
#define UNICAM_PRI    0x00c
#define UNICAM_CLK    0x010
#define UNICAM_CLT    0x014
#define UNICAM_DAT0   0x018
#define UNICAM_DAT1   0x01c
#define UNICAM_DLT    0x028
#define UNICAM_CMP0   0x02c
#define UNICAM_ICTL   0x100
#define UNICAM_ISTA   0x104
#define UNICAM_IDI0   0x108
#define UNICAM_IPIPE  0x10c
#define UNICAM_IBSA0  0x110
#define UNICAM_IBEA0  0x114
#define UNICAM_IBLS   0x118
#define UNICAM_IBWP   0x11c
#define UNICAM_IHWIN  0x120
#define UNICAM_IVWIN  0x128
#define UNICAM_DCS    0x200
#define UNICAM_MISC   0x400

// CTRL
#define CTRL_CPE      (1U << 0)
#define CTRL_MEM      (1U << 1)
#define CTRL_CPR      (1U << 2)
#define CTRL_SOE      (1U << 4)
#define CTRL_PFT(x)   ((x) << 8)
#define CTRL_OET(x)   ((x) << 12)
// STA
#define STA_SBE  (1U << 2)
#define STA_PBE  (1U << 3)
#define STA_HOE  (1U << 4)
#define STA_PLE  (1U << 5)
#define STA_SSC  (1U << 6)
#define STA_CRCE (1U << 7)
#define STA_IFO  (1U << 9)
#define STA_OFO  (1U << 10)
#define STA_DL   (1U << 12)
#define STA_PS   (1U << 13)
#define STA_IS   (1U << 14)
#define STA_PI0  (1U << 15)
#define STA_PI1  (1U << 16)
#define STA_MASK_ALL (STA_SBE | STA_PBE | STA_HOE | STA_PLE | STA_SSC | \
                      STA_CRCE | STA_IFO | STA_OFO | STA_DL | STA_PS | \
                      STA_PI0 | STA_PI1)
#define STA_ERRORS (STA_SBE | STA_PBE | STA_HOE | STA_PLE | STA_SSC | \
                    STA_CRCE | STA_IFO | STA_OFO)
// ANA
#define ANA_AR        (1U << 2)
#define ANA_DDL       (1U << 3)
#define ANA_CTATADJ(x) ((x) << 4)
#define ANA_PTATADJ(x) ((x) << 8)
// PRI
#define PRI_PE        (1U << 0)
#define PRI_PT(x)     ((x) << 1)
#define PRI_NP(x)     ((x) << 4)
#define PRI_PP(x)     ((x) << 8)
// CLK / DATn
#define LANE_EN       (1U << 0)
#define LANE_LPE      (1U << 2)
#define LANE_HSE      (1U << 3)
#define LANE_TRE      (1U << 4)
// CLT / DLT
#define T1(x)  ((x) << 0)
#define T2(x)  ((x) << 8)
// CMP0
#define CMP_PCE       (1U << 31)
#define CMP_GI        (1U << 9)
#define CMP_CPH       (1U << 8)
#define CMP_PCDT(x)   ((x) << 0)
// ICTL
#define ICTL_FSIE     (1U << 0)
#define ICTL_FEIE     (1U << 1)
#define ICTL_IBOB     (1U << 2)
#define ICTL_TFC      (1U << 4)
#define ICTL_LIP(x)   ((x) << 5)
// ISTA
#define ISTA_FSI      (1U << 0)
#define ISTA_FEI      (1U << 1)
#define ISTA_LCI      (1U << 2)
#define ISTA_MASK_ALL (ISTA_FSI | ISTA_FEI | ISTA_LCI)
// MISC
#define MISC_FL0      (1U << 6)
#define MISC_FL1      (1U << 9)

#define CSI1_MAX_LANES 2

// Clock manager: CAM1 ("lp" clock of CSI1), clk-bcm2835.c
#define CM_PASSWORD   0x5a000000U
#define CM_CAM1CTL    0x048
#define CM_CAM1DIV    0x04c
#define CM_ENABLE     (1U << 4)
#define CM_BUSY       (1U << 7)
#define CM_SRC_OSC    1U          // 19.2 MHz crystal
#define CM_SRC_PLLD   6U          // PLLD_PER, 500 MHz
#define CM_DIVI(x)    ((uint32)(x) << 12)

// DMA memory layout inside CAMDMA (non-cacheable)
#define FRAME_VA      ((uchar *)CAMDMA)
#define FRAME_PA      CAMDMA_PA
#define DUMMY_PA      (CAMDMA_PA + CAM_MAX_FRAME_BYTES)
#define BUS(pa)       (0xc0000000U | (uint32)(pa))   // as DWC2_DMA_BUS
_Static_assert(CAM_MAX_FRAME_BYTES + PGSIZE <= CAMDMA_SIZE,
               "CAMDMA too small for frame + dummy page");

#define CAM_WARMUP_MS      1500    // warm-up limit (polling fallback)
#define CAM_TIMEOUT_MS     4000
#define CAM_MAX_SENSORS    4

enum cam_state { CAM_IDLE, CAM_WARMUP, CAM_ARMED, CAM_CAPTURE, CAM_DONE };

static struct {
  struct spinlock lock;       // state below; taken by the IRQ handler
  struct sleeplock busy;      // one capture at a time
  struct camsensor *candidates[CAM_MAX_SENSORS];  // registered drivers
  int ncandidates;
  struct camsensor *sensor;   // bound sensor, 0 if none answered
  const struct cam_mode *mode;
  uint32 frame_bytes;         // mode->bytesperline * mode->height
  volatile enum cam_state state;
  uint frame_starts;
  uint frame_ends;
  uint irqs;
  uint spurious;              // CSI1 interrupts that found no Unicam status
  int irq_off;                // gave up on the interrupt: poll only
  uint sta_seen;              // OR of STA over the capture, for diagnostics
  uint64 warm_deadline;
  uint64 done_us;
  uint32 lp_hz;
} cam;

static int camera_bind(void);

// Sensor drivers call this before camera_init() (Linux:
// v4l2_async_register_subdev).
int
camsensor_register(struct camsensor *s)
{
  if(cam.ncandidates >= CAM_MAX_SENSORS || s == 0 || s->ops == 0 ||
     s->ops->power_on == 0 || s->ops->power_off == 0 ||
     s->ops->stream_on == 0 || s->ops->stream_off == 0 || s->nmodes < 1)
    return -1;
  cam.candidates[cam.ncandidates++] = s;
  return 0;
}

static inline uint32
rd(uint32 off)
{
  return *(volatile uint32 *)(UNICAM_BASE + off);
}

static inline void
wr(uint32 off, uint32 v)
{
  *(volatile uint32 *)(UNICAM_BASE + off) = v;
}

static inline void
set_bits(uint32 off, uint32 bits, int on)
{
  uint32 v = rd(off);
  wr(off, on ? (v | bits) : (v & ~bits));
}

static inline uint32
cm_rd(uint32 off)
{
  return *(volatile uint32 *)(CM_BASE + off);
}

static inline void
cm_wr(uint32 off, uint32 v)
{
  *(volatile uint32 *)(CM_BASE + off) = CM_PASSWORD | v;
}

static uint64
now_us(void)
{
  return r_cntvct_el0() / (r_cntfrq_el0() / 1000000);
}

static void
delay_us(uint32 us)
{
  uint64 end = r_cntvct_el0() + (uint64)r_cntfrq_el0() * us / 1000000;
  while(r_cntvct_el0() < end)
    asm volatile("yield" ::: "memory");
}

static int
cm_wait(uint32 busy_value)
{
  for(int i = 0; i < 1000; i++){
    if((cm_rd(CM_CAM1CTL) & CM_BUSY) == busy_value)
      return 0;
    delay_us(1);
  }
  return -1;
}

static void
cam1_clock_off(void)
{
  cm_wr(CM_CAM1CTL, cm_rd(CM_CAM1CTL) & ~CM_ENABLE & 0xffffff);
  cm_wait(0);
}

// The Linux driver runs the CSI "lp" clock at 100 MHz.  PLLD_PER is 500 MHz
// on the Pi 3; if that source turns out not to run, fall back to the
// crystal so the receiver still has an LP clock.
static int
cam1_clock_on(void)
{
  cam1_clock_off();
  cm_wr(CM_CAM1DIV, CM_DIVI(5));
  cm_wr(CM_CAM1CTL, CM_SRC_PLLD);
  cm_wr(CM_CAM1CTL, CM_SRC_PLLD | CM_ENABLE);
  if(cm_wait(CM_BUSY) == 0){
    cam.lp_hz = 100000000;
    return 0;
  }
  cam1_clock_off();
  cm_wr(CM_CAM1DIV, CM_DIVI(1));
  cm_wr(CM_CAM1CTL, CM_SRC_OSC);
  cm_wr(CM_CAM1CTL, CM_SRC_OSC | CM_ENABLE);
  if(cm_wait(CM_BUSY) == 0){
    cam.lp_hz = 19200000;
    printf("unicam: PLLD_PER not running, CAM1 clock from 19.2 MHz crystal\n");
    return 0;
  }
  printf("unicam: CAM1 clock did not start (ctl=%x)\n", cm_rd(CM_CAM1CTL));
  return -1;
}

static void
set_image_buffer(uint32 pa, uint32 bytes)
{
  // A zero-size buffer (end == start) is how Linux parks the receiver on
  // its dummy page, working around an overrun bug in circular mode.
  wr(UNICAM_IBSA0, BUS(pa));
  wr(UNICAM_IBEA0, BUS(pa + bytes));
}

// unicam_start_rx() for CSI-2, configured from the sensor's bus and mode.
static void
unicam_start(const struct camsensor_bus *bus, const struct cam_mode *mode)
{
  uint32 lane;

  // Lane clocks: CLK and DAT0..n-1 enabled (b01 each), with CM password.
  *(volatile uint32 *)UNICAM_CLKGATE = CM_PASSWORD |
    (0x155 & ((1U << (bus->data_lanes * 2 + 2)) - 1));

  wr(UNICAM_CTRL, CTRL_MEM);
  wr(UNICAM_ANA, ANA_AR | ANA_CTATADJ(7) | ANA_PTATADJ(7));
  delay_us(1500);
  set_bits(UNICAM_ANA, ANA_AR, 0);
  set_bits(UNICAM_CTRL, CTRL_CPR, 1);       // peripheral reset
  set_bits(UNICAM_CTRL, CTRL_CPR, 0);
  set_bits(UNICAM_CTRL, CTRL_CPE, 0);

  // CSI-2 mode (CPM=0), strobe (DCM=0), packet framer timeout, OET.
  wr(UNICAM_CTRL, (rd(UNICAM_CTRL) & ~((1U << 3) | (1U << 5) | (0xfU << 8) |
                                       (0x1ffU << 12))) |
                  CTRL_PFT(0xf) | CTRL_OET(128));
  wr(UNICAM_IHWIN, 0);
  wr(UNICAM_IVWIN, 0);
  wr(UNICAM_PRI, PRI_PP(0xe) | PRI_NP(8) | PRI_PT(2) | PRI_PE);
  set_bits(UNICAM_ANA, ANA_DDL, 0);

  wr(UNICAM_ICTL, ICTL_FSIE | ICTL_FEIE | ICTL_IBOB);
  wr(UNICAM_STA, STA_MASK_ALL);
  wr(UNICAM_ISTA, ISTA_MASK_ALL);

  wr(UNICAM_CLT, T1(2) | T2(6));             // tclk_term_en, tclk_settle
  wr(UNICAM_DLT, T1(2) | T2(6));             // td_term_en, ths_settle
  set_bits(UNICAM_CTRL, CTRL_SOE, 0);

  // Packet compare on frame end: required to avoid missing frame ends.
  wr(UNICAM_CMP0, CMP_PCE | CMP_GI | CMP_CPH | CMP_PCDT(1));

  // A non-continuous clock only needs LP detection; a continuous one also
  // needs HS termination and HS receive enabled from the start.
  lane = LANE_EN | LANE_LPE;
  if(!bus->noncontinuous_clk)
    lane |= LANE_TRE | LANE_HSE;
  wr(UNICAM_CLK, lane);
  wr(UNICAM_DAT0, lane);
  wr(UNICAM_DAT1, bus->data_lanes >= 2 ? lane : 0);

  wr(UNICAM_IBLS, mode->bytesperline);
  set_image_buffer(DUMMY_PA, 0);            // warm-up frames go nowhere
  wr(UNICAM_IPIPE, 0);                      // keep the CSI-2 packing
  wr(UNICAM_IDI0, (bus->vc << 6) | mode->csi_dt);
  set_bits(UNICAM_MISC, MISC_FL0 | MISC_FL1, 1);

  set_bits(UNICAM_CTRL, CTRL_CPE, 1);       // enable
  set_bits(UNICAM_ICTL, ICTL_LIP(1), 1);    // load image pointers
  set_bits(UNICAM_ICTL, ICTL_TFC, 1);       // sync to the next frame start
}

// unicam_disable()
static void
unicam_stop(void)
{
  wr(UNICAM_ICTL, 0);
  set_bits(UNICAM_ANA, ANA_DDL, 1);
  set_bits(UNICAM_CTRL, CTRL_SOE, 1);
  wr(UNICAM_DAT0, 0);
  wr(UNICAM_DAT1, 0);
  set_bits(UNICAM_CTRL, CTRL_CPR, 1);
  delay_us(100);
  set_bits(UNICAM_CTRL, CTRL_CPR, 0);
  set_bits(UNICAM_CTRL, CTRL_CPE, 0);
  wr(UNICAM_DCS, 0);
  wr(UNICAM_STA, STA_MASK_ALL);
  wr(UNICAM_ISTA, ISTA_MASK_ALL);
  *(volatile uint32 *)UNICAM_CLKGATE = CM_PASSWORD;
}

// Advance the capture state machine from the hardware status.
// Called from the CSI1 interrupt and from the polling fallback.
// Returns 0 if Unicam had nothing pending.
static int
unicam_service(int from_irq)
{
  uint32 sta, ista;
  int fe;

  acquire(&cam.lock);
  if(cam.state == CAM_IDLE){
    release(&cam.lock);
    return 0;
  }
  if(from_irq)
    cam.irqs++;
  sta = rd(UNICAM_STA);
  wr(UNICAM_STA, sta);
  ista = rd(UNICAM_ISTA);
  wr(UNICAM_ISTA, ista);
  cam.sta_seen |= sta;
  if(!(sta & (STA_IS | STA_PI0))){
    release(&cam.lock);
    return (sta | ista) != 0;
  }

  // Frame end first: an FE and the next FS can arrive together.
  fe = (ista & ISTA_FEI) || (sta & STA_PI0);
  if(fe){
    cam.frame_ends++;
    if(cam.state == CAM_CAPTURE){
      cam.state = CAM_DONE;
      cam.done_us = now_us();
      wakeup(&ticks);           // the reader sleeps on the tick channel
    }
  }
  if(ista & ISTA_FSI){
    cam.frame_starts++;
    if(cam.state == CAM_ARMED){
      // This frame goes to the real buffer; send the next to the dummy.
      set_image_buffer(DUMMY_PA, 0);
      cam.state = CAM_CAPTURE;
    } else if(cam.state == CAM_WARMUP &&
              (cam.frame_starts >= cam.sensor->warmup_frames ||
               r_cntvct_el0() >= cam.warm_deadline)){
      set_image_buffer(FRAME_PA, cam.frame_bytes);  // latched at next FS
      cam.state = CAM_ARMED;
    }
  }
  release(&cam.lock);
  return 1;
}

// CSI1_IRQ comes from the device tree (<2 7>), not from hardware we could
// test.  If the line turns out to belong to something else, the handler
// could never clear it and CPU0 would loop in the interrupt; so give up on
// the interrupt after repeated empty calls and let the reader poll.
void
unicam_irq(void)
{
  if(unicam_service(1))
    return;
  if(++cam.spurious >= 100 && !cam.irq_off){
    cam.irq_off = 1;
    bcm2837_disable_irq(CSI1_IRQ);
    printf("unicam: IRQ %d has no Unicam status; polling instead\n",
           CSI1_IRQ);
  }
}

static void
cam_power_off(void)
{
  cam.sensor->ops->power_off(cam.sensor);
  cam1_clock_off();
  mbox_set_domain(MBOX_DOMAIN_UNICAM1, 0);
}

// Capture one frame into FRAME_VA.  Returns 0 on success.
static int
cam_capture(struct cam_frame_hdr *hdr)
{
  enum cam_state st;
  uint64 deadline;

  if(mbox_set_domain(MBOX_DOMAIN_UNICAM1, 1) < 0){
    printf("unicam: firmware refused power domain UNICAM1\n");
    return -1;
  }
  if(cam1_clock_on() < 0 || cam.sensor->ops->power_on(cam.sensor) < 0){
    cam1_clock_off();
    mbox_set_domain(MBOX_DOMAIN_UNICAM1, 0);
    return -1;
  }
  printf("unicam: sensor powered, receiver setup begins\n");
  memset(FRAME_VA, 0, cam.frame_bytes);

  acquire(&cam.lock);
  cam.frame_starts = cam.frame_ends = cam.irqs = cam.sta_seen = 0;
  cam.warm_deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * CAM_WARMUP_MS / 1000;
  cam.state = CAM_WARMUP;
  release(&cam.lock);

  // Linux order: receiver first, then the sensor's stream.
  unicam_start(&cam.sensor->bus, cam.mode);
  printf("unicam: receiver enabled, starting sensor stream\n");
  if(!cam.irq_off)
    bcm2837_enable_irq(CSI1_IRQ);
  if(cam.sensor->ops->stream_on(cam.sensor, cam.mode) < 0){
    acquire(&cam.lock);
    cam.state = CAM_IDLE;
    release(&cam.lock);
    bcm2837_disable_irq(CSI1_IRQ);
    cam.sensor->ops->stream_off(cam.sensor);
    unicam_stop();
    cam_power_off();
    return -1;
  }
  printf("unicam: sensor streaming, waiting for frame\n");

  // Bring-up uses bounded polling.  An unverified/stuck legacy CSI IRQ can
  // starve CPU0's generic-timer interrupt, in which case a timeout based on
  // xv6 ticks never expires.  CNTVCT keeps advancing independently.
  deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * CAM_TIMEOUT_MS / 1000;
  for(;;){
    unicam_service(0);
    st = cam.state;
    if(st == CAM_DONE || r_cntvct_el0() >= deadline || myproc()->killed)
      break;
    delay_us(1000);
  }

  // Linux order: sensor stream off, then the receiver.
  cam.sensor->ops->stream_off(cam.sensor);
  acquire(&cam.lock);
  st = cam.state;
  cam.state = CAM_IDLE;
  release(&cam.lock);
  bcm2837_disable_irq(CSI1_IRQ);
  uint32 ibwp = rd(UNICAM_IBWP);
  unicam_stop();
  cam_power_off();

  if(st != CAM_DONE){
    printf("unicam: no frame (state %d, FS %d, FE %d, irqs %d, "
           "sta %x, ibwp %x, lp %d Hz)\n", st, cam.frame_starts,
           cam.frame_ends, cam.irqs, cam.sta_seen, ibwp, cam.lp_hz);
    return -1;
  }
  if(cam.irqs == 0){
    if(cam.irq_off)
      printf("unicam: frame captured by polling; CSI1 IRQ %d intentionally "
             "masked\n", CSI1_IRQ);
    else
      printf("unicam: frame captured by polling; CSI1 IRQ %d did not fire\n",
             CSI1_IRQ);
  }
  if(cam.sta_seen & STA_ERRORS)
    printf("unicam: CSI-2 errors seen, sta %x\n", cam.sta_seen);

  hdr->magic = CAM_MAGIC;
  hdr->width = cam.mode->width;
  hdr->height = cam.mode->height;
  hdr->bytesperline = cam.mode->bytesperline;
  hdr->format = cam.mode->format;
  hdr->data_bytes = cam.frame_bytes;
  hdr->sequence = cam.frame_ends;
  hdr->reserved = 0;
  hdr->timestamp_us = cam.done_us;
  return 0;
}

static int
camread(int user_dst, uint64 dst, int n)
{
  struct cam_frame_hdr hdr;
  int r = -1;

  acquiresleep(&cam.busy);
  // The software SCCB bus on clone camera modules can miss the boot-time
  // probe.  /dev/video0 remains registered, so retry binding on first use
  // instead of leaving the device permanently inactive until reboot.
  if((cam.sensor == 0 && camera_bind() < 0) ||
     n < (int)(sizeof(hdr) + cam.frame_bytes))
    goto out;
  if(cam_capture(&hdr) == 0 &&
     either_copyout(user_dst, dst, &hdr, sizeof(hdr)) == 0 &&
     either_copyout(user_dst, dst + sizeof(hdr), FRAME_VA,
                    cam.frame_bytes) == 0)
    r = sizeof(hdr) + cam.frame_bytes;
out:
  releasesleep(&cam.busy);
  return r;
}

// "pattern=N" selects a sensor test pattern for later captures.  The value
// is checked and cached by the sensor (set_ctrl) and applied at stream_on.
static int
camwrite(int user_src, uint64 src, int n)
{
  char buf[16];
  int value = 0, i, r;

  if(n <= 0 || n >= (int)sizeof(buf) ||
     either_copyin(buf, user_src, src, n) < 0)
    return -1;
  buf[n] = 0;
  if(strncmp(buf, "pattern=", 8) != 0 || buf[8] == 0)
    return -1;
  for(i = 8; buf[i] >= '0' && buf[i] <= '9' && value < 1000; i++)
    value = value * 10 + buf[i] - '0';
  if(buf[i] != 0 && buf[i] != '\n')
    return -1;
  acquiresleep(&cam.busy);            // never change controls mid-capture
  if(cam.sensor == 0){
    printf("camera: retrying sensor probe on control request\n");
    if(camera_bind() < 0){
      releasesleep(&cam.busy);
      return -1;
    }
  }
  r = cam.sensor->ops->set_ctrl ?
      cam.sensor->ops->set_ctrl(cam.sensor, CAM_CTRL_TEST_PATTERN, value) : -1;
  releasesleep(&cam.busy);
  return r < 0 ? -1 : n;
}

// Can this receiver and DMA area take the mode at all?
static int
mode_supported(const struct camsensor *s, const struct cam_mode *m)
{
  uint64 bytes = (uint64)m->bytesperline * m->height;

  return s->bus.data_lanes >= 1 && s->bus.data_lanes <= CSI1_MAX_LANES &&
         s->bus.vc >= 0 && s->bus.vc <= 3 &&
         m->bytesperline % 16 == 0 && m->bytesperline > 0 &&
         m->height > 0 && bytes <= CAM_MAX_FRAME_BYTES;
}

// Bind the first usable registered sensor.  The caller serializes this with
// cam.busy after userspace is running; camera_init() calls it before any user
// process can open video0.
static int
camera_bind(void)
{
  if(cam.sensor)
    return 0;
  for(int i = 0; i < cam.ncandidates; i++){
    struct camsensor *s = cam.candidates[i];
    // Boot has no current process and should not be delayed by repeated full
    // I2C scans.  A userspace open/control request may retry complete clean
    // power cycles: clone modules occasionally miss their first power edge.
    int attempts = myproc() ? 3 : 1;

    if(!mode_supported(s, &s->modes[0])){
      printf("unicam: %s default mode %s not supported by CSI1\n",
             s->name, s->modes[0].name);
      continue;
    }
    int attempt;
    for(attempt = 1; attempt <= attempts; attempt++){
      if(s->ops->power_on(s) == 0)
        break;
      if(attempt < attempts){
        printf("camera: %s runtime probe retry %d/%d after power cycle\n",
               s->name, attempt + 1, attempts);
        delay_us(100000);
      }
    }
    if(attempt > attempts)
      continue;
    s->ops->power_off(s);
    cam.sensor = s;
    cam.mode = &s->modes[0];
    cam.frame_bytes = cam.mode->bytesperline * cam.mode->height;
    printf("camera: %s on CSI1 (%d lane%s) -> /dev/video0, %s\n", s->name,
           s->bus.data_lanes, s->bus.data_lanes > 1 ? "s" : "",
           cam.mode->name);
    return 0;
  }
  return -1;
}

// Boot-time probe (Linux: the async notifier's bound callback).  Try the
// registered sensors in order; bind the first that powers up and returns
// its chip ID, then power it down again until the first read().
void
camera_init(void)
{
  static struct file_operations fops = {
    .read = camread,
    .write = camwrite,
  };

  initlock(&cam.lock, "unicam");
  initsleeplock(&cam.busy, "camera");
  cam.state = CAM_IDLE;
  // Keep the legacy CSI line masked until its BCM2837 routing is validated
  // from successful polling captures.  This also guarantees a bad level IRQ
  // cannot prevent the ARM-counter capture timeout from running.
  cam.irq_off = 1;
  bcm2837_disable_irq(CSI1_IRQ);
  printf("camera: driver gpio-sccb-v8 unicam-poll-v2\n");
  if(register_chrdev(CAMERA, "video0", &fops) < 0){
    printf("unicam: cannot register /dev/video0\n");
    return;
  }
  if(camera_bind() == 0)
    return;
  printf("camera: no sensor found (%d driver%s tried); /dev/video0 "
         "inactive\n", cam.ncandidates, cam.ncandidates == 1 ? "" : "s");
}
