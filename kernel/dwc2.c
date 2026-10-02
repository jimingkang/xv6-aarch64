// BCM2837 DWC2 USB host controller, DMA transport and device enumeration.
// Class/network policy lives in usbnet.c; this file only publishes USB devices
// and provides the host-controller transfer engine.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "device.h"
#include "usb.h"
#include "workqueue.h"

#define DWC2_BASE (PERIPHERAL_BASE + 0x00980000UL)
#define USB_MBOX_BASE (PERIPHERAL_BASE + 0x0000b880UL)

#define GAHBCFG   0x008
#define GUSBCFG   0x00c
#define GRSTCTL   0x010
#define GINTSTS   0x014
#define GINTMSK   0x018
#define GSNPSID   0x040
#define HCFG      0x400
#define HFNUM     0x408
#define HAINT     0x414
#define HAINTMSK  0x418
#define HPRT0     0x440
#define HCCHAR(c) (0x500 + 0x20*(c))
#define HCSPLT(c) (0x504 + 0x20*(c))
#define HCINT(c)  (0x508 + 0x20*(c))
#define HCINTMSK(c) (0x50c + 0x20*(c))
#define HCTSIZ(c) (0x510 + 0x20*(c))
#define HCDMA(c)  (0x514 + 0x20*(c))

#define GAHBCFG_DMA_EN       (1U << 5)
#define GAHBCFG_GLBL_INTR_EN (1U << 0)
#define GINTSTS_HCHINT       (1U << 25)
#define GUSBCFG_FORCEHOST    (1U << 29)
#define GUSBCFG_FORCEDEV     (1U << 30)
#define GRSTCTL_AHBIDLE      (1U << 31)
#define GRSTCTL_CSFTRST      (1U << 0)
#define GINTSTS_CURMODE_HOST (1U << 0)
#define HPRT_PWR             (1U << 12)
#define HPRT_RST             (1U << 8)
#define HPRT_ENA             (1U << 2)
#define HPRT_CONNDET         (1U << 1)
#define HPRT_CONNSTS         (1U << 0)
#define HPRT_W1C             ((1U << 5) | (1U << 3) | (1U << 1))

#define HCCHAR_CHENA         (1U << 31)
#define HCCHAR_CHDIS         (1U << 30)
#define HCCHAR_DEVADDR(a)    ((uint32)(a) << 22)
#define HCCHAR_EPTYPE(t)     ((uint32)(t) << 18)
#define HCCHAR_LSPDDEV       (1U << 17)
#define HCCHAR_EPDIR_IN      (1U << 15)
#define HCCHAR_EPNUM(e)      ((uint32)(e) << 11)
#define HCCHAR_MPS(n)        ((uint32)(n))
#define EPTYPE_CONTROL       0
#define EPTYPE_BULK          2
#define EPTYPE_INTERRUPT     3

#define HCINT_XFERCOMPL      (1U << 0)
#define HCINT_CHHLTD         (1U << 1)
#define HCINT_STALL          (1U << 3)
#define HCINT_NAK            (1U << 4)
#define HCINT_ACK            (1U << 5)
#define HCINT_NYET           (1U << 6)
#define HCINT_XACTERR        (1U << 7)
#define HCINT_DTERR          (1U << 10)
#define HCINT_ERRORS         ((1U << 2) | HCINT_STALL | HCINT_XACTERR | \
                              (1U << 8) | (1U << 9) | HCINT_DTERR)

#define PID_DATA0            0
#define PID_DATA1            2
#define PID_SETUP            3
#define HCTSIZ_PID(p)        ((uint32)(p) << 29)
#define HCTSIZ_PKTCNT(n)     ((uint32)(n) << 19)
#define HCTSIZ_XFERSIZE(n)   ((uint32)(n) & 0x7ffff)
#define HCSPLT_SPLTENA       (1U << 31)
#define HCSPLT_COMPSPLT      (1U << 16)
#define HCSPLT_XACTPOS_ALL   (3U << 14)
#define HCSPLT_HUBADDR(a)    ((uint32)(a) << 7)
#define HCSPLT_PRTADDR(p)    ((uint32)(p))

#define USB_GET_DESCRIPTOR   6
#define USB_SET_ADDRESS      5
#define USB_SET_CONFIG       9
#define USB_DT_DEVICE        1
#define USB_DT_CONFIG        2
#define USB_DT_HUB           0x29
#define USB_REQ_GET_STATUS   0
#define USB_REQ_CLEAR_FEATURE 1
#define USB_REQ_SET_FEATURE  3
#define HUB_PORT_RESET       4
#define HUB_PORT_POWER       8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_RESET     20
#define HUB_PORT_CONNECTION  (1U << 0)
#define HUB_PORT_ENABLE      (1U << 1)
#define HUB_PORT_RESET_STAT  (1U << 4)
#define HUB_PORT_LOW_SPEED   (1U << 9)
#define HUB_PORT_HIGH_SPEED  (1U << 10)

#define USB_BUF_SIZE 2048
#define MBOX_READ       0x00
#define MBOX_STATUS     0x18
#define MBOX_WRITE      0x20
#define MBOX_EMPTY      0x40000000U
#define MBOX_FULL       0x80000000U
#define MBOX_PROP_CH    8U
#define MBOX_SET_POWER_STATE 0x00028001U
#define MBOX_POWER_USB_HCD   3U

// DWC2 is a bus master.  BCM2837 RAM physical addresses 0x00000000..
// are presented to VideoCore-side bus masters through the 0xc0000000 alias.
// QEMU also accepts this form, while real hardware cannot DMA from a kernel
// high virtual address or an untranslated ARM physical pointer.
#define DWC2_DMA_BUS(a) (0xc0000000U | (uint32)V2P(a))

struct usb_setup {
  uint8 type;
  uint8 request;
  uint16 value;
  uint16 index;
  uint16 length;
} __attribute__((packed));

static struct spinlock usb_lock;
static int usb_ready;
static int usb_address;
static int ep0_mps = 8;
static int bulk_in_toggle;
static int bulk_out_toggle;
static int rx_armed;
static volatile uint32 dwc2_irq_pending;
static struct {
  uint8 hub;
  uint8 port;
  uint8 low_speed;
} usb_route[128];
static struct dwc2_interrupt_request {
  struct urb *urb;
  struct work_struct work;
  int mps;
  int active;
  int complete_split;
  int csplit_retries;
  uint32 last_issue_frame;
  uint32 ssplit_ack_frame;
} interrupt_rx;
static uint interrupt_rx_errors;
static uint interrupt_rx_irq_logs;
static int interrupt_rx_arm_logged;
#define DWC2_RX_REQUESTS 4
#define RX_FREE   0
#define RX_QUEUED 1
#define RX_ACTIVE 2
#define RX_DONE   3
static struct dwc2_rx_request {
  struct usb_device *udev;
  void *buffer;
  int length;
  int endpoint;
  int mps;
  int result;
  int state;
} async_rx[DWC2_RX_REQUESTS];
static int async_rx_active = -1;
static uchar setup_buf[64] __attribute__((aligned(64)));
static uchar ctrl_buf[512] __attribute__((aligned(64)));
static uchar rx_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static uchar rx_deliver_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static uchar tx_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static struct device *usb_parent;
static struct usb_device usb_child;
static int usb_child_registered;
static int usb_next_address = 8;
static uint32 usb_power_message[8] __attribute__((aligned(64)));

struct hid_selection {
  int found;
  int address;
  int ep0_mps;
  uint16 vid;
  uint16 pid;
  uint8 interface_number;
};

static inline uint32
rd(uint32 off)
{
  return *(volatile uint32*)(DWC2_BASE + off);
}

static inline void
wr(uint32 off, uint32 value)
{
  *(volatile uint32*)(DWC2_BASE + off) = value;
}

static int
usb_firmware_power_on(void)
{
  uint32 *m = usb_power_message;
  uint32 request, response;
  uint64 deadline;

  memset(m, 0, sizeof(usb_power_message));
  m[0] = sizeof(usb_power_message);
  m[1] = 0;
  m[2] = MBOX_SET_POWER_STATE;
  m[3] = 8;
  m[4] = 8;
  m[5] = MBOX_POWER_USB_HCD;
  m[6] = 3; // bit 0: on, bit 1: wait for the transition
  m[7] = 0;
  asm volatile("dc cvac, %0\n\tdsb sy" :: "r"(m) : "memory");
  request = ((uint32)V2P(m) & ~0xfU) | MBOX_PROP_CH;
  deadline = r_cntvct_el0() + r_cntfrq_el0();
  while(*(volatile uint32*)(USB_MBOX_BASE + MBOX_STATUS) & MBOX_FULL)
    if(r_cntvct_el0() >= deadline)
      return -1;
  *(volatile uint32*)(USB_MBOX_BASE + MBOX_WRITE) = request;
  asm volatile("dmb sy" ::: "memory");
  for(;;){
    while(*(volatile uint32*)(USB_MBOX_BASE + MBOX_STATUS) & MBOX_EMPTY)
      if(r_cntvct_el0() >= deadline)
        return -1;
    response = *(volatile uint32*)(USB_MBOX_BASE + MBOX_READ);
    if((response & 0xfU) == MBOX_PROP_CH)
      break;
  }
  asm volatile("dc ivac, %0\n\tdsb sy" :: "r"(m) : "memory");
  if(m[1] != 0x80000000U || (m[4] & 0x80000000U) == 0 ||
     m[5] != MBOX_POWER_USB_HCD || (m[6] & 1) == 0)
    return -1;
  return 0;
}

