// EHCI host controller driver (USB 2.0 high speed), for the S5PV210 USB
// host port (Tiny210 SDK USB-A sockets, 0xEC200000, IRQ 55).
//
// Everything runs on the asynchronous schedule: a ring of queue heads (QH),
// one per endpoint in use, started from a dummy head QH. Transfers are
// chains of qTDs appended to an endpoint's QH with Linux's dummy-qTD
// method, so the bulk-in QH can keep several receive buffers queued while
// control and bulk-out transfers proceed on their own QHs.
//
//   control()            synchronous SETUP/DATA/STATUS on the device's EP0 QH
//   bulk()               synchronous bulk IN/OUT
//   bulk_rx_arm()        queue one receive buffer on a bulk-in QH
//   bulk_rx_complete()   reap the oldest finished receive buffer (-2: none)
//
// Completion interrupts (IOC) wake waiters and tell mt7601u that receive
// buffers finished. Devices behind a high-speed hub are enumerated too;
// full/low-speed devices behind a hub use split transactions, full/low-speed
// devices on the root port belong to the companion OHCI and are skipped.
// Caches are off in the ARM32 port, so descriptors and buffers need no
// cache maintenance; physical = virtual.
#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/proc.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/module.h"
#include "kernel/usb.h"
#include "kernel/arch/arm/irq.h"
#include "ehci.h"

// Capability and operational registers (EHCI 1.0, sections 2.2 and 2.3).
#define CAPLENGTH   0x00
#define HCSPARAMS   0x04
#define HCCPARAMS   0x08
#define INSNREG00   0x90        // Samsung: AHB burst enables (Linux ehci-exynos/s5p)
#define INSNREG01   0x94        // Synopsys: packet buffer OUT [31:16] / IN [15:0] thresholds, dwords
#define USBCMD      0x00
#define USBSTS      0x04
#define USBINTR     0x08
#define CTRLDSSEG   0x10
#define ASYNCLIST   0x18
#define CONFIGFLAG  0x40
#define PORTSC(n)   (0x44 + 4*(n))

#define CMD_RUN     (1U << 0)
#define CMD_RESET   (1U << 1)
#define CMD_ASE     (1U << 5)
#define CMD_ITC(n)  ((uint32)(n) << 16)
#define STS_USBINT  (1U << 0)
#define STS_ERR     (1U << 1)
#define STS_PCD     (1U << 2)
#define STS_HALTED  (1U << 12)
#define STS_ASS     (1U << 15)
#define PORT_CCS    (1U << 0)
#define PORT_CSC    (1U << 1)
#define PORT_PE     (1U << 2)
#define PORT_PEC    (1U << 3)
#define PORT_OCC    (1U << 5)
#define PORT_RESET  (1U << 8)
#define PORT_LS(p)  (((p) >> 10) & 3)
#define PORT_POWER  (1U << 12)
#define PORT_OWNER  (1U << 13)
#define PORT_W1C    (PORT_CSC | PORT_PEC | PORT_OCC)

// Link pointers and qTD token (EHCI 3.5, 3.6).
#define LINK_T      1U
#define LINK_QH     (1U << 1)
#define TOK_ACTIVE  (1U << 7)
#define TOK_HALTED  (1U << 6)
#define TOK_ERRMASK 0x7cU       // halted, buffer, babble, xact, missed uframe
#define TOK_PID_OUT   (0U << 8)
#define TOK_PID_IN    (1U << 8)
#define TOK_PID_SETUP (2U << 8)
#define TOK_CERR3   (3U << 10)
#define TOK_IOC     (1U << 15)
#define TOK_BYTES(n) ((uint32)(n) << 16)
#define TOK_LEFT(t) (((t) >> 16) & 0x7fff)
#define TOK_DT      (1U << 31)

#define SPEED_FULL  0
#define SPEED_LOW   1
#define SPEED_HIGH  2

// Hardware parts include the 64-bit addressing extension (EHCI appendix B:
// buf_hi[5]). A controller with HCCPARAMS.64BIT reads and *writes back*
// those words, so the software fields must come after them, as in Linux.
struct qtd {
  volatile uint32 next, alt, token, buf[5];
  volatile uint32 buf_hi[5];
  // software fields
  struct qtd *snext;
  void *data;
  int length;
  int inuse;
} __attribute__((aligned(32)));

