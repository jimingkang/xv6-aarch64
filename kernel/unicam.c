// BCM2837 Unicam CSI-2 receiver (CSI1) and /dev/video0: the camera bridge.
//
// Register programming follows Linux
// drivers/media/platform/broadcom/bcm2835-unicam.c (unicam_start_rx,
// unicam_disable, unicam_isr).  The image sensor is reached only through
// struct sensor_ops (camsensor.h): sensor drivers register themselves with
// camsensor_register(), camera_init() binds the first one that answers, and
// everything the receiver must know (lanes, clock mode, virtual channel,
// line length, CSI-2 data type, frame size) comes from its camsensor_bus
// and cam_mode.  The sensor streams continuously and the receiver is parked
// on a zero-size dummy buffer between reads, so read() grabs a frame on
// demand without a power cycle per frame:
//
//   first read(/dev/video0)
//     power domain UNICAM1 on, CAM1 "lp" clock 100 MHz, sensor->power_on
//     Unicam started with the DMA pointers on a zero-size dummy buffer
//     sensor->stream_on(mode)
//     WARMUP    : let auto exposure settle (sensor->warmup_frames FS)
//     STREAMING : sensor keeps streaming, receiver parked on the dummy
//   every read(/dev/video0)
//     ARMED   : a free slot is programmed; Unicam latches it at the next FS
//     CAPTURE : that slot is being written; another free slot is queued
//     DONE    : FE enqueues the completed slot; read copies or mmap dequeues it
//   release (/dev/video0 closed)
//     sensor->stream_off, receiver stop, power domain off
//
// As in Linux, new buffer addresses written to IBSA0/IBEA0 only take effect
// at the next frame start, so programming the dummy right after the real
// buffer's frame start leaves exactly one complete frame in the buffer.
//
// CSI1 IRQ advances the state machine and wakes the reader.  A 10-ms delayed
// work item runs the same service routine as a watchdog/fallback on boards
// where legacy IRQ 39 is not routed; the user process sleeps in both cases.
// Two DMA slots allow one frame to be owned by userspace while the other is
// NEXT/ACTIVE, and a read-only Normal-NC mmap provides an optional zero-copy
// dequeue API without cache-attribute aliases.

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
#include "mman.h"
#include "mbox.h"
#include "camsensor.h"
#include "workqueue.h"

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
#define FRAME_VA(n)   ((uchar *)CAMDMA + (n) * CAM_SLOT_BYTES)
#define FRAME_PA(n)   (CAMDMA_PA + (n) * CAM_SLOT_BYTES)
#define DUMMY_PA      (CAMDMA_PA + CAM_BUFFER_COUNT * CAM_SLOT_BYTES)
#define BUS(pa)       (0xc0000000U | (uint32)(pa))   // as DWC2_DMA_BUS
_Static_assert(CAM_BUFFER_COUNT * CAM_SLOT_BYTES + PGSIZE <= CAMDMA_SIZE,
               "CAMDMA too small for frame slots + dummy page");

#define CAM_WARMUP_MS      1500    // warm-up limit (polling fallback)
#define CAM_TIMEOUT_MS     4000
#define CAM_MAX_SENSORS    4

enum cam_state { CAM_IDLE, CAM_WARMUP, CAM_STREAMING, CAM_ARMED,
                 CAM_CAPTURE, CAM_DONE };
enum cam_buffer_state { CAM_BUF_FREE, CAM_BUF_NEXT, CAM_BUF_ACTIVE,
                        CAM_BUF_DONE, CAM_BUF_USER };

struct cam_buffer {
  enum cam_buffer_state state;
  uint sequence;
  uint64 timestamp_us;
};

struct cam_file {
  int held_slot;
};

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
  int irq_reported;           // positive IRQ-path banner printed once/stream
  uint spurious;              // CSI1 interrupts that found no Unicam status
  int irq_off;                // gave up on the interrupt: poll only
  uint sta_seen;              // OR of STA over the capture, for diagnostics
  uint64 warm_deadline;
  uint64 done_us;
  uint32 lp_hz;
  int streaming;              // sensor streaming continuously
  int open_count;
  int active_slot;            // slot currently receiving pixels, -1 for dummy
  int next_slot;              // slot latched by the next FS, -1 for dummy
  struct cam_buffer buffers[CAM_BUFFER_COUNT];
  int doneq[CAM_BUFFER_COUNT];
  int done_head;
  int done_count;
  struct workqueue wq;
  struct delayed_work poll_work; // IRQ watchdog/fallback, never busy-waits caller
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
  // The device must see both pointer writes before software publishes the
  // corresponding NEXT/ARMED state to an interrupt running on another CPU.
  asm volatile("dsb sy" ::: "memory");
}

