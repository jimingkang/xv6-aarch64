/* FriendlyARM Tiny210 GPIO keys.
 *
 * K1..K4: GPH2[0..3] / EINT16..19
 * K5..K8: GPH3[0..3] / EINT24..27
 *
 * EINT16..31 share VIC0 IRQ 16.  The hard IRQ handler masks and
 * acknowledges only the pins that fired.  A delayed worker samples the
 * active-low GPIO state 20 ms later and submits one evdev frame through the
 * common input subsystem.  Press and release therefore appear as
 * EV_KEY(value 1/0), followed by EV_SYN/SYN_REPORT.
 */
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/device.h"
#include "kernel/input.h"
#include "kernel/module.h"
#include "kernel/workqueue.h"
#include "kernel/arch/arm/irq.h"

#define GPH2CON       0x0c40
#define GPH2DAT       0x0c44
#define GPH2PUD       0x0c48
#define GPH3CON       0x0c60
#define GPH3DAT       0x0c64
#define GPH3PUD       0x0c68
#define EXT_INT_2_CON 0x0e08
#define EXT_INT_3_CON 0x0e0c
#define EXT_INT_2_MASK 0x0f08
#define EXT_INT_3_MASK 0x0f0c
#define EXT_INT_2_PEND 0x0f48
#define EXT_INT_3_PEND 0x0f4c

#define KEY_BITS       0x0fU
#define EINT_BOTH_EDGE 4U
#define GPIO_EINT_FUNC 0x0fU
#define GPIO_PULL_UP   2U
#define DEBOUNCE_TICKS 2U       /* arm32 timer tick is 10 ms */

struct tiny210_key {
  int bank;
  int bit;
  int code;
  char *name;
};

static const struct tiny210_key keymap[] = {
  { 2, 0, KEY_1, "K1" }, { 2, 1, KEY_2, "K2" },
  { 2, 2, KEY_3, "K3" }, { 2, 3, KEY_4, "K4" },
  { 3, 0, KEY_5, "K5" }, { 3, 1, KEY_6, "K6" },
  { 3, 2, KEY_7, "K7" }, { 3, 3, KEY_8, "K8" },
};

struct tiny210_keys {
  uintptr gpio;
  int irq;
  struct input_dev *input;
  struct spinlock lock;
  struct delayed_work debounce_work;
  uint32 pending2;
  uint32 pending3;
  int debounce_armed;
};

static struct tiny210_keys keys;
#define REG(d, off) (*(volatile uint32*)((d)->gpio + (off)))

static void
configure_bank(struct tiny210_keys *d, uint conoff, uint pudoff,
               uint eintcon)
{
  uint32 con = REG(d, conoff), pud = REG(d, pudoff), trigger = REG(d, eintcon);

  for(int bit = 0; bit < 4; bit++){
    con = (con & ~(0xfU << (4 * bit))) |
          (GPIO_EINT_FUNC << (4 * bit));
    pud = (pud & ~(3U << (2 * bit))) |
          (GPIO_PULL_UP << (2 * bit));
    trigger = (trigger & ~(0xfU << (4 * bit))) |
              (EINT_BOTH_EDGE << (4 * bit));
  }
  REG(d, conoff) = con;
  REG(d, pudoff) = pud;
  REG(d, eintcon) = trigger;
}

static int
key_pressed(const struct tiny210_key *key, uint32 dat2, uint32 dat3)
{
  uint32 dat = key->bank == 2 ? dat2 : dat3;
  return (dat & (1U << key->bit)) == 0;       /* switches close to ground */
}

static void
keys_debounce_work(struct work_struct *work)
{
  struct tiny210_keys *d = (struct tiny210_keys *)
    ((char*)work - __builtin_offsetof(struct tiny210_keys, debounce_work.work));
  uint32 p2, p3, dat2, dat3;

  acquire(&d->lock);
  p2 = d->pending2;
  p3 = d->pending3;
  d->pending2 = d->pending3 = 0;
  d->debounce_armed = 0;
  release(&d->lock);

  /* Discard bounce edges accumulated while masked, sample the stable level,
     then unmask.  A transition after the clear sets PEND again and causes a
     new IRQ as soon as the line is unmasked. */
  if(p2) REG(d, EXT_INT_2_PEND) = p2;
  if(p3) REG(d, EXT_INT_3_PEND) = p3;
  dat2 = REG(d, GPH2DAT);
  dat3 = REG(d, GPH3DAT);
  if(p2) REG(d, EXT_INT_2_MASK) &= ~p2;
  if(p3) REG(d, EXT_INT_3_MASK) &= ~p3;

  for(uint i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++){
    const struct tiny210_key *key = &keymap[i];
    uint32 changed = key->bank == 2 ? p2 : p3;
    if(changed & (1U << key->bit))
      input_report_key(d->input, key->code,
                       key_pressed(key, dat2, dat3));
  }
  input_sync(d->input);
}