struct qh {
  volatile uint32 horiz, epchar, epcap, cur;
  volatile uint32 next, alt, token, buf[5];   // transfer overlay
  volatile uint32 buf_hi[5];
  // software fields
  struct qtd *dummy;      // terminal inactive qTD the HC is parked on
  struct qtd *head;       // oldest queued qTD (rx reaping)
  int in_ring;
} __attribute__((aligned(32)));

#define NQH  16
#define NQTD 96
static struct qh qhs[NQH];
static struct qtd qtds[NQTD];
static int nqh_used;

static uintptr cap, op;
static struct spinlock ehci_lock;      // descriptor pools and qTD queues
static struct sleeplock xfer_lock;     // one synchronous transfer at a time
static struct ehci_platdata *pdata;
static int nports;
static struct qh *head_qh;

static inline uint32 rd(uint32 off) { return *(volatile uint32*)(op + off); }
static inline void wr(uint32 off, uint32 v) { *(volatile uint32*)(op + off) = v; }
static inline uint32 phys(void *p) { return (uint32)V2P(p); }

static void
udelay(uint32 us)
{
  uint64 t = ((uint64)r_cntfrq_el0() * us + 999999) / 1000000;
  uint64 s = r_cntvct_el0();
  while(r_cntvct_el0() - s < t)
    ;
}
static void mdelay(uint32 ms) { udelay(ms * 1000); }

// ---- descriptor pools ----------------------------------------------------

static struct qtd *
qtd_alloc(void)
{
  acquire(&ehci_lock);
  for(int i = 0; i < NQTD; i++)
    if(!qtds[i].inuse){
      memset(&qtds[i], 0, sizeof(qtds[i]));
      qtds[i].inuse = 1;
      qtds[i].next = qtds[i].alt = LINK_T;
      qtds[i].token = TOK_HALTED;       // inactive and halted: never executed
      release(&ehci_lock);
      return &qtds[i];
    }
  release(&ehci_lock);
  return 0;
}

static void
qtd_free(struct qtd *t)
{
  acquire(&ehci_lock);
  t->inuse = 0;
  release(&ehci_lock);
}

// Fill a qTD's buffer pointers; a qTD covers at most 5 pages.
static int
qtd_fill(struct qtd *t, void *data, int len, uint32 token)
{
  uint32 a = data ? phys(data) : 0;
  int max = 0x5000 - (a & 0xfff);
  if(len > max)
    len = max;
  t->buf[0] = a;
  for(int i = 1; i < 5; i++)
    t->buf[i] = (a & ~0xfffU) + 0x1000U*i;
  t->data = data;
  t->length = len;
  t->token = token | TOK_BYTES(len) | TOK_CERR3;   // still inactive: no ACTIVE bit yet
  return len;
}

// ---- queue heads ---------------------------------------------------------

static struct qh *
qh_create(int addr, int ep, int speed, int mps, int control, int hub, int port)
{
  if(nqh_used >= NQH)
    return 0;
  struct qh *q = &qhs[nqh_used++];
  memset(q, 0, sizeof(*q));
  q->dummy = qtd_alloc();
  if(q->dummy == 0)
    return 0;
  q->epchar = (uint32)addr | ((uint32)ep << 8) | ((uint32)speed << 12) |
              ((uint32)mps << 16) | (8U << 28);          // NAK reload 8
  if(control)
    q->epchar |= 1U << 14;                              // DTC: toggle from qTD
  if(control && speed != SPEED_HIGH)
    q->epchar |= 1U << 27;                              // FS/LS control endpoint
  q->epcap = 1U << 30;                                  // Mult 1
  if(speed != SPEED_HIGH)
    q->epcap |= ((uint32)hub << 16) | ((uint32)port << 23);   // split target
  q->cur = 0;
  q->next = phys(q->dummy);
  q->alt = LINK_T;
  q->token = 0;
  q->head = 0;
  // Link after the head QH; a single aligned store publishes it.
  q->horiz = head_qh->horiz;
  dsb();
  head_qh->horiz = phys(q) | LINK_QH;
  dsb();
  q->in_ring = 1;
  return q;
}

