// Native VC4 scanout for BCM2837 / Raspberry Pi 3.
//
// The boot firmware still establishes the HDMI link and 640x480 pixel clock.
// From this point on, however, pixels come from normal ARM RAM allocated by
// the buddy allocator.  The kernel writes HVS display lists itself, programs
// HVS FIFO 1 and Pixel Valve 2, and completes flips from the PV2 vblank IRQ.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/defs.h>
#include <kernel/page.h>
#include <kernel/bcm2708_fb.h>
#include <kernel/vc4_native.h>

#define VC4_HVS_BASE       (PERIPHERAL_BASE + 0x00400000UL)
#define VC4_PV2_BASE       (PERIPHERAL_BASE + 0x00807000UL)

#define HVS_DISPCTRL       0x0000
#define HVS_DISPSTAT       0x0004
#define HVS_DISPLIST1      0x0024
#define HVS_DISPLACT1      0x0034
#define HVS_DISPCTRL1      0x0050
#define HVS_DISPBKGND1     0x0054
#define HVS_DISPSTAT1      0x0058
#define HVS_DISPBASE1      0x005c
#define HVS_DLIST_START    0x2000

#define HVS_GLOBAL_ENABLE  (1U << 31)
#define HVS_DMA_ERROR      (1U << 7)
#define HVS_SLAVE_ERRORS   ((1U << 6) | (1U << 5))
#define HVS_RESPONSE_ERROR (3U << 14)
#define HVS_CH_ENABLE      (1U << 31)
#define HVS_CH_RESET       (1U << 30)
#define HVS_AUTOHS         (1U << 31)
#define HVS_INTERLACE      (1U << 30)
#define HVS_CTL_END        (1U << 31)
#define HVS_CTL_VALID      (1U << 30)
#define HVS_CTL_SIZE(n)    ((uint32)(n) << 24)
#define HVS_CTL_ORDER_ABGR (3U << 13)
#define HVS_CTL_EXPAND     (3U << 11)
#define HVS_CTL_UNITY      (1U << 4)
#define HVS_FMT_RGBA8888   7U
#define HVS_POS0_ALPHA     (0xffU << 24)
#define HVS_POS2_FIXED     (1U << 30)

#define PV_CONTROL         0x00
#define PV_V_CONTROL       0x04
#define PV_VSYNCD_EVEN     0x08
#define PV_HORZA           0x0c
#define PV_HORZB           0x10
#define PV_VERTA           0x14
#define PV_VERTB           0x18
#define PV_INTEN           0x24
#define PV_INTSTAT         0x28

#define PV_EN              (1U << 0)
#define PV_FIFO_CLR        (1U << 1)
#define PV_CLK_HDMI        (1U << 2)
#define PV_WAIT_HSTART     (1U << 12)
#define PV_UNDERFLOW       (1U << 13)
#define PV_CLR_AT_START    (1U << 14)
#define PV_FIFO_LEVEL(n)   ((uint32)(n) << 15)
#define PV_VIDEN           (1U << 0)
#define PV_CONTINUOUS      (1U << 1)
#define PV_INT_VFP_START   (1U << 7)

#define VC4_WIDTH          640U
#define VC4_HEIGHT         480U
#define VC4_DEPTH          32U
#define VC4_PITCH          (VC4_WIDTH * 4U)
#define VC4_FRAME_BYTES    (VC4_PITCH * VC4_HEIGHT)
#define VC4_SCANOUT_ORDER  10              // 4 MiB, physically contiguous
#define VC4_SCANOUT_BYTES  ((1UL << VC4_SCANOUT_ORDER) * PGSIZE)
#define VC4_DLIST0         32U             // firmware reservation ends here
#define VC4_DLIST1         48U
#define VC4_DLIST_WORDS    7U
#define VC4_HVS_CHANNEL    1U

struct vc4_native_state {
  struct spinlock lock;
  int active;
  void *scanout_kva;
  uint64 scanout_pa;
  uint32 front;
  uint64 sequence;
  uint32 old_dispctrl;
  uint32 old_displist;
  uint32 old_chctrl;
  uint32 old_bkgnd;
  uint32 old_base;
  uint32 old_pv_control;
  uint32 old_pv_vcontrol;
  uint32 old_pv_horza;
  uint32 old_pv_horzb;
  uint32 old_pv_verta;
  uint32 old_pv_vertb;
};