static void
udelay(uint32 us)
{
  uint64 ticks = ((uint64)r_cntfrq_el0() * us + 999999) / 1000000;
  uint64 start = r_cntvct_el0();
  while(r_cntvct_el0() - start < ticks)
    asm volatile("yield" ::: "memory");
}

static void
cache_clean_range(void *buf, int len)
{
  uint64 p, end;
  if(len == 0)
    return;
  p = (uint64)buf & ~63ULL;
  end = ((uint64)buf + len + 63) & ~63ULL;
  for(; p < end; p += 64)
    asm volatile("dc cvac, %0" :: "r"(p) : "memory");
  asm volatile("dsb sy" ::: "memory");
}

static void
cache_invalidate_range(void *buf, int len)
{
  uint64 p, end;
  if(len == 0)
    return;
  p = (uint64)buf & ~63ULL;
  end = ((uint64)buf + len + 63) & ~63ULL;
  asm volatile("dsb sy" ::: "memory");
  for(; p < end; p += 64)
    asm volatile("dc ivac, %0" :: "r"(p) : "memory");
  asm volatile("dsb sy" ::: "memory");
}

static void
cache_clean_invalidate_range(void *buf, int len)
{
  uint64 p, end;
  if(len == 0)
    return;
  p = (uint64)buf & ~63ULL;
  end = ((uint64)buf + len + 63) & ~63ULL;
  for(; p < end; p += 64)
    asm volatile("dc civac, %0" :: "r"(p) : "memory");
  asm volatile("dsb sy" ::: "memory");
}

static void
halt_channel(int ch)
{
  uint32 c = rd(HCCHAR(ch));
  if(c & HCCHAR_CHENA){
    wr(HCCHAR(ch), c | HCCHAR_CHENA | HCCHAR_CHDIS);
    for(int i = 0; i < 10000; i++)
      if(!(rd(HCCHAR(ch)) & HCCHAR_CHENA))
        break;
  }
}

// Return actual byte count, -2 for NAK, -3 for a DATA-toggle mismatch,
// and -1 for another hard USB error.
static int
channel_xfer(int ch, int addr, int ep, int in, int type, int mps,
             void *buf, int len, int pid, int poll_us)
{
  uint32 intr, hcchar, left;
  int packets = len ? (len + mps - 1) / mps : 1;
  int saw_nak = 0;
  int split_ack_logs = 0, split_nak_logs = 0;
  int split, complete_split = 0;
  uint64 deadline;

  split = addr >= 0 && addr < 128 && usb_route[addr].hub != 0;
  if(split && len > mps && pid != PID_SETUP){
    int done = 0, current_pid = pid, r;
    while(done < len){
      int chunk = len - done;
      if(chunk > mps)
        chunk = mps;
      r = channel_xfer(ch, addr, ep, in, type, mps,
                       (uchar*)buf + done, chunk, current_pid, poll_us);
      if(r < 0)
        return r;
      done += r;
      if(r < chunk)
        break;
      current_pid = current_pid == PID_DATA0 ? PID_DATA1 : PID_DATA0;
    }
    return done;
  }

  halt_channel(ch);
  wr(HCINT(ch), 0x3fff);
  if(in)
    // Preserve any CPU writes (for example a poison/zero pattern used to
    // detect a short DMA) before invalidating ownership for the device.
    cache_clean_invalidate_range(buf, len);
  else
    cache_clean_range(buf, len);
  deadline = r_cntvct_el0() +
             ((uint64)r_cntfrq_el0() * poll_us + 999999) / 1000000;

restart_channel:
  split = addr >= 0 && addr < 128 && usb_route[addr].hub != 0;
  if(split)
    wr(HCSPLT(ch), HCSPLT_SPLTENA | HCSPLT_XACTPOS_ALL |
                    HCSPLT_HUBADDR(usb_route[addr].hub) |
                    HCSPLT_PRTADDR(usb_route[addr].port) |
                    (complete_split ? HCSPLT_COMPSPLT : 0));
  else
    wr(HCSPLT(ch), 0);
  wr(HCDMA(ch), DWC2_DMA_BUS(buf));
  wr(HCTSIZ(ch), HCTSIZ_XFERSIZE(len) | HCTSIZ_PKTCNT(packets) |
                  HCTSIZ_PID(pid));
  hcchar = HCCHAR_DEVADDR(addr) | HCCHAR_EPNUM(ep) |
           HCCHAR_EPTYPE(type) | HCCHAR_MPS(mps);
  if(addr >= 0 && addr < 128 && usb_route[addr].low_speed)
    hcchar |= HCCHAR_LSPDDEV;
  if(in)
    hcchar |= HCCHAR_EPDIR_IN;
  wr(HCCHAR(ch), hcchar | HCCHAR_CHENA);
  for(;;){
    intr = rd(HCINT(ch));
    if(split && !complete_split && (intr & HCINT_ACK) &&
       (intr & HCINT_CHHLTD)){
      if(ch == 0 && split_ack_logs++ < 4)
        printf("dwc2: ch0 SSPLIT ACK addr=%d in=%d hcint=%x hcsplt=%x "
               "hcchar=%x hctsiz=%x hfnum=%x\n",
               addr, in, intr, rd(HCSPLT(ch)), rd(HCCHAR(ch)),
               rd(HCTSIZ(ch)), rd(HFNUM));
      // The transaction translator accepted the start-split.  Ask for the
      // completed full/low-speed transaction in a later microframe.
      halt_channel(ch);
      wr(HCINT(ch), intr);
      complete_split = 1;
      udelay(125);
      goto restart_channel;
    }
    if(split && complete_split && (intr & HCINT_NYET)){
      // Hub transaction translator has not completed it yet.  Re-issue only
      // the complete-split; never repeat the start-split payload.
      halt_channel(ch);
      wr(HCINT(ch), intr);
      if(r_cntvct_el0() >= deadline)
        return -2;
      udelay(125);
      goto restart_channel;
    }
    if(intr & HCINT_NAK){
      if(ch == 0 && split && split_nak_logs++ < 4)
        printf("dwc2: ch0 %s NAK addr=%d in=%d hcint=%x hcsplt=%x "
               "hcchar=%x hctsiz=%x hfnum=%x\n",
               complete_split ? "CSPLIT" : "SSPLIT", addr, in, intr,
               rd(HCSPLT(ch)), rd(HCCHAR(ch)), rd(HCTSIZ(ch)), rd(HFNUM));
      wr(HCINT(ch), intr);
      saw_nak = 1;
      if(r_cntvct_el0() >= deadline){
        halt_channel(ch);
        if(poll_us > 2000)
          printf("dwc2: channel %d NAK timeout addr=%d in=%d hcsplt=%x "
                 "hcchar=%x hcint=%x hctsiz=%x hfnum=%x\n",
                 ch, addr, in, rd(HCSPLT(ch)), rd(HCCHAR(ch)),
                 rd(HCINT(ch)), rd(HCTSIZ(ch)), rd(HFNUM));
        return -2;
      }
      // DWC2 halts a host channel after NAK.  Clearing HCINT and merely
      // polling again leaves the channel stopped forever; resubmit the same
      // transaction on a later frame.  A NAK means the endpoint accepted no
      // data, so the PID and DMA position do not advance.
      halt_channel(ch);
      wr(HCINT(ch), 0x3fff);
      // A NAK ends this split attempt.  DWC2 host handling restarts the
      // transaction with a fresh start-split rather than retrying COMPSPLT.
      if(split)
        complete_split = 0;
      udelay(100);
      goto restart_channel;
    }
    if(intr & HCINT_DTERR){
      uint32 status = rd(HCTSIZ(ch));
      halt_channel(ch);
      wr(HCINT(ch), intr);
      printf("dwc2: channel %d data-toggle error hcchar=%x hctsiz=%x\n",
             ch, rd(HCCHAR(ch)), status);
      return -3;
    }
    if(intr & HCINT_ERRORS){
      halt_channel(ch);
      wr(HCINT(ch), intr);
      printf("dwc2: channel %d error intr=%x hcchar=%x hctsiz=%x\n",
             ch, intr, rd(HCCHAR(ch)), rd(HCTSIZ(ch)));
      return -1;
    }
    if(intr & (HCINT_XFERCOMPL | HCINT_CHHLTD))
      break;
    if(r_cntvct_el0() >= deadline){
      halt_channel(ch);
      if(saw_nak)
        return -2;
      printf("dwc2: channel %d timeout intr=%x hcchar=%x hctsiz=%x "
             "hcdma=%x hfnum=%x\n",
             ch, rd(HCINT(ch)), rd(HCCHAR(ch)), rd(HCTSIZ(ch)),
             rd(HCDMA(ch)), rd(HFNUM));
      return -1;
    }
  }
  left = rd(HCTSIZ(ch)) & 0x7ffff;
  wr(HCINT(ch), intr);
  wr(HCSPLT(ch), 0);
  if(in)
    cache_invalidate_range(buf, len);
  if(!(intr & HCINT_XFERCOMPL) || left > (uint32)len)
    return -1;
  if(ch == 0 && split && in && len > 0)
    printf("dwc2: ch0 CSPLIT complete addr=%d bytes=%d/%d hcint=%x "
           "hctsiz=%x hfnum=%x\n",
           addr, len - left, len, intr, rd(HCTSIZ(ch)), rd(HFNUM));
  // On BCM2837 the DWC2 DMA engine may leave XFERSIZE unchanged for an OUT
  // SETUP transaction even though the device ACKed it and XFERCOMPL is set.
  // An ACK is authoritative for OUT; only IN needs the residual count to
  // report a short packet.
  if(!in)
    return len;
  return len - left;
}