// Append a chain (first..last, last->next = new dummy) to q: the old dummy
// becomes the first real qTD, so the HC, parked on the old dummy, picks the
// work up when its token turns active. Returns the qTD now holding `first`'s
// contents (the old dummy).
static struct qtd *
qh_append(struct qh *q, struct qtd *first, struct qtd *last)
{
  struct qtd *nd = qtd_alloc();
  if(nd == 0)
    return 0;
  acquire(&ehci_lock);
  struct qtd *d = q->dummy;
  uint32 tok = first->token;
  // copy first into the dummy (all but the token), relink the chain
  d->alt = LINK_T;
  for(int i = 0; i < 5; i++) d->buf[i] = first->buf[i];
  d->data = first->data;
  d->length = first->length;
  if(first == last){
    d->next = phys(nd);
    d->snext = nd;
  } else {
    d->next = first->next;
    d->snext = first->snext;
    last->next = phys(nd);
    last->snext = nd;
  }
  q->dummy = nd;
  if(q->head == 0)
    q->head = d;
  dsb();
  // Activate everything after the first qTD, then the first one last.
  for(struct qtd *t = d->snext; t && t != nd; t = t->snext)
    t->token |= TOK_ACTIVE;
  dsb();
  d->token = tok | TOK_ACTIVE;
  dsb();
  release(&ehci_lock);
  if(first != d)
    qtd_free(first);
  return d;
}

// Stop the async schedule so a QH can be repaired after a timeout or error.
static void
async_stop(void)
{
  wr(USBCMD, rd(USBCMD) & ~CMD_ASE);
  for(int i = 0; i < 2000 && (rd(USBSTS) & STS_ASS); i++)
    udelay(10);
}
static void
async_start(void)
{
  wr(USBCMD, rd(USBCMD) | CMD_ASE);
  for(int i = 0; i < 2000 && !(rd(USBSTS) & STS_ASS); i++)
    udelay(10);
}

// Drop queued qTDs from a QH (from `from` to the dummy) and park it on the
// dummy again. Async schedule must be stopped.
static void
qh_flush(struct qh *q, struct qtd *from)
{
  for(struct qtd *t = from; t && t != q->dummy; ){
    struct qtd *n = t->snext;
    t->token = TOK_HALTED;
    qtd_free(t);
    t = n;
  }
  if(q->head == from)
    q->head = 0;
  q->next = phys(q->dummy);
  q->alt = LINK_T;
  // Clear Active/Halted/Ping but keep the data toggle: the device did not
  // reset its toggle, and a DATA0/DATA1 mismatch makes it silently drop
  // every following packet as a retransmission.
  q->token &= TOK_DT;
  q->cur = 0;
}

// Wait until qTD `last` is no longer active; returns its token or -1 on timeout.
static long long
wait_qtd(struct qtd *last, uint32 timeout_us)
{
  uint64 t = ((uint64)r_cntfrq_el0() * timeout_us) / 1000000;
  uint64 s = r_cntvct_el0();
  for(;;){
    uint32 tok = last->token;
    if(!(tok & TOK_ACTIVE))
      return tok;
    if(r_cntvct_el0() - s > t)
      return -1;
  }
}

// ---- control and bulk ----------------------------------------------------

struct ehci_dev {          // host_private of a usb_device
  int speed, hub, port;
  struct qh *ep0;
  struct qh *ep[2][16];    // [in][ep]
};
#define NDEV_MAX 8
static struct ehci_dev edevs[NDEV_MAX];
static int nedev;

static uint8 setup_buf[8] __attribute__((aligned(32)));
static uint8 ctrl_buf[1024] __attribute__((aligned(32)));

