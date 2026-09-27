// Minimal BCM2837 DWC2 host + CDC-ECM driver for QEMU's "usb-net" device.
// It deliberately supports a directly attached high/full-speed CDC device;
// the real Pi 3 LAN951x hub/Ethernet combination needs hub + SMSC95xx layers.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"

#define DWC2_BASE (PERIPHERAL_BASE + 0x00980000UL)

#define GAHBCFG   0x008
#define GUSBCFG   0x00c
#define GRSTCTL   0x010
#define GINTSTS   0x014
#define GINTMSK   0x018
#define GSNPSID   0x040
#define HCFG      0x400
#define HPRT0     0x440
#define HCCHAR(c) (0x500 + 0x20*(c))
#define HCINT(c)  (0x508 + 0x20*(c))
#define HCTSIZ(c) (0x510 + 0x20*(c))
#define HCDMA(c)  (0x514 + 0x20*(c))

#define GAHBCFG_DMA_EN       (1U << 5)
#define GUSBCFG_FORCEHOST    (1U << 29)
#define GRSTCTL_AHBIDLE      (1U << 31)
#define GRSTCTL_CSFTRST      (1U << 0)
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
#define HCCHAR_EPDIR_IN      (1U << 15)
#define HCCHAR_EPNUM(e)      ((uint32)(e) << 11)
#define HCCHAR_MPS(n)        ((uint32)(n))
#define EPTYPE_CONTROL       0
#define EPTYPE_BULK          2

#define HCINT_XFERCOMPL      (1U << 0)
#define HCINT_CHHLTD         (1U << 1)
#define HCINT_STALL          (1U << 3)
#define HCINT_NAK            (1U << 4)
#define HCINT_XACTERR        (1U << 7)
#define HCINT_ERRORS         ((1U << 2) | HCINT_STALL | HCINT_XACTERR | \
                              (1U << 8) | (1U << 9) | (1U << 10))

#define PID_DATA0            0
#define PID_DATA1            2
#define PID_SETUP            3
#define HCTSIZ_PID(p)        ((uint32)(p) << 29)
#define HCTSIZ_PKTCNT(n)     ((uint32)(n) << 19)
#define HCTSIZ_XFERSIZE(n)   ((uint32)(n) & 0x7ffff)

#define USB_GET_DESCRIPTOR   6
#define USB_SET_ADDRESS      5
#define USB_SET_CONFIG       9
#define USB_SET_INTERFACE    11
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
#define CDC_SET_PACKET_FILTER 0x43

#define USB_BUF_SIZE 2048

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
static uchar setup_buf[64] __attribute__((aligned(64)));
static uchar ctrl_buf[512] __attribute__((aligned(64)));
static uchar rx_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static uchar tx_buf[USB_BUF_SIZE] __attribute__((aligned(64)));

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

// Return actual byte count, -2 for NAK, -1 for a hard USB error.
static int
channel_xfer(int ch, int addr, int ep, int in, int type, int mps,
             void *buf, int len, int pid, int poll_us)
{
  uint32 intr, hcchar, left;
  int packets = len ? (len + mps - 1) / mps : 1;
  int saw_nak = 0;
  uint64 deadline;

  halt_channel(ch);
  wr(HCINT(ch), 0x3fff);
  if(in)
    cache_invalidate_range(buf, len);
  else
    cache_clean_range(buf, len);
  wr(HCDMA(ch), (uint32)V2P(buf));
  wr(HCTSIZ(ch), HCTSIZ_XFERSIZE(len) | HCTSIZ_PKTCNT(packets) |
                  HCTSIZ_PID(pid));
  hcchar = HCCHAR_DEVADDR(addr) | HCCHAR_EPNUM(ep) |
           HCCHAR_EPTYPE(type) | HCCHAR_MPS(mps);
  if(in)
    hcchar |= HCCHAR_EPDIR_IN;
  wr(HCCHAR(ch), hcchar | HCCHAR_CHENA);

  deadline = r_cntvct_el0() +
             ((uint64)r_cntfrq_el0() * poll_us + 999999) / 1000000;
  for(;;){
    intr = rd(HCINT(ch));
    if(intr & HCINT_NAK){
      wr(HCINT(ch), intr);
      saw_nak = 1;
      // DWC2 keeps control/bulk channels active after NAK and retries them
      // on a later USB frame. Do not tear the channel down immediately.
      if(r_cntvct_el0() >= deadline){
        halt_channel(ch);
        return -2;
      }
      continue;
    }
    if(intr & HCINT_ERRORS){
      halt_channel(ch);
      wr(HCINT(ch), intr);
      printf("usbnet: channel %d error intr=%x hcchar=%x hctsiz=%x\n",
             ch, intr, rd(HCCHAR(ch)), rd(HCTSIZ(ch)));
      return -1;
    }
    if(intr & (HCINT_XFERCOMPL | HCINT_CHHLTD))
      break;
    if(r_cntvct_el0() >= deadline){
      halt_channel(ch);
      if(saw_nak)
        return -2;
      printf("usbnet: channel %d timeout intr=%x hcchar=%x hctsiz=%x\n",
             ch, rd(HCINT(ch)), rd(HCCHAR(ch)), rd(HCTSIZ(ch)));
      return -1;
    }
  }
  left = rd(HCTSIZ(ch)) & 0x7ffff;
  wr(HCINT(ch), intr);
  if(in)
    cache_invalidate_range(buf, len);
  if(!(intr & HCINT_XFERCOMPL) || left > (uint32)len)
    return -1;
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
  if(r != sizeof(*s))
    return -1;
  if(len){
    r = channel_xfer(0, addr, 0, in, EPTYPE_CONTROL, ep0_mps,
                     data, len, PID_DATA1, 100000);
    if(r < 0)
      return -1;
  }
  r = channel_xfer(0, addr, 0, !in, EPTYPE_CONTROL, ep0_mps,
                   ctrl_buf, 0, PID_DATA1, 100000);
  return r < 0 ? -1 : 0;
}