// cam.lock held below.  Unicam latches these shadow pointers only at FS.
static int
free_slot_locked(void)
{
  for(int i = 0; i < CAM_BUFFER_COUNT; i++)
    if(cam.buffers[i].state == CAM_BUF_FREE)
      return i;
  return -1;
}

static void
park_dummy_locked(void)
{
  set_image_buffer(DUMMY_PA, 0);
  cam.next_slot = -1;
}

static int
queue_slot_locked(void)
{
  int slot;

  if(cam.next_slot >= 0)
    return cam.next_slot;
  slot = free_slot_locked();
  if(slot < 0){
    park_dummy_locked();
    return -1;
  }
  set_image_buffer(FRAME_PA(slot), cam.frame_bytes);
  cam.buffers[slot].state = CAM_BUF_NEXT;
  cam.next_slot = slot;
  if(cam.active_slot < 0)
    cam.state = CAM_ARMED;
  return slot;
}

static void
done_push_locked(int slot)
{
  if(cam.done_count >= CAM_BUFFER_COUNT)
    panic("unicam doneq");
  cam.doneq[(cam.done_head + cam.done_count) % CAM_BUFFER_COUNT] = slot;
  cam.done_count++;
}

static int
done_pop_locked(void)
{
  int slot;

  if(cam.done_count == 0)
    return -1;
  slot = cam.doneq[cam.done_head];
  cam.done_head = (cam.done_head + 1) % CAM_BUFFER_COUNT;
  cam.done_count--;
  cam.buffers[slot].state = CAM_BUF_USER;
  return slot;
}

static void
release_slot_locked(int slot)
{
  if(slot < 0 || slot >= CAM_BUFFER_COUNT ||
     cam.buffers[slot].state != CAM_BUF_USER)
    return;
  cam.buffers[slot].state = CAM_BUF_FREE;
  // If the receiver had to park because both slots were full, make the
  // returned slot the next capture without waiting for another read().
  if(cam.streaming && cam.next_slot < 0)
    queue_slot_locked();
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
  int fe, wake = 0;

  acquire(&cam.lock);
  if(cam.state == CAM_IDLE){
    release(&cam.lock);
    return 0;
  }
  sta = rd(UNICAM_STA);
  wr(UNICAM_STA, sta);
  ista = rd(UNICAM_ISTA);
  wr(UNICAM_ISTA, ista);
  cam.sta_seen |= sta;
  if(!(sta & (STA_IS | STA_PI0)) && !(ista & (ISTA_FSI | ISTA_FEI))){
    release(&cam.lock);
    return (sta | ista) != 0;
  }
  if(from_irq)
    cam.irqs++;

  // Frame end first: an FE and the next FS can arrive together.
  fe = (ista & ISTA_FEI) || (sta & STA_PI0);
  if(fe){
    cam.frame_ends++;
    if(cam.active_slot >= 0){
      int slot = cam.active_slot;
      cam.active_slot = -1;
      cam.buffers[slot].state = CAM_BUF_DONE;
      cam.buffers[slot].sequence = cam.frame_ends;
      cam.buffers[slot].timestamp_us = now_us();
      done_push_locked(slot);
      cam.done_us = now_us();
      cam.state = CAM_DONE;
      wake = 1;
    }
  }
  if(ista & ISTA_FSI){
    cam.frame_starts++;
    if(cam.next_slot >= 0){
      // Hardware has just latched this slot.  Queue another free slot for
      // the following FS, or park on dummy when userspace owns both slots.
      int slot = cam.next_slot;
      cam.next_slot = -1;
      cam.active_slot = slot;
      cam.buffers[slot].state = CAM_BUF_ACTIVE;
      cam.state = CAM_CAPTURE;
      if(queue_slot_locked() < 0)
        cam.state = CAM_CAPTURE;
    } else if(cam.state == CAM_WARMUP &&
              (cam.frame_starts >= cam.sensor->warmup_frames ||
               r_cntvct_el0() >= cam.warm_deadline)){
      // Warm-up done: the sensor keeps streaming and the receiver stays
      // parked on the zero-size dummy.  cam_grab() arms the real buffer to
      // capture exactly one frame on demand.
      cam.state = CAM_STREAMING;
      wake = 1;
    }
  }
  if(wake)
    wakeup(&cam.done_count);
  release(&cam.lock);
  return 1;
}