static int
control_qh(struct qh *q, uint8 type, uint8 request, uint16 value, uint16 index,
           void *data, uint16 length)
{
  struct qtd *s = qtd_alloc(), *d = 0, *st = qtd_alloc();
  if(s == 0 || st == 0)
    return -1;
  int in = type & 0x80;
  uint8 *sb = setup_buf;
  sb[0] = type; sb[1] = request; sb[2] = value; sb[3] = value >> 8;
  sb[4] = index; sb[5] = index >> 8; sb[6] = length; sb[7] = length >> 8;
  // Device-to-host data lands in ctrl_buf first: the caller's buffer may sit
  // on the stack of another context, or be unaligned.
  if(length > sizeof(ctrl_buf))
    return -1;
  if(!in && length)
    memmove(ctrl_buf, data, length);
  qtd_fill(s, sb, 8, TOK_PID_SETUP);
  if(length){
    d = qtd_alloc();
    if(d == 0)
      return -1;
    qtd_fill(d, ctrl_buf, length, (in ? TOK_PID_IN : TOK_PID_OUT) | TOK_DT);
  }
  qtd_fill(st, 0, 0, (in && length ? TOK_PID_OUT : TOK_PID_IN) | TOK_DT | TOK_IOC);
  s->snext = d ? d : st; s->next = phys(s->snext);
  if(d){ d->snext = st; d->next = phys(st); }
  struct qtd *first = qh_append(q, s, st);
  if(first == 0)
    return -1;
  long long tok = wait_qtd(st, 1000000);
  int r;
  if(tok < 0 || (tok & TOK_ERRMASK)){
    // find the failing stage for the log
    uint32 bad = tok < 0 ? 0xffffffffU : (uint32)tok;
    for(struct qtd *t = first; t && t != q->dummy; t = t->snext)
      if(t->token & (TOK_ACTIVE | TOK_ERRMASK)){ bad = t->token; break; }
    printf("ehci: control %x/%x val=%x failed token=%x\n", type, request, value, bad);
    async_stop();
    qh_flush(q, first);
    async_start();
    return -1;
  }
  r = length;
  if(d)
    r = d->length - TOK_LEFT(d->token);
  if(in && r > 0)
    memmove(data, ctrl_buf, r);
  for(struct qtd *t = first; t && t != q->dummy; ){
    struct qtd *n = t->snext; qtd_free(t); t = n;
  }
  q->head = 0;
  return r;
}

static struct qh *
ep_qh(struct usb_device *udev, int ep, int in)
{
  struct ehci_dev *e = udev->host_private;
  if(e == 0 || ep <= 0 || ep > 15)
    return 0;
  if(e->ep[in][ep] == 0){
    int mps = in ? (ep == udev->bulk_in_ep2 ? udev->bulk_in_max_packet : udev->bulk_in_max_packet)
                 : udev->bulk_out_max_packet;
    if(mps == 0)
      mps = e->speed == SPEED_HIGH ? 512 : 64;
    e->ep[in][ep] = qh_create(udev->address, ep, e->speed, mps, 0, e->hub, e->port);
  }
  return e->ep[in][ep];
}

static int
ehci_usb_control(struct usb_device *udev, uint8 type, uint8 request,
                 uint16 value, uint16 index, void *data, uint16 length)
{
  struct ehci_dev *e = udev->host_private;
  if(e == 0 || e->ep0 == 0)
    return -1;
  acquiresleep(&xfer_lock);
  int r = control_qh(e->ep0, type, request, value, index, data, length);
  releasesleep(&xfer_lock);
  return r;
}

// Synchronous bulk transfer; data may be any kernel buffer (DMA from DRAM).
// IN transfers use one qTD (<= 16 KiB) so a short packet completes it.
static int
bulk_qh(struct qh *q, int endpoint, int in, void *data, int length)
{
  if(in && length > 0x4000)
    return -1;
  struct qtd *first = 0, *last = 0;
  int off = 0;
  do {
    struct qtd *t = qtd_alloc();
    if(t == 0)
      return -1;
    int n = qtd_fill(t, (uint8*)data + off, length - off, in ? TOK_PID_IN : TOK_PID_OUT);
    off += n;
    if(last){ last->snext = t; last->next = phys(t); } else first = t;
    last = t;
  } while(off < length);
  last->token |= TOK_IOC;
  struct qtd *d = qh_append(q, first, last);
  if(d == 0)
    return -1;
  if(first == last)
    last = d;
  // OUT: a busy device may NAK (PING) for a while; give it 3 s.
  long long tok = wait_qtd(last, in ? 1000000 : 3000000);
  if(tok < 0 || (tok & TOK_ERRMASK)){
    if(tok >= 0 || !in){
      printf("ehci: bulk ep%d %s %d bytes failed token=%x\n", endpoint,
             in ? "in" : "out", length, tok < 0 ? 0xffffffffU : (uint32)tok);
      printf("ehci:   qh %x epchar %x epcap %x cur %x next %x alt %x token %x buf0 %x\n",
             phys(q), q->epchar, q->epcap, q->cur, q->next, q->alt, q->token, q->buf[0]);
      printf("ehci:   qtd %x token %x buf0 %x; usbsts %x usbcmd %x\n",
             phys(last), last->token, last->buf[0], rd(USBSTS), rd(USBCMD));
    }
    async_stop();
    qh_flush(q, d);
    async_start();
    return -1;
  }
  int done = 0;
  for(struct qtd *t = d; t && t != q->dummy; ){
    struct qtd *n = t->snext;
    done += t->length - TOK_LEFT(t->token);
    qtd_free(t);
    t = n;
  }
  q->head = 0;
  return done;
}