static struct vc4_native_state native;

static inline uint32
hvs_read(uint32 off)
{
  return *(volatile uint32 *)(VC4_HVS_BASE + off);
}

static inline void
hvs_write(uint32 off, uint32 value)
{
  *(volatile uint32 *)(VC4_HVS_BASE + off) = value;
}

static inline uint32
pv_read(uint32 off)
{
  return *(volatile uint32 *)(VC4_PV2_BASE + off);
}

static inline void
pv_write(uint32 off, uint32 value)
{
  *(volatile uint32 *)(VC4_PV2_BASE + off) = value;
}

static void
cache_clean_range(void *buf, uint64 len)
{
  uint64 p = (uint64)buf & ~63ULL;
  uint64 end = ((uint64)buf + len + 63) & ~63ULL;

  for(; p < end; p += 64)
    asm volatile("dc cvac, %0" :: "r"(p) : "memory");
  asm volatile("dsb sy" ::: "memory");
}

static uint32
hvs_frame_count(void)
{
  // BCM2835/6/7 HVS4: FIFO 1 frame counter is DISPSTAT1[17:12].
  return (hvs_read(HVS_DISPSTAT1) >> 12) & 0x3fU;
}

static int
wait_frame_change(uint32 before)
{
  uint64 deadline = r_cntvct_el0() + r_cntfrq_el0() / 5;

  while(hvs_frame_count() == before){
    if(r_cntvct_el0() >= deadline)
      return -1;
    asm volatile("yield" ::: "memory");
  }
  return 0;
}

static int
wait_pv_clear(uint32 off, uint32 mask)
{
  uint64 deadline = r_cntvct_el0() + r_cntfrq_el0() / 100;

  while(pv_read(off) & mask){
    if(r_cntvct_el0() >= deadline)
      return -1;
    asm volatile("yield" ::: "memory");
  }
  return 0;
}

static void
write_dlist(uint32 index, uint64 pa)
{
  volatile uint32 *dlist = (volatile uint32 *)(VC4_HVS_BASE +
                                               HVS_DLIST_START);

  // XRGB8888 in CPU memory is ABGR byte order from the HVS point of view.
  dlist[index + 0] = HVS_CTL_VALID | HVS_CTL_SIZE(VC4_DLIST_WORDS) |
                     HVS_CTL_ORDER_ABGR | HVS_CTL_EXPAND |
                     HVS_CTL_UNITY | HVS_FMT_RGBA8888;
  dlist[index + 1] = HVS_POS0_ALPHA; // x=0, y=0, fixed alpha=255
  dlist[index + 2] = HVS_POS2_FIXED |
                     (VC4_HEIGHT << 16) | VC4_WIDTH;
  dlist[index + 3] = 0xc0c0c0c0U;  // context word, owned by the HVS
  dlist[index + 4] = 0xc0000000U | (uint32)pa;
  dlist[index + 5] = 0xc0c0c0c0U;  // pointer context word
  dlist[index + 6] = VC4_PITCH;
  dlist[index + VC4_DLIST_WORDS] = HVS_CTL_END;
  asm volatile("dsb sy" ::: "memory");
}

static void
save_registers(void)
{
  native.old_dispctrl = hvs_read(HVS_DISPCTRL);
  native.old_displist = hvs_read(HVS_DISPLIST1);
  native.old_chctrl = hvs_read(HVS_DISPCTRL1);
  native.old_bkgnd = hvs_read(HVS_DISPBKGND1);
  native.old_base = hvs_read(HVS_DISPBASE1);
  native.old_pv_control = pv_read(PV_CONTROL);
  native.old_pv_vcontrol = pv_read(PV_V_CONTROL);
  native.old_pv_horza = pv_read(PV_HORZA);
  native.old_pv_horzb = pv_read(PV_HORZB);
  native.old_pv_verta = pv_read(PV_VERTA);
  native.old_pv_vertb = pv_read(PV_VERTB);
}