static int
usbnet_xmit(void *packet, int len)
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

void
usbnetpoll(void)
{
  int r, packets;
  static int debug_polls;
  if(!usb_ready)
    return;
  // Avoid waiting behind a user sender from timer interrupt context.
  if(!holding(&usb_lock)){
    acquire(&usb_lock);
    r = channel_xfer(2, usb_address, 2, 1, EPTYPE_BULK, 64,
                     rx_buf, 1536,
                     bulk_in_toggle ? PID_DATA1 : PID_DATA0, 2000);
    if(debug_polls < 5){
      printf("usbnet: rx poll=%d result=%d intr=%x size=%x\n",
             debug_polls, r, rd(HCINT(2)), rd(HCTSIZ(2)));
      debug_polls++;
    }
    if(r > 0)
      printf("usbnet: received Ethernet frame len=%d\n", r);
    if(r > 0){
      packets = (r + 63) / 64;
      if(packets & 1)
        bulk_in_toggle ^= 1;
      net_rx(rx_buf, r);
    }
    release(&usb_lock);
  }
}

void
usbnetinit(void)
{
  uint32 p, id;
  int cfg = -1, total, off, idx, root_class, nport, port = 0;
  uint16 vid, pid;

  initlock(&usb_lock, "usbnet");
  id = rd(GSNPSID);
  if((id >> 16) != 0x4f54){
    printf("usbnet: no DWC2 controller id=%x\n", id);
    return;
  }

  wr(GAHBCFG, 0);
  wr(GUSBCFG, rd(GUSBCFG) | GUSBCFG_FORCEHOST);
  for(int i = 0; i < 100000; i++)
    if(rd(GRSTCTL) & GRSTCTL_AHBIDLE)
      break;
  wr(GRSTCTL, GRSTCTL_CSFTRST);
  for(int i = 0; i < 100000; i++)
    if((rd(GRSTCTL) & GRSTCTL_CSFTRST) == 0)
      break;
  wr(GINTMSK, 0);
  wr(GINTSTS, 0xffffffff);
  wr(HCFG, 0);
  wr(GAHBCFG, GAHBCFG_DMA_EN);

  p = rd(HPRT0);
  if(!(p & HPRT_CONNSTS)){
    printf("usbnet: DWC2 ready, no USB Ethernet device\n");
    return;
  }
  p &= ~HPRT_W1C;
  wr(HPRT0, p | HPRT_PWR | HPRT_RST);
  udelay(50000);
  p = rd(HPRT0) & ~HPRT_W1C;
  wr(HPRT0, (p | HPRT_PWR) & ~HPRT_RST);
  udelay(20000);
  if(!(rd(HPRT0) & HPRT_ENA)){
    printf("usbnet: port reset failed hprt=%x\n", rd(HPRT0));
    return;
  }

  memset(ctrl_buf, 0, sizeof(ctrl_buf));
  if(control(0, 0x80, USB_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
             ctrl_buf, 18) < 0){
    printf("usbnet: GET device descriptor failed\n");
    return;
  }
  if(ctrl_buf[1] != USB_DT_DEVICE ||
     (ctrl_buf[7] != 8 && ctrl_buf[7] != 16 &&
      ctrl_buf[7] != 32 && ctrl_buf[7] != 64)){
    printf("usbnet: unsupported ep0 descriptor type=%d mps=%d\n",
           ctrl_buf[1], ctrl_buf[7]);
    return;
  }
  ep0_mps = ctrl_buf[7];
  root_class = ctrl_buf[4];
  vid = ctrl_buf[8] | ((uint16)ctrl_buf[9] << 8);
  pid = ctrl_buf[10] | ((uint16)ctrl_buf[11] << 8);
  printf("usbnet: root device class=%d vid=%x pid=%x configs=%d mps=%d\n",
         ctrl_buf[4], vid, pid, ctrl_buf[17], ep0_mps);
  if(control(0, 0x00, USB_SET_ADDRESS, 1, 0, ctrl_buf, 0) < 0){
    printf("usbnet: SET_ADDRESS failed\n");
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
      printf("usbnet: hub configuration failed\n");
      return;
    }
    nport = ctrl_buf[2];
    for(idx = 1; idx <= nport; idx++)
      control(1, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_POWER, idx,
              ctrl_buf, 0);
    udelay(20000);
    for(idx = 1; idx <= nport; idx++){
      memset(ctrl_buf, 0, 4);
      if(control(1, 0xa3, USB_REQ_GET_STATUS, 0, idx,
                 ctrl_buf, 4) == 0 && (ctrl_buf[0] & 1)){
        port = idx;
        break;
      }
    }
    if(port == 0){
      printf("usbnet: hub has no connected downstream device\n");
      return;
    }
    if(control(1, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_RESET, port,
               ctrl_buf, 0) < 0){
      printf("usbnet: hub port %d reset request failed\n", port);
      return;
    }
    udelay(50000);
    control(1, 0x23, USB_REQ_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, port,
            ctrl_buf, 0);
    control(1, 0x23, USB_REQ_CLEAR_FEATURE, HUB_C_PORT_RESET, port,
            ctrl_buf, 0);

    // The freshly reset child is again address zero. QEMU's DWC2 model
    // resolves its topology internally; real hardware additionally requires
    // start/complete split transactions through this high-speed hub.
    ep0_mps = 8;
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    if(control(0, 0x80, USB_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
               ctrl_buf, 18) < 0 || ctrl_buf[1] != USB_DT_DEVICE){
      printf("usbnet: child device descriptor failed\n");
      return;
    }
    ep0_mps = ctrl_buf[7];
    root_class = ctrl_buf[4];
    vid = ctrl_buf[8] | ((uint16)ctrl_buf[9] << 8);
    pid = ctrl_buf[10] | ((uint16)ctrl_buf[11] << 8);
    if(control(0, 0x00, USB_SET_ADDRESS, 2, 0, ctrl_buf, 0) < 0){
      printf("usbnet: child SET_ADDRESS failed\n");
      return;
    }
    udelay(5000);
    usb_address = 2;
    printf("usbnet: hub port=%d child class=%d vid=%x pid=%x mps=%d\n",
           port, root_class, vid, pid, ep0_mps);
  }

  // Select the configuration that advertises a CDC Ethernet control
  // interface (class 2, subclass 6), rather than relying on descriptor order.
  for(idx = 0; idx < 2 && cfg < 0; idx++){
    memset(ctrl_buf, 0, sizeof(ctrl_buf));
    if(control(usb_address, 0x80, USB_GET_DESCRIPTOR,
               (USB_DT_CONFIG << 8) | idx, 0, ctrl_buf, 9) < 0)
      continue;
    total = ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8);
    if(total < 9 || total > (int)sizeof(ctrl_buf))
      continue;
    if(control(usb_address, 0x80, USB_GET_DESCRIPTOR,
               (USB_DT_CONFIG << 8) | idx, 0, ctrl_buf, total) < 0)
      continue;
    for(off = 0; off + 2 <= total && ctrl_buf[off] >= 2;
        off += ctrl_buf[off]){
      if(ctrl_buf[off + 1] == 4 && ctrl_buf[off] >= 9 &&
         ctrl_buf[off + 5] == 2 && ctrl_buf[off + 6] == 6){
        cfg = ctrl_buf[5];
        break;
      }
    }
  }
  if(cfg < 0){
    printf("usbnet: no CDC-ECM configuration\n");
    return;
  }
  // Interface 1 alternate 1 activates bulk endpoint 2 IN/OUT.
  if(control(usb_address, 0x00, USB_SET_CONFIG, cfg, 0, ctrl_buf, 0) < 0){
    printf("usbnet: CDC SET_CONFIG failed\n");
    return;
  }
  if(control(usb_address, 0x01, USB_SET_INTERFACE, 1, 1, ctrl_buf, 0) < 0){
    printf("usbnet: CDC SET_INTERFACE failed\n");
    return;
  }
  if(control(usb_address, 0x21, CDC_SET_PACKET_FILTER, 0x000f, 0,
             ctrl_buf, 0) < 0){
    printf("usbnet: CDC SET_PACKET_FILTER failed\n");
    return;
  }
  bulk_in_toggle = bulk_out_toggle = 0;
  usb_ready = 1;
  net_set_xmit(usbnet_xmit);
  printf("usbnet: CDC-ECM ready addr=%d cfg=%d vid=%x pid=%x\n",
         usb_address, cfg, vid, pid);
}