static int
ehci_usb_bulk(struct usb_device *udev, int endpoint, int in, void *data, int length)
{
  struct qh *q = ep_qh(udev, endpoint, in);
  if(q == 0 || length < 0)
    return -1;
  acquiresleep(&xfer_lock);
  int r = bulk_qh(q, endpoint, in, data, length);
  releasesleep(&xfer_lock);
  return r;
}

static int
ehci_usb_bulk_rx_arm(struct usb_device *udev, int endpoint, void *data, int length)
{
  struct qh *q = ep_qh(udev, endpoint, 1);
  if(q == 0 || data == 0 || length <= 0 || length > 0x4000)
    return -1;
  struct qtd *t = qtd_alloc();
  if(t == 0)
    return -1;
  qtd_fill(t, data, length, TOK_PID_IN | TOK_IOC);
  return qh_append(q, t, t) ? 0 : -1;
}

static int
ehci_usb_bulk_rx_complete(struct usb_device *udev, int endpoint, void **data)
{
  struct qh *q = ep_qh(udev, endpoint, 1);
  if(data)
    *data = 0;
  if(q == 0)
    return -1;
  acquire(&ehci_lock);
  struct qtd *t = q->head;
  if(t == 0 || t == q->dummy || (t->token & TOK_ACTIVE)){
    release(&ehci_lock);
    return -2;
  }
  uint32 tok = t->token;
  q->head = t->snext == q->dummy ? 0 : t->snext;
  release(&ehci_lock);
  if(data)
    *data = t->data;
  int r = (tok & TOK_ERRMASK) ? -1 : t->length - (int)TOK_LEFT(tok);
  qtd_free(t);
  if(tok & TOK_HALTED){
    // a halted QH stops the whole queue: drop the rest, the driver re-arms
    async_stop();
    qh_flush(q, q->head);
    async_start();
  }
  return r;
}

static const struct usb_host_ops ehci_usb_ops = {
  .control = ehci_usb_control,
  .bulk = ehci_usb_bulk,
  .bulk_rx_arm = ehci_usb_bulk_rx_arm,
  .bulk_rx_complete = ehci_usb_bulk_rx_complete,
};

// ---- interrupt -----------------------------------------------------------

void mt7601u_rx_irq(void) __attribute__((weak));

static void
ehci_irq(int irq, void *arg)
{
  uint32 st = rd(USBSTS) & 0x3f;
  if(st == 0)
    return;
  wr(USBSTS, st);                 // W1C
  if(st & (STS_USBINT | STS_ERR))
    if(mt7601u_rx_irq)
      mt7601u_rx_irq();           // a receive buffer may have completed
}

// ---- enumeration ---------------------------------------------------------

#define USB_DT_DEVICE 1
#define USB_DT_CONFIG 2
#define USB_DT_INTERFACE 4
#define USB_DT_ENDPOINT 5
#define USB_DT_HUB 0x29
#define REQ_GET_STATUS 0
#define REQ_CLEAR_FEATURE 1
#define REQ_SET_FEATURE 3
#define REQ_SET_ADDRESS 5
#define REQ_GET_DESCRIPTOR 6
#define REQ_SET_CONFIG 9
#define HUB_PORT_RESET 4
#define HUB_PORT_POWER 8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_RESET 20

static struct usb_device udevs[NDEV_MAX];
static int next_address = 1;
static uint8 desc[512] __attribute__((aligned(4)));

static void enumerate(int speed, int hub, int port, int depth);