static void
restore_registers(void)
{
  pv_write(PV_INTEN, 0);
  pv_write(PV_CONTROL, native.old_pv_control & ~PV_EN);
  pv_write(PV_V_CONTROL, native.old_pv_vcontrol & ~PV_VIDEN);
  hvs_write(HVS_DISPCTRL1, 0);
  hvs_write(HVS_DISPBASE1, native.old_base);
  hvs_write(HVS_DISPBKGND1, native.old_bkgnd);
  hvs_write(HVS_DISPLIST1, native.old_displist);
  hvs_write(HVS_DISPCTRL1, native.old_chctrl);
  hvs_write(HVS_DISPCTRL, native.old_dispctrl);
  pv_write(PV_HORZA, native.old_pv_horza);
  pv_write(PV_HORZB, native.old_pv_horzb);
  pv_write(PV_VERTA, native.old_pv_verta);
  pv_write(PV_VERTB, native.old_pv_vertb);
  pv_write(PV_CONTROL, native.old_pv_control);
  pv_write(PV_V_CONTROL, native.old_pv_vcontrol);
  asm volatile("dsb sy" ::: "memory");
}

static void
program_hvs(void)
{
  uint32 bkgnd;

  hvs_write(HVS_DISPCTRL1, 0);
  hvs_write(HVS_DISPCTRL1, HVS_CH_RESET);
  hvs_write(HVS_DISPCTRL1, 0);

  // HVS4 has a 20736-pixel Composite Output Buffer.  Match Linux's split:
  // FIFO2=0..6143, FIFO1=6144..12287, FIFO0=12288..20735.
  hvs_write(HVS_DISPBASE1, (12287U << 16) | 6144U);
  bkgnd = native.old_bkgnd | HVS_AUTOHS;
  bkgnd &= ~HVS_INTERLACE;
  hvs_write(HVS_DISPBKGND1, bkgnd);
  hvs_write(HVS_DISPLIST1, VC4_DLIST0);
  hvs_write(HVS_DISPCTRL, native.old_dispctrl | HVS_GLOBAL_ENABLE);
  hvs_write(HVS_DISPCTRL1, HVS_CH_ENABLE |
                              (VC4_WIDTH << 12) | VC4_HEIGHT);
}

static int
program_pixel_valve(void)
{
  // 640x480@60 timing.  The firmware allocation has already selected the
  // matching HDMI pixel clock and PHY mode; PV2 and everything upstream are
  // now programmed directly by the kernel.
  pv_write(PV_V_CONTROL, native.old_pv_vcontrol & ~PV_VIDEN);
  pv_write(PV_CONTROL, native.old_pv_control & ~PV_EN);
  if(wait_pv_clear(PV_V_CONTROL, PV_VIDEN) < 0)
    return -1;
  pv_write(PV_CONTROL, (native.old_pv_control & ~PV_EN) | PV_FIFO_CLR);
  if(wait_pv_clear(PV_CONTROL, PV_FIFO_CLR) < 0)
    return -1;
  pv_write(PV_HORZA, (48U << 16) | 96U);  // back porch | hsync
  pv_write(PV_HORZB, (16U << 16) | VC4_WIDTH); // front porch | active
  pv_write(PV_VERTA, (33U << 16) | 2U);  // back porch | vsync
  pv_write(PV_VERTB, (10U << 16) | VC4_HEIGHT); // front porch | active
  pv_write(PV_VSYNCD_EVEN, 0);
  pv_write(PV_CONTROL, PV_FIFO_LEVEL(45) | PV_CLR_AT_START |
                        PV_UNDERFLOW | PV_WAIT_HSTART | PV_CLK_HDMI);
  pv_write(PV_CONTROL, pv_read(PV_CONTROL) | PV_EN);
  pv_write(PV_V_CONTROL, PV_CONTINUOUS | PV_VIDEN);
  asm volatile("dsb sy" ::: "memory");
  return 0;
}

int
vc4_native_kms_init(const struct bcm2708_fb_info *firmware_mode,
                    struct bcm2708_fb_info *scanout)
{
  uint32 before;

  if(firmware_mode == 0 || scanout == 0 ||
     firmware_mode->width != VC4_WIDTH ||
     firmware_mode->height != VC4_HEIGHT ||
     firmware_mode->depth != VC4_DEPTH)
    return -1;