static int
control(int addr, uint8 type, uint8 request, uint16 value, uint16 index,
        void *data, uint16 len)
{
  struct usb_setup *s = (struct usb_setup*)setup_buf;
  int in = (type & 0x80) != 0;
  int r;

  s->type = type;
  s->request = request;
  s->value = value;
  s->index = index;
  s->length = len;
  r = channel_xfer(0, addr, 0, 0, EPTYPE_CONTROL, ep0_mps,
                   s, sizeof(*s), PID_SETUP, 100000);
  if(r != sizeof(*s)){
    printf("dwc2: control setup failed req=%d addr=%d result=%d\n",
           request, addr, r);
    return -1;
  }
  if(len){
    r = channel_xfer(0, addr, 0, in, EPTYPE_CONTROL, ep0_mps,
                     data, len, PID_DATA1, 100000);
    if(r < 0){
      printf("dwc2: control data failed req=%d addr=%d result=%d\n",
             request, addr, r);
      return -1;
    }
  }
  r = channel_xfer(0, addr, 0, !in, EPTYPE_CONTROL, ep0_mps,
                   ctrl_buf, 0, PID_DATA1, 100000);
  if(r < 0)
    printf("dwc2: control status failed req=%d addr=%d result=%d\n",
           request, addr, r);
  return r < 0 ? -1 : 0;
}

static int
get_device_descriptor(int addr, uchar *descriptor)
{
  // USB enumeration starts with only the first eight bytes while EP0 is
  // conservatively assumed to have an 8-byte maximum packet.  Byte 7 then
  // tells us the real bMaxPacketSize0 for the complete descriptor request.
  ep0_mps = 8;
  memset(descriptor, 0, 18);
  if(control(addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
             descriptor, 8) < 0 || descriptor[0] != 18 ||
     descriptor[1] != USB_DT_DEVICE)
    return -1;
  if(descriptor[7] != 8 && descriptor[7] != 16 &&
     descriptor[7] != 32 && descriptor[7] != 64)
    return -1;
  ep0_mps = descriptor[7];
  memset(descriptor, 0, 18);
  if(control(addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
             descriptor, 18) < 0 || descriptor[0] != 18 ||
     descriptor[1] != USB_DT_DEVICE ||
     (descriptor[8] == 0 && descriptor[9] == 0))
    return -1;
  return 0;
}

static int
config_has_boot_keyboard(int addr, struct hid_selection *sel)
{
  int total;

  memset(ctrl_buf, 0, sizeof(ctrl_buf));
  if(control(addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
             ctrl_buf, 9) < 0 || ctrl_buf[1] != USB_DT_CONFIG)
    return 0;
  total = ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8);
  if(total < 9 || total > (int)sizeof(ctrl_buf) ||
     control(addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
             ctrl_buf, total) < 0)
    return 0;
  for(int off = 0; off + 9 <= total && ctrl_buf[off] >= 2;
      off += ctrl_buf[off]){
    if(ctrl_buf[off + 1] == 4 && ctrl_buf[off] >= 9 &&
       ctrl_buf[off + 5] == 3 && ctrl_buf[off + 6] == 1 &&
       ctrl_buf[off + 7] == 1){
      sel->interface_number = ctrl_buf[off + 2];
      return 1;
    }
  }
  return 0;
}

// Search a configured high-speed hub's downstream ports.  This is bounded to
// two extra levels so a malformed topology cannot recurse forever during boot.
static int
find_keyboard_below_hub(int hub_addr, int depth, struct hid_selection *sel)
{
  int total, cfg, nport;

  if(depth > 2)
    return 0;
  memset(ctrl_buf, 0, sizeof(ctrl_buf));
  if(control(hub_addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
             ctrl_buf, 9) < 0 || ctrl_buf[1] != USB_DT_CONFIG)
    return 0;
  total = ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8);
  cfg = ctrl_buf[5];
  if(total < 9 || total > (int)sizeof(ctrl_buf) || cfg == 0 ||
     control(hub_addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
             ctrl_buf, total) < 0 ||
     control(hub_addr, 0x00, USB_SET_CONFIG, cfg, 0, ctrl_buf, 0) < 0 ||
     control(hub_addr, 0xa0, USB_GET_DESCRIPTOR, USB_DT_HUB << 8, 0,
             ctrl_buf, 9) < 0)
    return 0;
  nport = ctrl_buf[2];
  printf("dwc2: scanning nested hub addr=%d depth=%d ports=%d\n",
         hub_addr, depth, nport);
  for(int port = 1; port <= nport; port++)
    control(hub_addr, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_POWER, port,
            ctrl_buf, 0);
  udelay(100000);

  for(int port = 1; port <= nport; port++){
    uint16 status = 0, vid, pid;
    int child_class, child_addr;

    // Some hubs/keyboards need substantially longer than the nominal power
    // good delay on a cold boot.  Wait up to 500 ms for connect/debounce
    // instead of taking one status snapshot and permanently missing it.
    for(int connect_wait = 0; connect_wait < 50; connect_wait++){
      memset(ctrl_buf, 0, 4);
      if(control(hub_addr, 0xa3, USB_REQ_GET_STATUS, 0, port,
                 ctrl_buf, 4) == 0){
        status = ctrl_buf[0] | ((uint16)ctrl_buf[1] << 8);
        if(status & HUB_PORT_CONNECTION)
          break;
      }
      udelay(10000);
    }
    printf("dwc2: nested hub %d port %d status=%x\n",
           hub_addr, port, status);
    if(!(status & HUB_PORT_CONNECTION))
      continue;
    if(control(hub_addr, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_RESET, port,
               ctrl_buf, 0) < 0)
      continue;
    for(int wait = 0; wait < 50; wait++){
      udelay(10000);
      memset(ctrl_buf, 0, 4);
      if(control(hub_addr, 0xa3, USB_REQ_GET_STATUS, 0, port,
                 ctrl_buf, 4) < 0)
        continue;
      status = ctrl_buf[0] | ((uint16)ctrl_buf[1] << 8);
      if(!(status & HUB_PORT_RESET_STAT) && (status & HUB_PORT_ENABLE))
        break;
    }
    if((status & (HUB_PORT_CONNECTION | HUB_PORT_ENABLE)) !=
       (HUB_PORT_CONNECTION | HUB_PORT_ENABLE))
      continue;
    control(hub_addr, 0x23, USB_REQ_CLEAR_FEATURE,
            HUB_C_PORT_CONNECTION, port, ctrl_buf, 0);
    control(hub_addr, 0x23, USB_REQ_CLEAR_FEATURE,
            HUB_C_PORT_RESET, port, ctrl_buf, 0);

    usb_route[0].hub = 0;
    usb_route[0].port = 0;
    usb_route[0].low_speed = 0;
    if(!(status & HUB_PORT_HIGH_SPEED)){
      usb_route[0].hub = hub_addr;
      usb_route[0].port = port;
      usb_route[0].low_speed = (status & HUB_PORT_LOW_SPEED) != 0;
    }
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    if(get_device_descriptor(0, ctrl_buf) < 0){
      printf("dwc2: nested hub %d port %d descriptor failed\n",
             hub_addr, port);
      continue;
    }
    child_class = ctrl_buf[4];
    vid = ctrl_buf[8] | ((uint16)ctrl_buf[9] << 8);
    pid = ctrl_buf[10] | ((uint16)ctrl_buf[11] << 8);
    child_addr = usb_next_address++;
    if(child_addr >= 128 ||
       control(0, 0x00, USB_SET_ADDRESS, child_addr, 0,
               ctrl_buf, 0) < 0)
      continue;
    udelay(5000);
    usb_route[child_addr] = usb_route[0];
    printf("dwc2: nested hub=%d port=%d child class=%d vid=%x pid=%x addr=%d\n",
           hub_addr, port, child_class, vid, pid, child_addr);

    memset(sel, 0, sizeof(*sel));
    if(config_has_boot_keyboard(child_addr, sel)){
      sel->found = 1;
      sel->address = child_addr;
      sel->ep0_mps = ep0_mps;
      sel->vid = vid;
      sel->pid = pid;
      return 1;
    }
    if(child_class == 9 &&
       find_keyboard_below_hub(child_addr, depth + 1, sel))
      return 1;
  }
  return 0;
}