static void
parse_config(struct usb_device *u, uint8 *c, int total)
{
  int first_if = 1;
  for(int off = 0; off + 2 <= total && c[off] >= 2; off += c[off]){
    uint8 *d = c + off;
    if(d[1] == USB_DT_INTERFACE){
      if(!first_if)
        break;                    // only the first interface is used
      first_if = 0;
      u->interface_number = d[2];
      if(u->class == 0){ u->class = d[5]; u->subclass = d[6]; u->protocol = d[7]; }
    } else if(d[1] == USB_DT_ENDPOINT){
      int addr = d[2], attr = d[3] & 3, mps = (d[4] | d[5] << 8) & 0x7ff;
      if(attr == 2 && (addr & 0x80)){
        if(u->bulk_in_ep == 0){ u->bulk_in_ep = addr & 0xf; u->bulk_in_max_packet = mps; }
        else if(u->bulk_in_ep2 == 0) u->bulk_in_ep2 = addr & 0xf;
      } else if(attr == 2){
        if(u->bulk_out_ep == 0){ u->bulk_out_ep = addr & 0xf; u->bulk_out_max_packet = mps; }
        if(u->n_bulk_out < sizeof(u->bulk_out_eps)) u->bulk_out_eps[u->n_bulk_out++] = addr & 0xf;
      } else if(attr == 3 && (addr & 0x80) && u->interrupt_in_ep == 0){
        u->interrupt_in_ep = addr & 0xf;
        u->interrupt_in_max_packet = mps;
        u->interrupt_in_interval = d[6];
      }
    }
  }
}

static void
hub_ports(struct usb_device *h, int depth)
{
  int n = ehci_usb_control(h, 0xa0, REQ_GET_DESCRIPTOR, USB_DT_HUB << 8, 0, desc, 9);
  if(n < 3){
    printf("ehci: hub %d: no hub descriptor\n", h->address);
    return;
  }
  int ports = desc[2], pwr_ms = desc[5] * 2;
  printf("ehci: hub %d: %d ports\n", h->address, ports);
  for(int p = 1; p <= ports; p++)
    ehci_usb_control(h, 0x23, REQ_SET_FEATURE, HUB_PORT_POWER, p, 0, 0);
  mdelay(pwr_ms > 100 ? pwr_ms : 100);
  for(int p = 1; p <= ports; p++){
    uint8 *s = desc;
    if(ehci_usb_control(h, 0xa3, REQ_GET_STATUS, 0, p, s, 4) < 4 || !(s[0] & 1))
      continue;
    ehci_usb_control(h, 0x23, REQ_CLEAR_FEATURE, HUB_C_PORT_CONNECTION, p, 0, 0);
    ehci_usb_control(h, 0x23, REQ_SET_FEATURE, HUB_PORT_RESET, p, 0, 0);
    int ok = 0;
    for(int i = 0; i < 50; i++){
      mdelay(10);
      if(ehci_usb_control(h, 0xa3, REQ_GET_STATUS, 0, p, s, 4) == 4 && !(s[0] & 0x10)){
        ok = s[0] & 2;            // enabled after reset
        break;
      }
    }
    ehci_usb_control(h, 0x23, REQ_CLEAR_FEATURE, HUB_C_PORT_RESET, p, 0, 0);
    if(!ok){
      printf("ehci: hub %d port %d: reset failed\n", h->address, p);
      continue;
    }
    uint16 w = s[0] | s[1] << 8;
    int speed = (w & (1U << 10)) ? SPEED_HIGH : (w & (1U << 9)) ? SPEED_LOW : SPEED_FULL;
    mdelay(20);
    struct ehci_dev *he = h->host_private;
    // split transactions go to the nearest high-speed hub
    int tt_hub = he->speed == SPEED_HIGH ? h->address : he->hub;
    int tt_port = he->speed == SPEED_HIGH ? p : he->port;
    enumerate(speed, tt_hub, tt_port, depth + 1);
  }
}

static const char *speed_name[] = {"full", "low", "high"};