  initlock(&native.lock, "vc4-native");
  save_registers();
  // QEMU's raspi3 framebuffer implements the mailbox path but not the VC4
  // HVS/PV blocks.  Zero control registers therefore select the safe firmware
  // fallback instead of writing into an unmodelled device.
  if((native.old_pv_control & PV_EN) == 0 ||
     (native.old_pv_vcontrol & PV_VIDEN) == 0){
    printf("vc4-native: PV2/HVS unavailable; using firmware backend\n");
    return -1;
  }

  native.scanout_kva = __get_free_pages(VC4_SCANOUT_ORDER);
  if(native.scanout_kva == 0){
    printf("vc4-native: no 4 MiB contiguous scanout block\n");
    return -1;
  }
  native.scanout_pa = V2P(native.scanout_kva);
  memset(native.scanout_kva, 0, VC4_SCANOUT_BYTES);
  cache_clean_range(native.scanout_kva, VC4_SCANOUT_BYTES);

  write_dlist(VC4_DLIST0, native.scanout_pa);
  write_dlist(VC4_DLIST1, native.scanout_pa + VC4_FRAME_BYTES);
  // DISPSTAT is write-one-to-clear.  Do not mistake a stale firmware-side
  // flag for a failure caused by the new display list.
  hvs_write(HVS_DISPSTAT, 0xffffffffU);
  before = hvs_frame_count();
  program_hvs();
  if(program_pixel_valve() < 0 || wait_frame_change(before) < 0 ||
     (hvs_read(HVS_DISPSTAT) &
      (HVS_DMA_ERROR | HVS_SLAVE_ERRORS | HVS_RESPONSE_ERROR))){
    restore_registers();
    free_pages(native.scanout_kva, VC4_SCANOUT_ORDER);
    native.scanout_kva = 0;
    printf("vc4-native: takeover failed; restored firmware scanout\n");
    return -1;
  }

  native.front = 0;
  native.sequence = 0;
  native.active = 1;
  asm volatile("dmb sy" ::: "memory");
  pv_write(PV_INTSTAT, PV_INT_VFP_START);
  pv_write(PV_INTEN, PV_INT_VFP_START);
  bcm2837_enable_irq(PV2_IRQ);

  memset(scanout, 0, sizeof(*scanout));
  scanout->width = VC4_WIDTH;
  scanout->height = VC4_HEIGHT;
  scanout->virtual_height = VC4_HEIGHT * 2;
  scanout->depth = VC4_DEPTH;
  scanout->pitch = VC4_PITCH;
  scanout->base = native.scanout_pa;
  scanout->size = VC4_SCANOUT_BYTES;
  scanout->frame_bytes = VC4_FRAME_BYTES;
  scanout->buffer_count = 2;
  printf("vc4-native: HVS1/PV2 owns scanout pa=%p, IRQ %d\n",
         native.scanout_pa, PV2_IRQ);
  return 0;
}

int
vc4_native_is_active(void)
{
  return native.active;
}

void
vc4_native_irq(void)
{
  uint32 status = pv_read(PV_INTSTAT);

  if(!native.active || (status & PV_INT_VFP_START) == 0)
    return;
  pv_write(PV_INTSTAT, PV_INT_VFP_START);
  acquire(&native.lock);
  native.sequence++;
  release(&native.lock);
  wakeup(&native.sequence);
}

int
vc4_native_wait_vblank(uint64 *sequence)
{
  uint64 target;

  if(!native.active)
    return -1;
  acquire(&native.lock);
  target = native.sequence + 1;
  while(native.sequence < target)
    sleep(&native.sequence, &native.lock);
  if(sequence)
    *sequence = native.sequence;
  release(&native.lock);
  return 0;
}

int
vc4_native_page_flip(uint32 slot, uint64 *sequence)
{
  uint64 target;
  uint32 dlist;

  if(!native.active || slot >= 2)
    return -1;
  dlist = slot ? VC4_DLIST1 : VC4_DLIST0;
  acquire(&native.lock);
  target = native.sequence + 1;
  hvs_write(HVS_DISPLIST1, dlist);
  asm volatile("dsb sy" ::: "memory");
  while(native.sequence < target)
    sleep(&native.sequence, &native.lock);
  if(hvs_read(HVS_DISPLACT1) != dlist){
    release(&native.lock);
    return -1;
  }
  native.front = slot;
  if(sequence)
    *sequence = native.sequence;
  release(&native.lock);
  return 0;
}