static int
is_mt7601(uint16 vendor, uint16 product)
{
  static const uint16 ids[][2] = {
    {0x0b05,0x17d3}, {0x0e8d,0x760a}, {0x0e8d,0x760b},
    {0x13d3,0x3431}, {0x13d3,0x3434}, {0x148f,0x7601},
    {0x148f,0x760a}, {0x148f,0x760b}, {0x148f,0x760c},
    {0x148f,0x760d}, {0x2001,0x3d04}, {0x2717,0x4106},
    {0x2955,0x0001}, {0x2955,0x1001}, {0x2955,0x1003},
    {0x2a5f,0x1000}, {0x7392,0x7710},
  };
  for(uint i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
    if(ids[i][0] == vendor && ids[i][1] == product)
      return 1;
  return 0;
}

static int
dwc2_usb_control(struct usb_device *udev, uint8 type, uint8 request,
                 uint16 value, uint16 index, void *data, uint16 length)
{
  return control(udev->address, type, request, value, index, data, length);
}

static int
dwc2_usb_bulk(struct usb_device *udev, int endpoint, int in,
              void *data, int length)
{
  int mps, packets, pid, r, attempt, channel;
  uint8 *toggle;
  if(endpoint <= 0 || endpoint > 15 || length < 0)
    return -1;
  mps = in ? udev->bulk_in_max_packet : udev->bulk_out_max_packet;
  if(mps == 0)
    mps = 64;
  if(in)
    toggle = endpoint == udev->bulk_in_ep2 ? &udev->bulk_in_toggle2 :
                                             &udev->bulk_in_toggle;
  else
    toggle = &udev->bulk_out_toggle;
  // Keep endpoint roles on independent host channels: EP0=0, generic/MCU
  // response IN=3, data RX=4 (asynchronous), data TX=5.
  channel = in ? 3 : 5;
  r = -1;
  for(attempt = 0; attempt < 2; attempt++){
    pid = *toggle ? PID_DATA1 : PID_DATA0;
    r = channel_xfer(channel, udev->address, endpoint, in, EPTYPE_BULK, mps,
                     data, length, pid,
                     in && endpoint == udev->bulk_in_ep ? 2000 : 1000000);
    if(r != -3 || !in)
      break;
    // DTERR means the endpoint and software disagree about DATA0/DATA1.
    // No payload was accepted, so reverse only this endpoint's toggle and
    // retry once; other IN endpoint and OUT endpoint state are independent.
    *toggle ^= 1;
    printf("dwc2: endpoint %d IN toggle resync -> DATA%d\n",
           endpoint, *toggle);
  }
  if(r >= 0){
    if(in){
      packets = (r + mps - 1) / mps;
      // A short IN transfer whose data length is an exact multiple of MPS is
      // terminated by a zero-length packet; that ZLP also advances DATA PID.
      if(r < length && (r % mps) == 0)
        packets++;
      if(packets & 1)
        *toggle ^= 1;
    } else {
      packets = length ? (length + mps - 1) / mps : 1;
      if(packets & 1)
        *toggle ^= 1;
    }
  }
  return r;
}

static void
dwc2_interrupt_rx_start(struct dwc2_interrupt_request *req)
{
  struct urb *urb = req->urb;
  struct usb_device *udev = urb->dev;
  uint32 split = 0;
  int packets = (urb->transfer_buffer_length + req->mps - 1) / req->mps;
  int pid = udev->interrupt_in_toggle ? PID_DATA1 : PID_DATA0;

  halt_channel(6);
  wr(HCINT(6), 0x3fff);
  wr(HCINTMSK(6), HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_NAK |
                   HCINT_ACK | HCINT_NYET | HCINT_ERRORS);
  dwc2_irq_pending &= ~(1U << 6);
  if(usb_route[udev->address].hub){
    split = HCSPLT_SPLTENA | HCSPLT_XACTPOS_ALL |
            HCSPLT_HUBADDR(usb_route[udev->address].hub) |
            HCSPLT_PRTADDR(usb_route[udev->address].port);
    if(req->complete_split)
      split |= HCSPLT_COMPSPLT;
  }
  wr(HCSPLT(6), split);
  wr(HCDMA(6), DWC2_DMA_BUS(urb->transfer_buffer));
  wr(HCTSIZ(6), HCTSIZ_XFERSIZE(urb->transfer_buffer_length) |
                  HCTSIZ_PKTCNT(packets) | HCTSIZ_PID(pid));
  wr(HAINTMSK, rd(HAINTMSK) | (1U << 6));
  wr(HCCHAR(6), HCCHAR_DEVADDR(udev->address) |
                  HCCHAR_EPNUM(urb->endpoint) |
                  HCCHAR_EPTYPE(EPTYPE_INTERRUPT) |
                  HCCHAR_MPS(req->mps) | HCCHAR_EPDIR_IN |
                  (usb_route[udev->address].low_speed ? HCCHAR_LSPDDEV : 0) |
                  HCCHAR_CHENA);
  req->last_issue_frame = rd(HFNUM) & 0x3fff;
  if(!interrupt_rx_arm_logged){
    interrupt_rx_arm_logged = 1;
    printf("dwc2: ch6 armed addr=%d ep=%d split=%x hcchar=%x hctsiz=%x "
           "hcintmsk=%x haintmsk=%x gintmsk=%x\n",
           udev->address, urb->endpoint, rd(HCSPLT(6)), rd(HCCHAR(6)),
           rd(HCTSIZ(6)), rd(HCINTMSK(6)), rd(HAINTMSK), rd(GINTMSK));
  }
}

// Split transactions have microframe deadlines.  A normal kworker may not run
// until several milliseconds after the SSPLIT ACK, which is too late to issue
// its CSPLIT.  Keep only these intermediate channel transitions in the HCD
// interrupt path; completed reports and interval pacing remain deferred to the
// keyboard workqueue.
static void
dwc2_wait_frame_delta(uint32 base, uint32 delta)
{
  uint64 deadline = r_cntvct_el0() + r_cntfrq_el0() / 2000; // 500 us guard

  while((((rd(HFNUM) & 0x3fff) - base) & 0x3fff) < delta &&
        r_cntvct_el0() < deadline)
    asm volatile("yield" ::: "memory");
}

// Return one when channel 6 was consumed and restarted entirely in the HCD
// fast path.  Return zero when the class-driver worker must consume the result.
static int
dwc2_interrupt_rx_irq_fast(void)
{
  struct urb *urb = interrupt_rx.urb;
  struct usb_device *udev = urb ? urb->dev : 0;
  uint32 intr, now;

  if(!interrupt_rx.active || udev == 0 ||
     usb_route[udev->address].hub == 0)
    return 0;
  intr = rd(HCINT(6));
  now = rd(HFNUM) & 0x3fff;

  if(!interrupt_rx.complete_split && (intr & HCINT_ACK)){
    // Capture the real IRQ-time frame.  Recording this in a kworker produced
    // stale values and scheduled CSPLIT several frames after its SSPLIT.
    wr(HCINT(6), intr);
    interrupt_rx.complete_split = 1;
    interrupt_rx.csplit_retries = 0;
    interrupt_rx.ssplit_ack_frame = now;
    // Periodic split-IN CSPLIT starts two microframes after the SSPLIT was
    // issued.  Waiting from last_issue_frame handles ACK arriving at either
    // age zero or one without guessing a fixed delay.
    dwc2_wait_frame_delta(interrupt_rx.last_issue_frame, 2);
    dwc2_interrupt_rx_start(&interrupt_rx);
    return 1;
  }

  if(interrupt_rx.complete_split && (intr & HCINT_NYET)){
    interrupt_rx.csplit_retries++;
    if((now >> 3) == (interrupt_rx.ssplit_ack_frame >> 3) &&
       interrupt_rx.csplit_retries < 3){
      wr(HCINT(6), intr);
      // Retry CSPLIT in the following microframe, still inside this periodic
      // transaction's frame.  Do not involve the process scheduler here.
      dwc2_wait_frame_delta(interrupt_rx.last_issue_frame, 1);
      dwc2_interrupt_rx_start(&interrupt_rx);
      return 1;
    }
  }
  return 0;
}

static int
dwc2_submit_urb(struct urb *urb)
{
  struct usb_device *udev;

  if(urb == 0 || (udev = urb->dev) == 0 || !urb->direction_in ||
     urb->endpoint <= 0 || urb->endpoint > 15 ||
     urb->transfer_buffer == 0 || urb->transfer_buffer_length <= 0 ||
     udev->interrupt_in_max_packet == 0)
    return -1;
  acquire(&usb_lock);
  if(interrupt_rx.active){
    release(&usb_lock);
    return -1;
  }
  memset(urb->transfer_buffer, 0, urb->transfer_buffer_length);
  cache_clean_invalidate_range(urb->transfer_buffer,
                               urb->transfer_buffer_length);
  interrupt_rx.urb = urb;
  interrupt_rx.mps = udev->interrupt_in_max_packet;
  interrupt_rx.complete_split = 0;
  interrupt_rx.csplit_retries = 0;
  interrupt_rx.active = 1;
  dwc2_interrupt_rx_start(&interrupt_rx);
  release(&usb_lock);
  return 0;
}

static int
dwc2_interrupt_rx_complete(struct urb *urb)
{
  struct usb_device *udev = urb ? urb->dev : 0;
  uint32 intr, left;
  int result, packets;

  acquire(&usb_lock);
  if(!interrupt_rx.active || interrupt_rx.urb != urb ||
     !(dwc2_irq_pending & (1U << 6))){
    release(&usb_lock);
    return -2;
  }
  dwc2_irq_pending &= ~(1U << 6);
  intr = rd(HCINT(6));
  wr(HCINT(6), intr);

  if(usb_route[udev->address].hub && !interrupt_rx.complete_split &&
     (intr & HCINT_ACK)){
    // Start-split was accepted by the LAN951x transaction translator.
    interrupt_rx.complete_split = 1;
    interrupt_rx.csplit_retries = 0;
    interrupt_rx.ssplit_ack_frame = rd(HFNUM) & 0x3fff;
    // A complete-split must be issued in a later high-speed microframe.
    // The synchronous path already waits here; keep the asynchronous path
    // from immediately polling the TT in the same microframe.
    udelay(250);
    dwc2_interrupt_rx_start(&interrupt_rx);
    release(&usb_lock);
    return -2;
  }
  if(usb_route[udev->address].hub && interrupt_rx.complete_split &&
     (intr & HCINT_NYET)){
    uint32 now = rd(HFNUM) & 0x3fff;

    interrupt_rx.csplit_retries++;
    // A periodic CSPLIT is valid only in the scheduling window belonging to
    // its SSPLIT.  NYET is normal when the low/full-speed endpoint NAKed or
    // the TT has not produced a result yet, but retrying CSPLIT forever turns
    // an idle keyboard into a host-channel interrupt storm.  Linux DWC2 also
    // stops periodic CSPLIT retries after the active frame has passed.
    if((now >> 3) == (interrupt_rx.ssplit_ack_frame >> 3) &&
       interrupt_rx.csplit_retries < 3){
      udelay(125);
      dwc2_interrupt_rx_start(&interrupt_rx);
      release(&usb_lock);
      return -2;
    }

    // This poll produced no report.  End the split transaction and begin a
    // fresh SSPLIT at the endpoint's bInterval.  Drop usb_lock while waiting:
    // other USB channels must not be blocked by an idle keyboard.
    interrupt_rx.complete_split = 0;
    interrupt_rx.csplit_retries = 0;
    wr(HCSPLT(6), 0);
    release(&usb_lock);
    udelay((udev->interrupt_in_interval ?
            udev->interrupt_in_interval : 1) * 1000U);
    acquire(&usb_lock);
    if(interrupt_rx.active && interrupt_rx.urb == urb)
      dwc2_interrupt_rx_start(&interrupt_rx);
    release(&usb_lock);
    return -2;
  }
  if(intr & HCINT_NAK){
    // No key-state change.  A new USB transaction begins with start-split.
    interrupt_rx.complete_split = 0;
    interrupt_rx.csplit_retries = 0;
    release(&usb_lock);
    udelay((udev->interrupt_in_interval ?
            udev->interrupt_in_interval : 1) * 1000U);
    acquire(&usb_lock);
    if(interrupt_rx.active && interrupt_rx.urb == urb)
      dwc2_interrupt_rx_start(&interrupt_rx);
    release(&usb_lock);
    return -2;
  }
  left = rd(HCTSIZ(6)) & 0x7ffff;
  if((intr & HCINT_ERRORS) || !(intr & HCINT_XFERCOMPL) ||
     left > (uint32)urb->transfer_buffer_length){
    if(interrupt_rx_errors++ < 4)
      printf("dwc2: keyboard ch6 error intr=%x hctsiz=%x hcsplt=%x\n",
             intr, rd(HCTSIZ(6)), rd(HCSPLT(6)));
    interrupt_rx.active = 0;
    wr(HCSPLT(6), 0);
    release(&usb_lock);
    return -1;
  }
  result = urb->transfer_buffer_length - left;
  cache_invalidate_range(urb->transfer_buffer, urb->transfer_buffer_length);
  packets = (result + interrupt_rx.mps - 1) / interrupt_rx.mps;
  if(result < urb->transfer_buffer_length &&
     (result % interrupt_rx.mps) == 0)
    packets++;
  if(packets & 1)
    udev->interrupt_in_toggle ^= 1;
  interrupt_rx.active = 0;
  wr(HCSPLT(6), 0);
  release(&usb_lock);
  return result;
}

static void
dwc2_interrupt_urb_work(struct work_struct *work)
{
  struct dwc2_interrupt_request *req =
    (struct dwc2_interrupt_request *)((char *)work -
      __builtin_offsetof(struct dwc2_interrupt_request, work));
  struct urb *urb = req->urb;
  int result;

  if(urb == 0)
    return;
  result = dwc2_interrupt_rx_complete(urb);
  if(result == -2)
    return;
  usb_hcd_giveback_urb(urb, result < 0 ? -1 : 0,
                       result < 0 ? 0 : result);
}

static void
dwc2_kill_urb(struct urb *urb)
{
  acquire(&usb_lock);
  if(interrupt_rx.urb == urb){
    interrupt_rx.active = 0;
    halt_channel(6);
    wr(HCINTMSK(6), 0);
    wr(HAINTMSK, rd(HAINTMSK) & ~(1U << 6));
    wr(HCINT(6), 0x3fff);
    wr(HCSPLT(6), 0);
    dwc2_irq_pending &= ~(1U << 6);
  }
  release(&usb_lock);
  cancel_work_sync(&interrupt_rx.work);
  acquire(&usb_lock);
  if(interrupt_rx.urb == urb)
    interrupt_rx.urb = 0;
  release(&usb_lock);
}

static void
dwc2_async_rx_start(void)
{
  struct dwc2_rx_request *req = 0;
  struct usb_device *udev;
  uint32 hcchar;
  int packets, pid;

  if(async_rx_active >= 0)
    return;
  for(int i = 0; i < DWC2_RX_REQUESTS; i++)
    if(async_rx[i].state == RX_QUEUED){
      async_rx_active = i;
      req = &async_rx[i];
      req->state = RX_ACTIVE;
      break;
    }
  if(req == 0)
    return;
  udev = req->udev;
  packets = (req->length + req->mps - 1) / req->mps;
  pid = udev->bulk_in_toggle ? PID_DATA1 : PID_DATA0;
  wr(HCINT(4), 0x3fff);
  wr(HCINTMSK(4), HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_NAK |
                   HCINT_ERRORS);
  dwc2_irq_pending &= ~(1U << 4);
  cache_clean_invalidate_range(req->buffer, req->length);
  wr(HCDMA(4), DWC2_DMA_BUS(req->buffer));
  wr(HCTSIZ(4), HCTSIZ_XFERSIZE(req->length) |
                  HCTSIZ_PKTCNT(packets) | HCTSIZ_PID(pid));
  hcchar = HCCHAR_DEVADDR(udev->address) |
           HCCHAR_EPNUM(req->endpoint) | HCCHAR_EPTYPE(EPTYPE_BULK) |
           HCCHAR_MPS(req->mps) | HCCHAR_EPDIR_IN;
  wr(HAINTMSK, rd(HAINTMSK) | (1U << 4));
  wr(HCCHAR(4), hcchar | HCCHAR_CHENA);
}

static int
dwc2_usb_bulk_rx_arm(struct usb_device *udev, int endpoint,
                     void *data, int length)
{
  int slot = -1;
  if(udev == 0 || endpoint <= 0 || endpoint > 15 || data == 0 || length <= 0)
    return -1;
  acquire(&usb_lock);
  for(int i = 0; i < DWC2_RX_REQUESTS; i++){
    if(async_rx[i].state != RX_FREE && async_rx[i].buffer == data){
      release(&usb_lock);
      return 0;
    }
    if(slot < 0 && async_rx[i].state == RX_FREE)
      slot = i;
  }
  if(slot < 0){
    release(&usb_lock);
    return -1;
  }
  async_rx[slot].udev = udev;
  async_rx[slot].buffer = data;
  async_rx[slot].length = length;
  async_rx[slot].endpoint = endpoint;
  async_rx[slot].mps = udev->bulk_in_max_packet ? udev->bulk_in_max_packet : 64;
  async_rx[slot].result = -2;
  async_rx[slot].state = RX_QUEUED;
  dwc2_async_rx_start();
  release(&usb_lock);
  return 0;
}

static int
dwc2_usb_bulk_rx_complete(struct usb_device *udev, int endpoint, void **data)
{
  struct dwc2_rx_request *req;
  uint32 intr, left;
  int result, packets, slot;

  if(data)
    *data = 0;
  acquire(&usb_lock);
  if(async_rx_active >= 0 && (dwc2_irq_pending & (1U << 4))){
    slot = async_rx_active;
    req = &async_rx[slot];
    dwc2_irq_pending &= ~(1U << 4);
    intr = rd(HCINT(4));
    if(intr & HCINT_NAK){
      halt_channel(4);
      wr(HCINT(4), intr);
      req->state = RX_QUEUED;
      async_rx_active = -1;
      dwc2_async_rx_start();
    } else {
      left = rd(HCTSIZ(4)) & 0x7ffff;
      result = ((intr & HCINT_ERRORS) || !(intr & HCINT_XFERCOMPL) ||
                left > (uint32)req->length) ? -1 : req->length - left;
      halt_channel(4);
      wr(HCINT(4), intr);
      cache_invalidate_range(req->buffer, req->length);
      req->result = result;
      req->state = RX_DONE;
      async_rx_active = -1;
      if(result >= 0){
        packets = (result + req->mps - 1) / req->mps;
        if(result < req->length && (result % req->mps) == 0)
          packets++;
        if(packets & 1)
          udev->bulk_in_toggle ^= 1;
      }
      dwc2_async_rx_start();
    }
  }
  for(slot = 0; slot < DWC2_RX_REQUESTS; slot++){
    req = &async_rx[slot];
    if(req->state == RX_DONE && req->udev == udev &&
       req->endpoint == endpoint){
      result = req->result;
      if(data)
        *data = req->buffer;
      req->state = RX_FREE;
      release(&usb_lock);
      return result;
    }
  }
  release(&usb_lock);
  return -2;
}

static const struct usb_host_ops dwc2_usb_ops = {
  .control = dwc2_usb_control,
  .bulk = dwc2_usb_bulk,
  .bulk_rx_arm = dwc2_usb_bulk_rx_arm,
  .bulk_rx_complete = dwc2_usb_bulk_rx_complete,
  .submit_urb = dwc2_submit_urb,
  .kill_urb = dwc2_kill_urb,
};

int
dwc2_cdc_xmit(void *packet, int len)
{
  int r, packets;
  if(!usb_ready || len <= 0 || len > USB_BUF_SIZE)
    return -1;
  acquire(&usb_lock);
  memmove(tx_buf, packet, len);
  r = channel_xfer(1, usb_address, 2, 0, EPTYPE_BULK, 64,
                   tx_buf, len, bulk_out_toggle ? PID_DATA1 : PID_DATA0,
                   100000);
  if(r >= 0){
    packets = (r + 63) / 64;
    if(packets & 1)
      bulk_out_toggle ^= 1;
  }
  release(&usb_lock);
  return r;
}

static void
arm_rx(void)
{
  uint32 hcchar;
  int len = 1536;
  int packets = (len + 63) / 64;

  wr(HCINT(2), 0x3fff);
  wr(HCINTMSK(2), HCINT_XFERCOMPL | HCINT_CHHLTD | HCINT_NAK |
                   HCINT_ERRORS);
  wr(HAINTMSK, rd(HAINTMSK) | (1U << 2));
  cache_clean_invalidate_range(rx_buf, len);
  wr(HCDMA(2), DWC2_DMA_BUS(rx_buf));
  wr(HCTSIZ(2), HCTSIZ_XFERSIZE(len) | HCTSIZ_PKTCNT(packets) |
                 HCTSIZ_PID(bulk_in_toggle ? PID_DATA1 : PID_DATA0));
  hcchar = HCCHAR_DEVADDR(usb_address) | HCCHAR_EPNUM(2) |
           HCCHAR_EPTYPE(EPTYPE_BULK) | HCCHAR_MPS(64) |
           HCCHAR_EPDIR_IN;
  wr(HCCHAR(2), hcchar | HCCHAR_CHENA);
  rx_armed = 1;
}

void
dwc2_cdc_poll(void)
{
  uint32 intr, left;
  int r, packets;

  if(!usb_ready)
    return;
  // Avoid waiting behind a user sender from timer interrupt context.
  if(!holding(&usb_lock)){
    acquire(&usb_lock);
    if(!rx_armed){
      arm_rx();
      release(&usb_lock);
      return;
    }

    if((dwc2_irq_pending & (1U << 2)) == 0){
      release(&usb_lock);
      return;
    }
    dwc2_irq_pending &= ~(1U << 2);

    intr = rd(HCINT(2));
    if(intr & HCINT_NAK){
      halt_channel(2);
      wr(HCINT(2), intr);
      rx_armed = 0;
      arm_rx();
      release(&usb_lock);
      return;
    }
    if(intr & HCINT_ERRORS){
      halt_channel(2);
      wr(HCINT(2), intr);
      rx_armed = 0;
      arm_rx();
      release(&usb_lock);
      return;
    }
    if(!(intr & HCINT_XFERCOMPL)){
      release(&usb_lock);
      return;
    }

    left = rd(HCTSIZ(2)) & 0x7ffff;
    r = left <= 1536 ? 1536 - left : -1;
    wr(HCINT(2), intr);
    rx_armed = 0;
    cache_invalidate_range(rx_buf, 1536);
    if(r > 0){
      packets = (r + 63) / 64;
      if(packets & 1)
        bulk_in_toggle ^= 1;
      memmove(rx_deliver_buf, rx_buf, r);
      // usbnet_rx() may answer ARP synchronously through dwc2_cdc_xmit(). Do not
      // enter the network stack while holding the non-recursive USB lock.
      release(&usb_lock);
      usbnet_rx(rx_deliver_buf, r);
      acquire(&usb_lock);
    }
    if(!rx_armed)
      arm_rx();
    release(&usb_lock);
  }
}

void
dwc2_irq(void)
{
  uint32 channels;

  if((rd(GINTSTS) & rd(GINTMSK) & GINTSTS_HCHINT) == 0)
    return;
  channels = rd(HAINT) & rd(HAINTMSK);
  if(channels == 0)
    return;
  // SSPLIT ACK and in-window CSPLIT NYET are HCD scheduling events, not USB
  // class-driver completions.  Handle them before masking the channel and
  // waking a kworker, otherwise scheduler latency misses the split window.
  // In particular, do not printf before this call: one serial line at 115200
  // baud takes several milliseconds and destroys the 125-us microframe
  // deadline that this fast path exists to preserve.
  if((channels & (1U << 6)) && dwc2_interrupt_rx_irq_fast()){
    channels &= ~(1U << 6);
    if(channels == 0)
      return;
  }
  // Only final completion/no-data/error events reach this point.  Logging is
  // now safe because no in-window CSPLIT remains to be scheduled.
  if((channels & (1U << 6)) && interrupt_rx_irq_logs++ < 12)
    printf("dwc2 ch6 final p=%d int=%x uf=%x issue=%x age=%x ssack=%x\n",
           interrupt_rx.complete_split,
           rd(HCINT(6)), rd(HFNUM) & 7, interrupt_rx.last_issue_frame,
           ((rd(HFNUM) & 0x3fff) - interrupt_rx.last_issue_frame) & 0x3fff,
           interrupt_rx.ssplit_ack_frame);
  // Mask completed asynchronous channels.  Their HCINT status and DMA
  // residual count are consumed by the deferred driver poll, which rearms
  // the channel and its HAINT bit after cache maintenance and frame parsing.
  wr(HAINTMSK, rd(HAINTMSK) & ~channels);
  dwc2_irq_pending |= channels;
  if(channels & (1U << 4))
    mt7601u_rx_irq();
  if(channels & (1U << 6))
    schedule_work(&interrupt_rx.work);
}

static void
dwc2_init(void)
{
  uint32 p, id;
  int cfg = -1, total, off, idx, root_class, nport, port = 0;
  int mt_found = 0, hid_found = 0;
  int selected_subclass = 0, selected_protocol = 0, selected_interface = 0;
  int current_interface = -1, selected_interface_found = 0;
  uint16 vid, pid;
  struct hid_selection nested_hid;

  initlock(&usb_lock, "usbnet");
  init_work(&interrupt_rx.work, dwc2_interrupt_urb_work);
  if(usb_firmware_power_on() < 0)
    printf("dwc2: firmware USB HCD power request failed\n");
  else
    printf("dwc2: firmware USB HCD power on\n");
  id = rd(GSNPSID);
  if((id >> 16) != 0x4f54){
    printf("dwc2: no DWC2 controller id=%x\n", id);
    return;
  }

  wr(GAHBCFG, 0);
  // Reset the core before selecting host mode.  Selecting FORCEHOST first is
  // unreliable on real DWC2: core soft-reset may discard the mode request.
  for(int i = 0; i < 100000; i++)
    if(rd(GRSTCTL) & GRSTCTL_AHBIDLE)
      break;
  wr(GRSTCTL, GRSTCTL_CSFTRST);
  for(int i = 0; i < 100000; i++)
    if((rd(GRSTCTL) & GRSTCTL_CSFTRST) == 0)
      break;
  for(int i = 0; i < 100000; i++)
    if(rd(GRSTCTL) & GRSTCTL_AHBIDLE)
      break;
  udelay(100000);

  uint32 gusbcfg = rd(GUSBCFG);
  gusbcfg &= ~GUSBCFG_FORCEDEV;
  gusbcfg |= GUSBCFG_FORCEHOST;
  wr(GUSBCFG, gusbcfg);
  for(int i = 0; i < 100; i++){
    if(rd(GINTSTS) & GINTSTS_CURMODE_HOST)
      break;
    udelay(10000);
  }
  if(!(rd(GINTSTS) & GINTSTS_CURMODE_HOST)){
    printf("dwc2: DWC2 failed to enter host mode gusbcfg=%x gintsts=%x\n",
           rd(GUSBCFG), rd(GINTSTS));
    return;
  }
  wr(HAINTMSK, 0);
  for(int channel = 0; channel < 8; channel++)
    wr(HCINTMSK(channel), 0);
  wr(GINTMSK, GINTSTS_HCHINT);
  wr(GINTSTS, 0xffffffff);
  wr(HCFG, 0);
  wr(GAHBCFG, GAHBCFG_DMA_EN | GAHBCFG_GLBL_INTR_EN);
  printf("dwc2: host-channel IRQ enabled\n");

  // Power the root port before testing connect status.  On real Raspberry
  // Pi 3 hardware the LAN951x hub is behind this port and needs time after
  // DWC2 host-mode initialization before HPRT_CONNSTS becomes visible.
  p = rd(HPRT0) & ~HPRT_W1C;
  wr(HPRT0, p | HPRT_PWR);
  udelay(100000);
  for(int i = 0; i < 100; i++){
    p = rd(HPRT0);
    if(p & HPRT_CONNSTS)
      break;
    udelay(10000);
  }
  if(!(p & HPRT_CONNSTS)){
    printf("dwc2: DWC2 root port has no connection hprt=%x gintsts=%x\n",
           p, rd(GINTSTS));
    return;
  }
  p &= ~HPRT_W1C;
  wr(HPRT0, p | HPRT_PWR | HPRT_RST);
  udelay(50000);
  p = rd(HPRT0) & ~HPRT_W1C;
  wr(HPRT0, (p | HPRT_PWR) & ~HPRT_RST);
  udelay(20000);
  if(!(rd(HPRT0) & HPRT_ENA)){
    printf("dwc2: port reset failed hprt=%x\n", rd(HPRT0));
    return;
  }
  printf("dwc2: host hprt=%x hcfg=%x hfnum=%x\n",
         rd(HPRT0), rd(HCFG), rd(HFNUM));

  memset(ctrl_buf, 0, sizeof(ctrl_buf));
  if(get_device_descriptor(0, ctrl_buf) < 0){
    printf("dwc2: GET device descriptor failed\n");
    return;
  }
  ep0_mps = ctrl_buf[7];
  root_class = ctrl_buf[4];
  vid = ctrl_buf[8] | ((uint16)ctrl_buf[9] << 8);
  pid = ctrl_buf[10] | ((uint16)ctrl_buf[11] << 8);
  printf("dwc2: root device class=%d vid=%x pid=%x configs=%d mps=%d\n",
         ctrl_buf[4], vid, pid, ctrl_buf[17], ep0_mps);
  if(control(0, 0x00, USB_SET_ADDRESS, 1, 0, ctrl_buf, 0) < 0){
    printf("dwc2: SET_ADDRESS failed\n");
    return;
  }
  udelay(5000);
  usb_address = 1;

  if(root_class == 9){
    // Configure the high-speed hub connected to the DWC2 root port.
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    if(control(1, 0x80, USB_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
               ctrl_buf, 9) < 0 ||
       control(1, 0x00, USB_SET_CONFIG, ctrl_buf[5], 0,
               ctrl_buf, 0) < 0 ||
       control(1, 0xa0, USB_GET_DESCRIPTOR, USB_DT_HUB << 8, 0,
               ctrl_buf, 9) < 0){
      printf("dwc2: hub configuration failed\n");
      return;
    }
    nport = ctrl_buf[2];
    for(idx = 1; idx <= nport; idx++)
      control(1, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_POWER, idx,
              ctrl_buf, 0);
    udelay(100000);
    // Port numbering differs among LAN9512/LAN9514 board revisions.  Scan
    // every downstream port and identify devices by VID/PID; do not assume
    // that port 1 is always the permanently attached Ethernet function.
    for(idx = 1; idx <= nport; idx++){
      uint16 initial_status = 0;
      for(int connect_wait = 0; connect_wait < 50; connect_wait++){
        memset(ctrl_buf, 0, 4);
        if(control(1, 0xa3, USB_REQ_GET_STATUS, 0, idx,
                   ctrl_buf, 4) == 0){
          initial_status = ctrl_buf[0] | ((uint16)ctrl_buf[1] << 8);
          if(initial_status & HUB_PORT_CONNECTION)
            break;
        }
        udelay(10000);
      }
      if(initial_status == 0){
        printf("dwc2: hub port %d status read failed\n", idx);
        continue;
      }
      printf("dwc2: hub port %d status=%x change=%x\n", idx,
             initial_status,
             ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8));
      if(!(initial_status & HUB_PORT_CONNECTION))
        continue;
      port = idx;
      printf("dwc2: hub external port %d connected status=%x\n",
             port, initial_status);
      if(control(1, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_RESET, port,
                 ctrl_buf, 0) < 0){
        printf("dwc2: hub port %d reset request failed\n", port);
        continue;
      }
      uint16 port_status = 0;
      for(int reset_wait = 0; reset_wait < 50; reset_wait++){
        udelay(10000);
        memset(ctrl_buf, 0, 4);
        if(control(1, 0xa3, USB_REQ_GET_STATUS, 0, port,
                   ctrl_buf, 4) < 0)
          continue;
        port_status = ctrl_buf[0] | ((uint16)ctrl_buf[1] << 8);
        if((port_status & HUB_PORT_RESET_STAT) == 0 &&
           (port_status & HUB_PORT_ENABLE))
          break;
      }
      printf("dwc2: hub port %d after-reset status=%x speed=%s\n",
             port, port_status,
             (port_status & HUB_PORT_HIGH_SPEED) ? "high" :
             (port_status & HUB_PORT_LOW_SPEED) ? "low" : "full");
      if((port_status & (HUB_PORT_CONNECTION | HUB_PORT_ENABLE)) !=
         (HUB_PORT_CONNECTION | HUB_PORT_ENABLE)){
        printf("dwc2: hub port %d did not enable after reset\n", port);
        continue;
      }
      control(1, 0x23, USB_REQ_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, port,
              ctrl_buf, 0);
      control(1, 0x23, USB_REQ_CLEAR_FEATURE, HUB_C_PORT_RESET, port,
              ctrl_buf, 0);
      // Full/low-speed children of the LAN951x high-speed hub require DWC2
      // start-split/complete-split transactions.  Address zero is the
      // temporary route used during enumeration.
      usb_route[0].hub = 0;
      usb_route[0].port = 0;
      usb_route[0].low_speed = 0;
      if(!(port_status & HUB_PORT_HIGH_SPEED)){
        usb_route[0].hub = 1;
        usb_route[0].port = port;
        usb_route[0].low_speed =
          (port_status & HUB_PORT_LOW_SPEED) != 0;
      }
      memset(ctrl_buf, 0, sizeof(ctrl_buf));
      if(get_device_descriptor(0, ctrl_buf) < 0){
        printf("dwc2: hub port %d child descriptor failed\n", port);
        continue;
      }
      ep0_mps = ctrl_buf[7];
      root_class = ctrl_buf[4];
      vid = ctrl_buf[8] | ((uint16)ctrl_buf[9] << 8);
      pid = ctrl_buf[10] | ((uint16)ctrl_buf[11] << 8);
      usb_address = port + 1;
      if(control(0, 0x00, USB_SET_ADDRESS, usb_address, 0,
                 ctrl_buf, 0) < 0){
        printf("dwc2: hub port %d child SET_ADDRESS failed\n", port);
        continue;
      }
      udelay(5000);
      usb_route[usb_address] = usb_route[0];
      printf("dwc2: hub port=%d child class=%d vid=%x pid=%x mps=%d\n",
             port, root_class, vid, pid, ep0_mps);
      if(root_class == 9){
        memset(&nested_hid, 0, sizeof(nested_hid));
        if(find_keyboard_below_hub(usb_address, 1, &nested_hid)){
          usb_address = nested_hid.address;
          ep0_mps = nested_hid.ep0_mps;
          vid = nested_hid.vid;
          pid = nested_hid.pid;
          root_class = 3;
          selected_subclass = 1;
          selected_protocol = 1;
          selected_interface = nested_hid.interface_number;
          hid_found = 1;
          break;
        }
      }
      if(is_mt7601(vid, pid)){
        mt_found = 1;
        break;
      }
      // Most HID devices report class 0 in the device descriptor and put
      // class/subclass/protocol in their interface descriptor.
      memset(ctrl_buf, 0, sizeof(ctrl_buf));
      if(control(usb_address, 0x80, USB_GET_DESCRIPTOR,
                 USB_DT_CONFIG << 8, 0, ctrl_buf, 9) == 0){
        int probe_total = ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8);
        if(probe_total >= 9 && probe_total <= (int)sizeof(ctrl_buf) &&
           control(usb_address, 0x80, USB_GET_DESCRIPTOR,
                   USB_DT_CONFIG << 8, 0, ctrl_buf, probe_total) == 0){
          for(int poff = 0; poff + 9 <= probe_total && ctrl_buf[poff] >= 2;
              poff += ctrl_buf[poff]){
            if(ctrl_buf[poff + 1] == 4 && ctrl_buf[poff] >= 9 &&
               ctrl_buf[poff + 5] == 3 && ctrl_buf[poff + 6] == 1 &&
               ctrl_buf[poff + 7] == 1){
              root_class = 3;
              selected_subclass = 1;
              selected_protocol = 1;
              selected_interface = ctrl_buf[poff + 2];
              hid_found = 1;
              break;
            }
          }
        }
      }
      if(hid_found)
        break;
    }
    if(!mt_found && !hid_found){
      printf("dwc2: no supported USB child on external hub ports\n");
      return;
    }
  }

  memset(&usb_child, 0, sizeof(usb_child));
  usb_child.dev.name = "usb1";
  usb_child.dev.parent = usb_parent;
  usb_child.ops = &dwc2_usb_ops;
  usb_child.vendor = vid;
  usb_child.product = pid;
  usb_child.class = root_class;
  usb_child.subclass = selected_subclass;
  usb_child.protocol = selected_protocol;
  usb_child.interface_number = selected_interface;
  usb_child.address = usb_address;
  usb_child.ep0_max_packet = ep0_mps;

  // MT7601U is a vendor-specific USB device, not CDC Ethernet.  Complete
  // the standard USB enumeration before publishing it on usb_bus so that
  // mt7601u_probe() may issue vendor requests against a configured device.
  if(is_mt7601(vid, pid)){
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    if(control(usb_address, 0x80, USB_GET_DESCRIPTOR,
               USB_DT_CONFIG << 8, 0, ctrl_buf, 9) < 0 ||
       ctrl_buf[1] != USB_DT_CONFIG || ctrl_buf[5] == 0){
      printf("dwc2: MT7601U configuration descriptor failed\n");
      return;
    }
    cfg = ctrl_buf[5];
    total = ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8);
    if(total < 9 || total > (int)sizeof(ctrl_buf) ||
       control(usb_address, 0x80, USB_GET_DESCRIPTOR,
               USB_DT_CONFIG << 8, 0, ctrl_buf, total) < 0){
      printf("dwc2: MT7601U full configuration descriptor failed\n");
      return;
    }
    for(off = 0; off + 7 <= total && ctrl_buf[off] >= 2;
        off += ctrl_buf[off]){
      uint8 addr;
      uint16 mps;
      if(ctrl_buf[off + 1] != 5 || ctrl_buf[off] < 7 ||
         (ctrl_buf[off + 3] & 3) != 2)
        continue;
      addr = ctrl_buf[off + 2];
      mps = ctrl_buf[off + 4] | ((uint16)ctrl_buf[off + 5] << 8);
      if(addr & 0x80){
        if(usb_child.bulk_in_ep == 0){
          usb_child.bulk_in_ep = addr & 0xf;
          usb_child.bulk_in_max_packet = mps;
        } else if(usb_child.bulk_in_ep2 == 0){
          usb_child.bulk_in_ep2 = addr & 0xf;
        }
      } else if(!(addr & 0x80) && usb_child.bulk_out_ep == 0){
        usb_child.bulk_out_ep = addr & 0xf;
        usb_child.bulk_out_max_packet = mps;
      }
    }
    if(usb_child.bulk_in_ep == 0 || usb_child.bulk_in_ep2 == 0 ||
       usb_child.bulk_out_ep == 0){
      printf("dwc2: MT7601U bulk endpoints missing\n");
      return;
    }
    if(control(usb_address, 0x00, USB_SET_CONFIG, cfg, 0,
               ctrl_buf, 0) < 0){
      printf("dwc2: MT7601U SET_CONFIG %d failed\n", cfg);
      return;
    }
  } else if(hid_found || root_class == 3){
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    if(control(usb_address, 0x80, USB_GET_DESCRIPTOR,
               USB_DT_CONFIG << 8, 0, ctrl_buf, 9) < 0 ||
       ctrl_buf[1] != USB_DT_CONFIG || ctrl_buf[5] == 0){
      printf("dwc2: HID configuration descriptor failed\n");
      return;
    }
    cfg = ctrl_buf[5];
    total = ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8);
    if(total < 9 || total > (int)sizeof(ctrl_buf) ||
       control(usb_address, 0x80, USB_GET_DESCRIPTOR,
               USB_DT_CONFIG << 8, 0, ctrl_buf, total) < 0){
      printf("dwc2: HID full configuration descriptor failed\n");
      return;
    }
    current_interface = -1;
    for(off = 0; off + 2 <= total && ctrl_buf[off] >= 2;
        off += ctrl_buf[off]){
      if(ctrl_buf[off + 1] == 4 && ctrl_buf[off] >= 9){
        current_interface = ctrl_buf[off + 2];
        if(current_interface == selected_interface && ctrl_buf[off + 5] == 3){
          usb_child.class = ctrl_buf[off + 5];
          usb_child.subclass = ctrl_buf[off + 6];
          usb_child.protocol = ctrl_buf[off + 7];
          usb_child.interface_number = current_interface;
          selected_interface_found = 1;
        }
      } else if(current_interface == selected_interface &&
                ctrl_buf[off + 1] == 5 && ctrl_buf[off] >= 7 &&
                (ctrl_buf[off + 2] & 0x80) &&
                (ctrl_buf[off + 3] & 3) == EPTYPE_INTERRUPT){
        usb_child.interrupt_in_ep = ctrl_buf[off + 2] & 0xf;
        usb_child.interrupt_in_max_packet =
          ctrl_buf[off + 4] | ((uint16)ctrl_buf[off + 5] << 8);
        usb_child.interrupt_in_interval = ctrl_buf[off + 6];
      }
    }
    if(!selected_interface_found ||
       usb_child.class != 3 || usb_child.subclass != 1 ||
       usb_child.protocol != 1 || usb_child.interrupt_in_ep == 0){
      printf("dwc2: HID interface %d unsupported class=%d subclass=%d "
             "protocol=%d interrupt-in=%d\n",
             selected_interface, usb_child.class, usb_child.subclass,
             usb_child.protocol, usb_child.interrupt_in_ep);
      return;
    }
    if(control(usb_address, 0x00, USB_SET_CONFIG, cfg, 0,
               ctrl_buf, 0) < 0){
      printf("dwc2: HID SET_CONFIG %d failed\n", cfg);
      return;
    }
  }
  if(usb_device_register(&usb_child) < 0){
    printf("dwc2: cannot register USB child %x:%x\n", vid, pid);
    return;
  }
  usb_child_registered = 1;

  if(is_mt7601(vid, pid)){
    printf("dwc2: MT7601U configured; bulk-in=%d,%d/%d bulk-out=%d/%d\n",
           usb_child.bulk_in_ep, usb_child.bulk_in_ep2,
           usb_child.bulk_in_max_packet,
           usb_child.bulk_out_ep, usb_child.bulk_out_max_packet);
    printf("dwc2: MT7601U handed to usb bus\n");
    return;
  }

  if(hid_found || root_class == 3){
    printf("dwc2: HID keyboard handed to usb bus\n");
    return;
  }

  bulk_in_toggle = bulk_out_toggle = 0;
  rx_armed = 0;
  usb_ready = 1;
  if(usbnet_attach(&usb_child) < 0){
    usb_ready = 0;
    printf("usbnet: cannot register usb0\n");
    return;
  }
}