static void
enumerate(int speed, int hub, int port, int depth)
{
  if(nedev >= NDEV_MAX || depth > 4)
    return;
  struct ehci_dev *e = &edevs[nedev];
  struct usb_device *u = &udevs[nedev];
  memset(e, 0, sizeof(*e));
  memset(u, 0, sizeof(*u));
  e->speed = speed; e->hub = hub; e->port = port;
  u->ops = &ehci_usb_ops;
  u->host_private = e;
  // Address 0 EP0 QH for the descriptor/SET_ADDRESS phase.
  e->ep0 = qh_create(0, 0, speed, speed == SPEED_LOW ? 8 : 64, 1, hub, port);
  if(e->ep0 == 0)
    return;
  if(ehci_usb_control(u, 0x80, REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, desc, 8) < 8){
    printf("ehci: %s-speed device: no descriptor\n", speed_name[speed]);
    return;
  }
  int mps0 = desc[7] ? desc[7] : 64;
  int addr = next_address++;
  if(ehci_usb_control(u, 0x00, REQ_SET_ADDRESS, addr, 0, 0, 0) < 0)
    return;
  mdelay(10);
  e->ep0 = qh_create(addr, 0, speed, mps0, 1, hub, port);   // the address-0 QH stays idle
  if(e->ep0 == 0)
    return;
  u->address = addr;
  u->ep0_max_packet = mps0;
  if(ehci_usb_control(u, 0x80, REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, desc, 18) < 18)
    return;
  u->vendor = desc[8] | desc[9] << 8;
  u->product = desc[10] | desc[11] << 8;
  u->class = desc[4]; u->subclass = desc[5]; u->protocol = desc[6];
  if(ehci_usb_control(u, 0x80, REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, desc, 9) < 9)
    return;
  int total = desc[2] | desc[3] << 8, cfg = desc[5];
  if(total > (int)sizeof(desc)) total = sizeof(desc);
  if(ehci_usb_control(u, 0x80, REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, desc, total) < total)
    return;
  parse_config(u, desc, total);
  if(ehci_usb_control(u, 0x00, REQ_SET_CONFIG, cfg, 0, 0, 0) < 0)
    return;
  nedev++;
  printf("ehci: %s-speed device %x:%x class %d at address %d (hub %d port %d)\n",
         speed_name[speed], u->vendor, u->product, u->class, addr, hub, port);
  printf("ehci:   bulk in %d,%d/%d out %d/%d (%d OUT endpoints) intr in %d\n", u->bulk_in_ep,
         u->bulk_in_ep2, u->bulk_in_max_packet, u->bulk_out_ep, u->bulk_out_max_packet,
         u->n_bulk_out, u->interrupt_in_ep);
  if(u->class == 9){
    hub_ports(u, depth);
    return;
  }
  u->dev.name = "usb-device";
  if(usb_device_register(u) < 0)
    printf("ehci: cannot register %x:%x\n", u->vendor, u->product);
}

static int
root_ports(void)
{
  int found = 0;
  for(int p = 0; p < nports; p++){
    uint32 s = rd(PORTSC(p));
    if(!(s & PORT_CCS))
      continue;
    if(PORT_LS(s) == 1){          // K state: low speed, companion's job
      printf("ehci: port %d: low-speed device, left to OHCI\n", p);
      wr(PORTSC(p), (s & ~PORT_W1C) | PORT_OWNER);
      continue;
    }
    wr(PORTSC(p), (s & ~(PORT_W1C | PORT_PE)) | PORT_RESET);
    mdelay(50);
    wr(PORTSC(p), rd(PORTSC(p)) & ~(PORT_W1C | PORT_RESET));
    for(int i = 0; i < 100 && (rd(PORTSC(p)) & PORT_RESET); i++)
      udelay(20);
    mdelay(10);
    s = rd(PORTSC(p));
    if(!(s & PORT_PE)){
      printf("ehci: port %d: full-speed device, left to OHCI\n", p);
      wr(PORTSC(p), (s & ~PORT_W1C) | PORT_OWNER);
      continue;
    }
    printf("ehci: port %d: high-speed device connected\n", p);
    enumerate(SPEED_HIGH, 0, 0, 0);
    found++;
  }
  return found;
}