// CSI1 IRQ is the normal completion source.  This delayed worker is a bounded
// watchdog for boards/firmware where legacy IRQ 39 is not routed: it advances
// the same state machine every 10 ms and wakes sleepers to check deadlines.
// Unlike the old delay_us(1000) loops, the calling process consumes no CPU.
static void
unicam_poll_work(struct work_struct *work)
{
  int active;

  (void)work;
  unicam_service(0);
  acquire(&cam.lock);
  active = cam.state != CAM_IDLE;
  if(active)
    wakeup(&cam.done_count);
  release(&cam.lock);
  if(active)
    queue_delayed_work(&cam.wq, &cam.poll_work, 1);
}

// CSI1_IRQ comes from the device tree (<2 7>), not from hardware we could
// test.  If the line turns out to belong to something else, the handler
// could never clear it and CPU0 would loop in the interrupt; so give up on
// the interrupt after repeated empty calls and let the reader poll.
void
unicam_irq(void)
{
  if(unicam_service(1)){
    cam.spurious = 0;
    return;
  }
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

// Stop the sensor and receiver and power down (idempotent).
static void
cam_stream_stop(void)
{
  if(!cam.streaming)
    return;
  bcm2837_disable_irq(CSI1_IRQ);
  // Publish IDLE before cancelling: a poll callback already in flight will
  // observe it and will not requeue itself behind cancel_delayed_work_sync().
  acquire(&cam.lock);
  cam.state = CAM_IDLE;
  release(&cam.lock);
  cancel_delayed_work_sync(&cam.poll_work);
  cam.sensor->ops->stream_off(cam.sensor);
  acquire(&cam.lock);
  cam.active_slot = cam.next_slot = -1;
  cam.done_head = cam.done_count = 0;
  for(int i = 0; i < CAM_BUFFER_COUNT; i++)
    cam.buffers[i].state = CAM_BUF_FREE;
  release(&cam.lock);
  unicam_stop();
  cam_power_off();
  cam.streaming = 0;
}

// Power the sensor on and start it streaming, discarding warm-up frames until
// auto exposure settles.  The receiver is parked on the zero-size dummy buffer
// throughout, so nothing is kept; cam_grab() arms the real buffer to capture
// one frame on demand.  Returns 0 once CAM_STREAMING is reached.
static int
cam_stream_start(void)
{
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
  for(int i = 0; i < CAM_BUFFER_COUNT; i++)
    memset(FRAME_VA(i), 0, cam.frame_bytes);

  acquire(&cam.lock);
  cam.frame_starts = cam.frame_ends = cam.irqs = cam.spurious =
    cam.sta_seen = cam.irq_reported = 0;
  cam.active_slot = cam.next_slot = -1;
  cam.done_head = cam.done_count = 0;
  for(int i = 0; i < CAM_BUFFER_COUNT; i++)
    cam.buffers[i].state = CAM_BUF_FREE;
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
  cam.streaming = 1;
  queue_delayed_work(&cam.wq, &cam.poll_work, 1);
  printf("unicam: sensor streaming, waiting for warm-up\n");

  // Bring-up uses bounded polling.  An unverified/stuck legacy CSI IRQ can
  // starve CPU0's generic-timer interrupt, in which case a timeout based on
  // xv6 ticks never expires.  CNTVCT keeps advancing independently.
  // unicam_service() advances the state machine to CAM_STREAMING.
  deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * CAM_WARMUP_MS / 1000;
  acquire(&cam.lock);
  while(cam.state != CAM_STREAMING && r_cntvct_el0() < deadline &&
        !myproc()->killed)
    sleep(&cam.done_count, &cam.lock);
  int settled = cam.state == CAM_STREAMING;
  release(&cam.lock);
  if(!settled){
    printf("unicam: stream did not settle (state %d, FS %d, FE %d)\n",
           cam.state, cam.frame_starts, cam.frame_ends);
    cam_stream_stop();
    return -1;
  }
  printf("unicam: streaming (parked), %d warm-up frames\n", cam.frame_starts);
  return 0;
}

// Dequeue one complete frame while the sensor keeps streaming.  If no frame
// is queued, arm a free slot; FS latches it, FE marks it done, and a second
// slot can already be NEXT/ACTIVE.  The dummy is used only when userspace owns
// all real slots.
static int
cam_grab(struct cam_frame_hdr *hdr, int *slotp)
{
  uint64 deadline;
  enum cam_state st = CAM_IDLE;
  int slot, report_irq = 0;

  deadline = r_cntvct_el0() +
    (uint64)r_cntfrq_el0() * CAM_TIMEOUT_MS / 1000;
  acquire(&cam.lock);
  // Buffer programming and CAM_ARMED publication are one critical section.
  // The spinlock disables the local IRQ and serializes a CSI IRQ on another
  // CPU; dsb in set_image_buffer() orders MMIO before the state is visible.
  if(cam.done_count == 0 && cam.next_slot < 0 && cam.active_slot < 0)
    queue_slot_locked();
  while(cam.done_count == 0 && r_cntvct_el0() < deadline &&
        !myproc()->killed)
    sleep(&cam.done_count, &cam.lock);
  st = cam.state;
  slot = done_pop_locked();
  if(slot >= 0 && cam.irqs > 0 && !cam.irq_reported){
    cam.irq_reported = 1;
    report_irq = 1;
  }
  release(&cam.lock);

  if(slot < 0){
    uint32 ibwp = rd(UNICAM_IBWP);
    printf("unicam: no frame (state %d, FS %d, FE %d, irqs %d, "
           "sta %x, ibwp %x, lp %d Hz)\n", st, cam.frame_starts,
           cam.frame_ends, cam.irqs, cam.sta_seen, ibwp, cam.lp_hz);
    cam_stream_stop();
    return -1;
  }

  if(report_irq){
    printf("unicam: frame captured by CSI1 IRQ %d; watchdog remains armed\n",
           CSI1_IRQ);
  } else if(cam.irqs == 0){
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
  hdr->sequence = cam.buffers[slot].sequence;
  hdr->reserved = slot;
  hdr->timestamp_us = cam.buffers[slot].timestamp_us;
  *slotp = slot;
  return 0;
}

static int
camopen(struct file *f)
{
  struct cam_file *cf = kalloc();

  if(cf == 0)
    return -1;
  memset(cf, 0, PGSIZE);
  cf->held_slot = -1;
  f->private_data = cf;
  acquiresleep(&cam.busy);
  cam.open_count++;
  releasesleep(&cam.busy);
  return 0;
}

static int
camread(struct file *f, int user_dst, uint64 dst, int n)
{
  struct cam_file *cf = f->private_data;
  struct cam_frame_hdr hdr;
  int slot = -1;
  int r = -1;

  acquiresleep(&cam.busy);
  // In mmap mode a slot stays owned by userspace until its next read (or
  // close), so the pixels cannot be overwritten while it is processing them.
  if(cf && cf->held_slot >= 0){
    acquire(&cam.lock);
    release_slot_locked(cf->held_slot);
    release(&cam.lock);
    cf->held_slot = -1;
  }
  // The software SCCB bus on clone camera modules can miss the boot-time
  // probe.  /dev/video0 remains registered, so retry binding on first use
  // instead of leaving the device permanently inactive until reboot.
  if((cam.sensor == 0 && camera_bind() < 0) ||
     (n != (int)sizeof(hdr) &&
      n < (int)(sizeof(hdr) + cam.frame_bytes)))
    goto out;
  // First read powers the sensor and starts it streaming (one warm-up); every
  // later read grabs a fresh frame without another power cycle.  release()
  // stops the stream when the last reference to /dev/video0 is dropped.
  if(!cam.streaming && cam_stream_start() < 0)
    goto out;
  if(cam_grab(&hdr, &slot) == 0 &&
     either_copyout(user_dst, dst, &hdr, sizeof(hdr)) == 0){
    if(n == (int)sizeof(hdr) && cf && myproc()->vm->camera_mapped){
      // Header-only read is the zero-copy dequeue operation.  hdr.reserved
      // selects CAM_MMAP_BASE + slot*CAM_SLOT_BYTES.
      cf->held_slot = slot;
      r = sizeof(hdr);
      slot = -1;
    } else if(either_copyout(user_dst, dst + sizeof(hdr), FRAME_VA(slot),
                             cam.frame_bytes) == 0){
      r = sizeof(hdr) + cam.frame_bytes;
    }
  }
  if(slot >= 0){
    acquire(&cam.lock);
    release_slot_locked(slot);
    release(&cam.lock);
  }
out:
  releasesleep(&cam.busy);
  return r;
}

// Stop the continuous stream when the last reference to /dev/video0 is
// dropped, so the sensor is not left powered between program runs.
static void
camrelease(struct file *f)
{
  struct cam_file *cf = f->private_data;

  acquiresleep(&cam.busy);
  if(cf && cf->held_slot >= 0){
    acquire(&cam.lock);
    release_slot_locked(cf->held_slot);
    release(&cam.lock);
  }
  if(cam.open_count > 0 && --cam.open_count == 0)
    cam_stream_stop();
  releasesleep(&cam.busy);
  if(cf)
    kfree(cf);
  f->private_data = 0;
}

// "pattern=N" selects a sensor test pattern for later captures.  The value
// is checked and cached by the sensor (set_ctrl) and applied at stream_on.
static int
camwrite(struct file *f, int user_src, uint64 src, int n)
{
  char buf[16];
  int value = 0, i, r;

  (void)f;
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

static uint64
cammmap(struct file *f, uint64 addr, uint64 len, int prot, int flags,
        uint64 off)
{
  struct cam_file *cf = f->private_data;
  struct vmspace *vm = myproc()->vm;
  uint64 va = addr ? addr : CAM_MMAP_BASE;

  if(cf == 0 || va != CAM_MMAP_BASE || off != 0 ||
     len != CAM_MMAP_BYTES || prot != PROT_READ ||
     !(flags & MAP_SHARED))
    return (uint64)-1;

  acquire(&vm->lock);
  if(vm->camera_mapped){
    release(&vm->lock);
    return (uint64)-1;
  }
  for(uint64 a = va; a < va + len; a += PGSIZE){
    pte_t *pte = walk(vm->pagetable, a, 0);
    if(pte && (*pte & PTE_V)){
      release(&vm->lock);
      return (uint64)-1;
    }
  }
  // Use the same Normal-NC attribute as the kernel alias.  Mixing cacheable
  // and non-cacheable aliases for one physical DMA page is architecturally
  // unsafe; RO applies only to CPU page-table accesses, not Unicam AXI writes.
  if(mappages(vm->pagetable, va, len, CAMDMA_PA,
              PTE_NORMAL_NC | PTE_URO | PTE_XN) < 0){
    // mappages() may have installed a prefix before a page-table allocation
    // failed.  Reserved DMA pages are never freed; remove only those leaves.
    for(uint64 a = va; a < va + len; a += PGSIZE){
      pte_t *pte = walk(vm->pagetable, a, 0);
      if(pte && (*pte & PTE_V) && (*pte & PTE_AF))
        *pte = 0;
    }
    flush_tlb();
    release(&vm->lock);
    return (uint64)-1;
  }
  flush_tlb();
  vm->camera_mapped = 1;
  release(&vm->lock);
  return va;
}

static int
cammunmap(struct file *f, uint64 addr, uint64 len)
{
  struct cam_file *cf = f->private_data;

  if(cf == 0 || addr != CAM_MMAP_BASE || len != CAM_MMAP_BYTES)
    return -1;
  acquiresleep(&cam.busy);
  if(cf->held_slot >= 0){
    acquire(&cam.lock);
    release_slot_locked(cf->held_slot);
    release(&cam.lock);
    cf->held_slot = -1;
  }
  releasesleep(&cam.busy);
  return camera_munmap_current(addr, len);
}

int
camera_munmap_current(uint64 addr, uint64 len)
{
  struct vmspace *vm = myproc()->vm;

  if(addr != CAM_MMAP_BASE || len != CAM_MMAP_BYTES)
    return -1;
  acquire(&vm->lock);
  if(!vm->camera_mapped){
    release(&vm->lock);
    return -1;
  }
  uvmunmap(vm->pagetable, addr, len / PGSIZE, 0);
  flush_tlb();
  vm->camera_mapped = 0;
  release(&vm->lock);
  return 0;
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
    .open = camopen,
    .fread = camread,
    .fwrite = camwrite,
    .release = camrelease,
    .mmap = cammmap,
    .munmap = cammunmap,
  };

  initlock(&cam.lock, "unicam");
  initsleeplock(&cam.busy, "camera");
  init_workqueue(&cam.wq, "camera_wq", 1);
  init_delayed_work(&cam.poll_work, unicam_poll_work);
  cam.state = CAM_IDLE;
  cam.active_slot = cam.next_slot = -1;
  // IRQ 39 is the primary receive-completion path.  The delayed worker keeps
  // captures functional if a board does not route the legacy CSI interrupt;
  // unicam_irq() masks a genuinely stuck/spurious line after 100 assertions.
  cam.irq_off = 0;
  bcm2837_disable_irq(CSI1_IRQ);
  printf("camera: driver gpio-sccb-v8 unicam-irq-v3 double-buffer mmap\n");
  if(register_chrdev(CAMERA, "video0", &fops) < 0){
    printf("unicam: cannot register /dev/video0\n");
    return;
  }
  if(camera_bind() == 0)
    return;
  printf("camera: no sensor found (%d driver%s tried); /dev/video0 "
         "inactive\n", cam.ncandidates, cam.ncandidates == 1 ? "" : "s");
}