static void
keys_irq(int irq, void *arg)
{
  struct tiny210_keys *d = arg;
  uint32 p2 = REG(d, EXT_INT_2_PEND) & KEY_BITS;
  uint32 p3 = REG(d, EXT_INT_3_PEND) & KEY_BITS;
  int arm = 0;

  (void)irq;
  if((p2 | p3) == 0)
    return;
  if(p2){
    REG(d, EXT_INT_2_MASK) |= p2;
    REG(d, EXT_INT_2_PEND) = p2;
  }
  if(p3){
    REG(d, EXT_INT_3_MASK) |= p3;
    REG(d, EXT_INT_3_PEND) = p3;
  }

  acquire(&d->lock);
  d->pending2 |= p2;
  d->pending3 |= p3;
  if(!d->debounce_armed){
    d->debounce_armed = 1;
    arm = 1;
  }
  release(&d->lock);
  if(arm)
    schedule_delayed_work(&d->debounce_work, DEBOUNCE_TICKS);
}

static int
keys_probe(struct device *dev)
{
  struct tiny210_keys *d = &keys;
  uint32 dat2, dat3;

  if(dev->nresource < 2 || dev->resource[0].type != IORESOURCE_MEM ||
     dev->resource[1].type != IORESOURCE_IRQ || d->input)
    return -1;
  memset(d, 0, sizeof(*d));
  d->gpio = (uintptr)dev->resource[0].start;
  d->irq = (int)dev->resource[1].start;
  initlock(&d->lock, "tiny210-keys");
  init_delayed_work(&d->debounce_work, keys_debounce_work);

  d->input = input_allocate_device();
  if(d->input == 0)
    return -1;
  d->input->name = "Tiny210 GPIO Keys";
  d->input->phys = "s5pv210/gpio-keys0";
  d->input->bustype = BUS_HOST;
  d->input->private = d;
  for(uint i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
    input_set_capability(d->input, EV_KEY, keymap[i].code);
  if(input_register_device(d->input) < 0){
    input_free_device(d->input);
    d->input = 0;
    return -1;
  }

  REG(d, EXT_INT_2_MASK) |= KEY_BITS;
  REG(d, EXT_INT_3_MASK) |= KEY_BITS;
  configure_bank(d, GPH2CON, GPH2PUD, EXT_INT_2_CON);
  configure_bank(d, GPH3CON, GPH3PUD, EXT_INT_3_CON);
  REG(d, EXT_INT_2_PEND) = KEY_BITS;
  REG(d, EXT_INT_3_PEND) = KEY_BITS;
  if(request_irq(d->irq, keys_irq, d) < 0){
    input_unregister_device(d->input);
    input_free_device(d->input);
    d->input = 0;
    return -1;
  }

  /* Seed input-core state for keys already held during boot. */
  dat2 = REG(d, GPH2DAT);
  dat3 = REG(d, GPH3DAT);
  for(uint i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
    if(key_pressed(&keymap[i], dat2, dat3))
      input_report_key(d->input, keymap[i].code, 1);
  input_sync(d->input);

  REG(d, EXT_INT_2_MASK) &= ~KEY_BITS;
  REG(d, EXT_INT_3_MASK) &= ~KEY_BITS;
  dev->driver_data = d;
  printf("input: Tiny210 K1-K8 GPIO keys on EINT16-19/24-27, IRQ %d\n",
         d->irq);
  return 0;
}

static void
keys_remove(struct device *dev)
{
  struct tiny210_keys *d = dev->driver_data;
  if(d == 0 || d->input == 0)
    return;
  REG(d, EXT_INT_2_MASK) |= KEY_BITS;
  REG(d, EXT_INT_3_MASK) |= KEY_BITS;
  free_irq(d->irq, d);
  cancel_delayed_work_sync(&d->debounce_work);
  input_unregister_device(d->input);
  input_free_device(d->input);
  d->input = 0;
  dev->driver_data = 0;
}

static struct device_driver keys_driver = {
  .name = "s5pv210-keys",
  .bus = &platform_bus,
  .probe = keys_probe,
  .remove = keys_remove,
};

static void keys_init(void) { platform_driver_register(&keys_driver); }
module_init("s5pv210-keys", keys_init);