static int
ehci_start(void)
{
  if(pdata && pdata->phy_init)
    pdata->phy_init();
  op = cap + (*(volatile uint8*)(cap + CAPLENGTH));
  uint32 hcs = *(volatile uint32*)(cap + HCSPARAMS);
  nports = hcs & 0xf;
  *(volatile uint32*)(cap + INSNREG00) = (1U << 25) | (1U << 24) | (1U << 23) | (1U << 22);
  // Start an OUT packet only once the whole 512-byte packet is in the HC's
  // buffer (and drain IN in 512-byte units): with the default threshold the
  // controller underran (qTD status Data Buffer Error) while the CPU, running
  // uncached, kept the DRAM bus busy.
  uint32 thr = *(volatile uint32*)(cap + INSNREG01);
  *(volatile uint32*)(cap + INSNREG01) = (0x80U << 16) | 0x80U;
  printf("ehci: INSNREG01 thresholds %x -> %x\n", thr, *(volatile uint32*)(cap + INSNREG01));

  wr(USBCMD, rd(USBCMD) & ~CMD_RUN);
  for(int i = 0; i < 100 && !(rd(USBSTS) & STS_HALTED); i++)
    udelay(100);
  wr(USBCMD, CMD_RESET);
  for(int i = 0; i < 1000 && (rd(USBCMD) & CMD_RESET); i++)
    udelay(100);
  if(rd(USBCMD) & CMD_RESET){
    printf("ehci: controller reset timed out\n");
    return -1;
  }
  // Head of the async ring: H bit, halted dummy overlay, points to itself.
  head_qh = &qhs[nqh_used++];
  memset(head_qh, 0, sizeof(*head_qh));
  head_qh->horiz = phys(head_qh) | LINK_QH;
  head_qh->epchar = (1U << 15) | ((uint32)SPEED_HIGH << 12) | (64U << 16);
  head_qh->epcap = 1U << 30;
  head_qh->next = head_qh->alt = LINK_T;
  head_qh->token = TOK_HALTED;
  wr(CTRLDSSEG, 0);
  wr(ASYNCLIST, phys(head_qh));
  wr(USBINTR, STS_USBINT | STS_ERR);
  wr(USBCMD, CMD_ITC(1) | CMD_ASE | CMD_RUN);
  wr(CONFIGFLAG, 1);                      // route the ports to EHCI
  mdelay(5);
  for(int p = 0; p < nports; p++)
    if(hcs & (1U << 4))                   // PPC: per-port power control
      wr(PORTSC(p), (rd(PORTSC(p)) & ~PORT_W1C) | PORT_POWER);
  mdelay(100);                            // power good + device attach
  printf("ehci: %d port(s), hccparams %x, running, async schedule %s\n", nports,
         *(volatile uint32*)(cap + HCCPARAMS), (rd(USBSTS) & STS_ASS) ? "on" : "off");
  return 0;
}

// Enumeration waits for power and resets for hundreds of milliseconds, so
// it runs once the scheduler, interrupts and the timer are running.
static struct spinlock enum_lock;
static void
ehci_enum_thread(void *arg)
{
  (void)arg;
  if(ehci_start() == 0){
    int n = 0;
    for(int tries = 0; tries < 20 && n == 0; tries++){   // a device may attach late
      n = root_ports();
      if(n == 0) mdelay(100);
    }
    if(n == 0)
      printf("ehci: no high-speed device on the root port(s)\n");
  }
  acquire(&enum_lock);
  for(;;)
    sleep(&enum_lock, &enum_lock);   // hot-plug is not handled
}

static int
ehci_probe(struct device *dev)
{
  cap = (uintptr)dev->resource[0].start;
  pdata = dev->driver_data;
  initlock(&ehci_lock, "ehci");
  initsleeplock(&xfer_lock, "ehci-xfer");
  initlock(&enum_lock, "ehci-enum");
  if(request_irq((int)dev->resource[1].start, ehci_irq, 0) < 0)
    printf("ehci: cannot get IRQ %d\n", (int)dev->resource[1].start);
  if(kthread_create(ehci_enum_thread, 0, "ehci-enum") < 0)
    return -1;
  printf("ehci: s5p-ehci at %x, enumeration deferred to ehci-enum\n", (uint)cap);
  return 0;
}

static struct device_driver ehci_driver = { .name = "s5p-ehci", .bus = &platform_bus, .probe = ehci_probe };
static void ehci_register(void) { platform_driver_register(&ehci_driver); }
module_init_ord("ehci-s5p", ehci_register, 2);   // after usb class drivers (order 1)
