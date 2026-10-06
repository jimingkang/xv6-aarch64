// VideoCore property mailbox, shared by drivers.
//
// One request at a time under mbox_lock.  The message lives in a cache-line
// aligned static buffer that is cleaned to RAM before the firmware reads it
// and invalidated before the ARM reads the reply.  As noted in sd.c, this
// board's firmware wants the plain ARM physical address in the mailbox word,
// not the 0xC0000000 VideoCore alias.
//
// Robustness, as in the Circle bare-metal library (bcmmailbox.cpp,
// bcmpropertytags.cpp), which drives the firmware GPIO expander this way:
//  - stale replies are drained before writing: the other drivers' private
//    mailbox code may give up on a slow reply and leave it in the FIFO;
//  - a reply only counts if it is the very word we wrote (our buffer
//    address + channel), not merely any channel-8 reply;
//  - the tag's request/response word is zero on submission, exactly as in
//    Linux rpi_firmware_property().  Although the protocol description calls
//    this a request length, the Raspberry Pi firmware GPIO tags require the
//    Linux convention on some firmware builds.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "mbox.h"

#define MBOX_BASE    (PERIPHERAL_BASE + 0x0000b880UL)
#define MBOX_READ    0x00
#define MBOX_STATUS  0x18
#define MBOX_WRITE   0x20
#define MBOX_EMPTY   0x40000000U
#define MBOX_FULL    0x80000000U
#define MBOX_PROP_CH 8U
#define MBOX_SUCCESS 0x80000000U

#define MBOX_MSG_WORDS 32
#define CACHE_LINE     64

static struct spinlock mbox_lock;
static int mbox_ready;
static struct mbox_result last;     // protected by mbox_lock
static uint stale;                  // replies that were not ours
static volatile uint32 msg[MBOX_MSG_WORDS] __attribute__((aligned(CACHE_LINE)));

static inline uint32
mbox_rd(uint32 off)
{
  return *(volatile uint32 *)(MBOX_BASE + off);
}

static void
msg_sync(int to_device)
{
  for(uint64 p = (uint64)msg; p < (uint64)msg + sizeof(msg); p += CACHE_LINE){
    if(to_device)
      asm volatile("dc cvac, %0" :: "r"(p) : "memory");
    else
      asm volatile("dc ivac, %0" :: "r"(p) : "memory");
  }
  asm volatile("dsb sy" ::: "memory");
}

// Send one tag.  value[] carries the request in and the response out.
// Returns 0 on success, -1 on timeout or a firmware error.
int
mbox_property(uint32 tag, uint32 *value, int value_bytes)
{
  int words = (value_bytes + 3) / 4;
  uint64 deadline;
  int ok = 0;

  if(words < 1 || words > MBOX_MSG_WORDS - 6)
    return -1;
  if(!mbox_ready){
    initlock(&mbox_lock, "mbox");
    mbox_ready = 1;
  }
  acquire(&mbox_lock);

  msg[0] = (6 + words) * 4;       // total size
  msg[1] = 0;                     // request
  msg[2] = tag;
  msg[3] = words * 4;             // value buffer size
  // Linux drivers/firmware/raspberrypi.c:rpi_firmware_property() always
  // submits req_resp_size == 0.  Do not put value_bytes here: clock tags may
  // tolerate that, but SET_GPIO_CONFIG/STATE on Pi 3 firmware can leave the
  // response bit clear and silently ignore the tag.
  msg[4] = 0;
  for(int i = 0; i < words; i++)
    msg[5 + i] = value[i];
  msg[5 + words] = 0;             // end tag
  msg_sync(1);

  last.err = MBOX_ERR_TIMEOUT;
  last.status = last.tag_resp = last.value0 = 0;
  deadline = r_cntvct_el0() + r_cntfrq_el0();      // 1 s
  uint32 word = ((uint32)V2P(msg) & ~0xfU) | MBOX_PROP_CH;
  while(!(mbox_rd(MBOX_STATUS) & MBOX_EMPTY)){     // drain stale replies
    (void)mbox_rd(MBOX_READ);
    stale++;
  }
  while(mbox_rd(MBOX_STATUS) & MBOX_FULL)
    if(r_cntvct_el0() >= deadline)
      goto out;
  *(volatile uint32 *)(MBOX_BASE + MBOX_WRITE) = word;
  for(;;){
    while(mbox_rd(MBOX_STATUS) & MBOX_EMPTY)
      if(r_cntvct_el0() >= deadline)
        goto out;
    if(mbox_rd(MBOX_READ) == word)
      break;
    stale++;                      // someone else's reply: keep waiting
  }
  msg_sync(0);
  last.status = msg[1];
  last.tag_resp = msg[4];
  last.value0 = msg[5];
  last.stale = stale;
  if(msg[1] != MBOX_SUCCESS)
    last.err = MBOX_ERR_REQUEST;
  else {
    // Match Linux rpi_firmware_property_list(): success is determined by
    // the status of the whole property message.  Linux does not reject a
    // reply merely because an individual tag did not set bit 31 in its
    // req_resp_size word; it copies the payload back and lets the consumer
    // validate it.  The firmware GPIO ABI uses value[0] == 0 as that
    // operation-specific success indication.
    for(int i = 0; i < words; i++)
      value[i] = msg[5 + i];
    last.err = (msg[4] & MBOX_SUCCESS) ? MBOX_OK : MBOX_ERR_TAG;
    ok = 1;
  }
out:
  release(&mbox_lock);
  return ok ? 0 : -1;
}

