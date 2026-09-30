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

#define DWC2_BASE (PERIPHERAL_BASE + 0x00980000UL)
#define USB_MBOX_BASE (PERIPHERAL_BASE + 0x0000b880UL)
#define SYS_TIMER_CLO (*(volatile uint32 *)(PERIPHERAL_BASE + 0x00003004UL))

#define GAHBCFG   0x008
#define GUSBCFG   0x00c
#define GRSTCTL   0x010
#define GINTSTS   0x014
#define GINTMSK   0x018
#define GSNPSID   0x040
#define HCFG      0x400
#define HFNUM     0x408
#define HPRT0     0x440
#define HCCHAR(c) (0x500 + 0x20*(c))
#define HCINT(c)  (0x508 + 0x20*(c))
#define HCTSIZ(c) (0x510 + 0x20*(c))
#define HCDMA(c)  (0x514 + 0x20*(c))

#define GAHBCFG_DMA_EN       (1U << 5)
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
#define HCINT_DTERR          (1U << 10)
#define HCINT_ERRORS         ((1U << 2) | HCINT_STALL | HCINT_XACTERR | \
                              (1U << 8) | (1U << 9) | HCINT_DTERR)

#define PID_DATA0            0
#define PID_DATA1            2
#define PID_SETUP            3
#define HCTSIZ_PID(p)        ((uint32)(p) << 29)
#define HCTSIZ_PKTCNT(n)     ((uint32)(n) << 19)
#define HCTSIZ_XFERSIZE(n)   ((uint32)(n) & 0x7ffff)

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
static uchar setup_buf[64] __attribute__((aligned(64)));
static uchar ctrl_buf[512] __attribute__((aligned(64)));
static uchar rx_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static uchar rx_deliver_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static uchar tx_buf[USB_BUF_SIZE] __attribute__((aligned(64)));
static struct device *usb_parent;
static struct usb_device usb_child;
static int usb_child_registered;
static uint32 usb_power_message[8] __attribute__((aligned(64)));

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
  // BCM2837 System Timer CLO is a firmware-independent 1 MHz free-running
  // counter.  Do not use CNTFRQ/CNTVCT here: their setup depends on the
  // firmware armstub and differed between the tested Pi 3B and Pi 3B+.
  uint32 start = SYS_TIMER_CLO;
  while((uint32)(SYS_TIMER_CLO - start) < us)
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
  uint64 deadline;

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
  wr(HCDMA(ch), DWC2_DMA_BUS(buf));
  wr(HCTSIZ(ch), HCTSIZ_XFERSIZE(len) | HCTSIZ_PKTCNT(packets) |
                  HCTSIZ_PID(pid));
  hcchar = HCCHAR_DEVADDR(addr) | HCCHAR_EPNUM(ep) |
           HCCHAR_EPTYPE(type) | HCCHAR_MPS(mps);
  if(in)
    hcchar |= HCCHAR_EPDIR_IN;
  wr(HCCHAR(ch), hcchar | HCCHAR_CHENA);
  for(;;){
    intr = rd(HCINT(ch));
    if(intr & HCINT_NAK){
      wr(HCINT(ch), intr);
      saw_nak = 1;
      if(r_cntvct_el0() >= deadline){
        halt_channel(ch);
        if(poll_us > 2000)
          printf("dwc2: channel %d NAK timeout hctsiz=%x hfnum=%x\n",
                 ch, rd(HCTSIZ(ch)), rd(HFNUM));
        return -2;
      }
      // DWC2 halts a host channel after NAK.  Clearing HCINT and merely
      // polling again leaves the channel stopped forever; resubmit the same
      // transaction on a later frame.  A NAK means the endpoint accepted no
      // data, so the PID and DMA position do not advance.
      halt_channel(ch);
      wr(HCINT(ch), 0x3fff);
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
  if(in)
    cache_invalidate_range(buf, len);
  if(!(intr & HCINT_XFERCOMPL) || left > (uint32)len)
    return -1;
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
             descriptor, 8) < 0 || descriptor[1] != USB_DT_DEVICE)
    return -1;
  if(descriptor[7] != 8 && descriptor[7] != 16 &&
     descriptor[7] != 32 && descriptor[7] != 64)
    return -1;
  ep0_mps = descriptor[7];
  memset(descriptor, 0, 18);
  if(control(addr, 0x80, USB_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
             descriptor, 18) < 0 || descriptor[1] != USB_DT_DEVICE)
    return -1;
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
  int mps, packets, pid, r, attempt;
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
  r = -1;
  for(attempt = 0; attempt < 2; attempt++){
    pid = *toggle ? PID_DATA1 : PID_DATA0;
    r = channel_xfer(3, udev->address, endpoint, in, EPTYPE_BULK, mps,
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

static const struct usb_host_ops dwc2_usb_ops = {
  .control = dwc2_usb_control,
  .bulk = dwc2_usb_bulk,
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

    intr = rd(HCINT(2));
    if(intr & HCINT_NAK)
      wr(HCINT(2), HCINT_NAK);
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

static void
dwc2_init(void)
{
  uint32 p, id;
  int cfg = -1, total, off, idx, root_class, nport, port = 0;
  int mt_found = 0;
  uint16 vid, pid;

  initlock(&usb_lock, "usbnet");
  if(usb_firmware_power_on() < 0)
    printf("dwc2: firmware USB HCD power request failed\n");
  else
    printf("dwc2: firmware USB HCD power on\n");

  // A Pi 3B may power-cycle the LAN9512/9514 and DWC2 clock when firmware
  // transfers ownership of the USB power domain.  The 3B+ usually settles
  // much faster, which hid this requirement on the original test board.
  // Do not touch DWC2 registers until that transition has completed.
  udelay(1000000);
  printf("dwc2: probing core at pa=%p\n", V2P(DWC2_BASE));
  id = rd(GSNPSID);
  printf("dwc2: core id=%x\n", id);
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
  wr(GINTMSK, 0);
  wr(GINTSTS, 0xffffffff);
  wr(HCFG, 0);
  wr(GAHBCFG, GAHBCFG_DMA_EN);

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
      memset(ctrl_buf, 0, 4);
      if(control(1, 0xa3, USB_REQ_GET_STATUS, 0, idx,
                 ctrl_buf, 4) < 0){
        printf("dwc2: hub port %d status read failed\n", idx);
        continue;
      }
      printf("dwc2: hub port %d status=%x change=%x\n", idx,
             ctrl_buf[0] | ((uint16)ctrl_buf[1] << 8),
             ctrl_buf[2] | ((uint16)ctrl_buf[3] << 8));
      if(!(ctrl_buf[0] & 1))
        continue;
      port = idx;
      printf("dwc2: hub external port %d connected status=%x\n",
             port, ctrl_buf[0] | ((uint16)ctrl_buf[1] << 8));
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
      printf("dwc2: hub port=%d child class=%d vid=%x pid=%x mps=%d\n",
             port, root_class, vid, pid, ep0_mps);
      if(is_mt7601(vid, pid)){
        mt_found = 1;
        break;
      }
    }
    if(!mt_found){
      printf("usbnet: no MT7601U on external hub ports\n");
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
    },
    .nresource = 1,
  };
  static struct device_driver drv = {
    .name = "bcm2837-dwc2",
    .probe = dwc2_probe,
    .remove = dwc2_remove,
  };
  platform_device_register(&dev);
  platform_driver_register(&drv);
}