static int
dwc2_probe(struct device *dev)
{
  usb_parent = dev;
  dwc2_init();
  return 0;
}

static void
dwc2_remove(struct device *dev)
{
  (void)dev;
  usbnet_detach();
  if(usb_child_registered){
    usb_device_unregister(&usb_child);
    usb_child_registered = 0;
  }
  usb_ready = 0;
  wr(HAINTMSK, 0);
  wr(GINTMSK, 0);
  wr(GAHBCFG, rd(GAHBCFG) & ~GAHBCFG_GLBL_INTR_EN);
  dwc2_irq_pending = 0;
  memset(async_rx, 0, sizeof(async_rx));
  async_rx_active = -1;
  memset(&async_rx, 0, sizeof(async_rx));
  usb_parent = 0;
}

void
dwc2_driver_init(void)
{
  static struct device dev = {
    .name = "bcm2837-dwc2",
    .id = 0,
    .resource = {
      { V2P_WO(DWC2_BASE), V2P_WO(DWC2_BASE) + 0x17ffff,
        IORESOURCE_MEM, "DWC2 USB host" },
      { DWC2_IRQ, DWC2_IRQ, IORESOURCE_IRQ, "DWC2 host-channel IRQ" },
    },
    .nresource = 2,
  };
  static struct device_driver drv = {
    .name = "bcm2837-dwc2",
    .probe = dwc2_probe,
    .remove = dwc2_remove,
  };
  platform_device_register(&dev);
  platform_driver_register(&drv);
}