void
mbox_last_result(struct mbox_result *r)
{
  if(!mbox_ready){
    memset(r, 0, sizeof(*r));
    return;
  }
  acquire(&mbox_lock);
  *r = last;
  release(&mbox_lock);
}

char *
mbox_strerror(int err)
{
  switch(err){
  case MBOX_OK:          return "ok";
  case MBOX_ERR_TIMEOUT: return "no reply";
  case MBOX_ERR_REQUEST: return "message rejected";
  case MBOX_ERR_TAG:     return "tag response bit not set";
  case MBOX_ERR_STATUS:  return "firmware reported an error";
  }
  return "?";
}

// Firmware build time (Unix seconds), 0 on failure.
uint32
mbox_get_firmware_rev(void)
{
  uint32 v[1] = { 0 };
  if(mbox_property(MBOX_TAG_GET_FIRMWARE_REV, v, sizeof(v)) < 0)
    return 0;
  return v[0];
}

uint32
mbox_get_clock_rate(uint32 clock_id)
{
  uint32 v[2] = { clock_id, 0 };
  if(mbox_property(MBOX_TAG_GET_CLOCK_RATE, v, sizeof(v)) < 0)
    return 0;
  return v[1];
}

int
mbox_set_domain(uint32 domain, int on)
{
  uint32 v[2] = { domain, on ? 1 : 0 };
  return mbox_property(MBOX_TAG_SET_DOMAIN_STATE, v, sizeof(v));
}

// The GPIO tags answer 0 in their first word on success and the GPIO
// number (non-zero) on error: Linux gpio-raspberrypi-exp.c checks
// "ret || set.gpio != 0".
static int
gpio_tag(uint32 tag, uint32 *v, int bytes)
{
  if(mbox_property(tag, v, bytes) < 0)
    return -1;
  if(v[0] != 0){
    acquire(&mbox_lock);
    last.err = MBOX_ERR_STATUS;
    release(&mbox_lock);
    return -1;
  }
  return 0;
}

// Drive a firmware expander GPIO as an output (gpio-raspberrypi-exp.c).
// Like Linux, configure it as an output first; if this firmware does not
// know SET_GPIO_CONFIG, still try SET_GPIO_STATE, which is older.  On
// failure mbox_last_result() describes the last tag tried.
int
mbox_set_expgpio(uint32 gpio, int value, struct mbox_result diag[2])
{
  // { gpio, direction=out, polarity, term_en, term_pull_up, state }
  uint32 cfg[6] = { gpio, 1, 0, 0, 0, value ? 1 : 0 };
  uint32 getcfg[5] = { gpio, 0, 0, 0, 0 };
  uint32 st[2] = { gpio, value ? 1 : 0 };
  int cfg_ok, st_ok;

  // Linux gpio-raspberrypi-exp.c preserves firmware-selected polarity.
  // CAM_GPIO0 is active-high on Pi 3, but retaining the setting also avoids
  // silently changing the meaning of the line on another board revision.
  if(gpio_tag(MBOX_TAG_GET_GPIO_CONFIG, getcfg, sizeof(getcfg)) == 0)
    cfg[2] = getcfg[2];
  cfg_ok = gpio_tag(MBOX_TAG_SET_GPIO_CONFIG, cfg, sizeof(cfg)) == 0;
  if(diag)
    mbox_last_result(&diag[0]);
  st_ok = gpio_tag(MBOX_TAG_SET_GPIO_STATE, st, sizeof(st)) == 0;
  if(diag)
    mbox_last_result(&diag[1]);
  return (cfg_ok || st_ok) ? 0 : -1;
}

int
mbox_get_expgpio(uint32 gpio, int *value)
{
  uint32 st[2] = { gpio, 0 };

  if(value == 0 || gpio_tag(MBOX_TAG_GET_GPIO_STATE, st, sizeof(st)) < 0)
    return -1;
  *value = st[1] != 0;
  return 0;
}

int
mbox_get_expgpio_config(uint32 gpio, int *direction, int *polarity)
{
  // { gpio, direction, polarity, term_en, term_pull_up }
  uint32 cfg[5] = { gpio, 0, 0, 0, 0 };

  if(gpio_tag(MBOX_TAG_GET_GPIO_CONFIG, cfg, sizeof(cfg)) < 0)
    return -1;
  if(direction)
    *direction = cfg[1] != 0;
  if(polarity)
    *polarity = cfg[2] != 0;
  return 0;
}
