// The built-in module table: walk what the linker collected, run it in boot
// order, and format it for /proc/modules.
//
// Below the built-in walk lives the loadable (.ko) side: the same /proc listing
// also reports modules that were not linked into the kernel but pulled off
// /lib/modules and relocated at runtime, which is the =m half of the Linux
// module story this port mirrors.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "defs.h"
#include "elf.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "vfs.h"
#include "workqueue.h"
#include "module.h"

// kernel.ld gathers every .initcall.list object between these two symbols.
// They are addresses, not sized arrays, so the table length is the span
// divided by the element size.
extern const struct module_desc __initcall_start[];
extern const struct module_desc __initcall_end[];

// Loadable-module machinery, defined below the built-in walk.
static void dynamic_modules_init(void);
static void seterr(char *err, int errlen, char *msg);

// The request_module()/kmod autoload machinery, defined at the bottom of the
// file next to the other runtime-module code.
static void kmod_init(void);
static void kmod_run(struct work_struct *work);

// The kernel has strncmp but no strcmp/streq; section and module names are
// short NUL-terminated strings, so a bounded compare against an sentinel-length
// request is all str_eq needs to be.
static int str_eq(const char *a, const char *b);

#define MODULE_MAX 48

static char *level_name[INIT_LEVEL_COUNT] = {
  "early", "core", "postcore", "arch",
  "subsys", "fs", "device", "late",
};

static struct {
  const struct module_desc *mods[MODULE_MAX];
  int done[MODULE_MAX];   // one per mods[] entry: init() has returned
  int nmod;
  int nrun;
} module_core;

// True when a runs before b.  Equal (level, order) keeps link order, which is
// why the sort below is insertion sort: it is stable.
static int
earlier(const struct module_desc *a, const struct module_desc *b)
{
  if(a->level != b->level)
    return a->level < b->level;
  return a->order < b->order;
}

// Linux orders initcalls by the section ld emits them in.  Sorting a small
// array of pointers instead lets one section name carry every level, so the
// Makefile object order can never change when a driver is probed.
static void
modules_collect(void)
{
  int n = __initcall_end - __initcall_start;
  if(n > MODULE_MAX)
    panic("too many built-in modules");
  for(int i = 0; i < n; i++){
    const struct module_desc *m = &__initcall_start[i];
    int j = i - 1;
    while(j >= 0 && earlier(m, module_core.mods[j])){
      module_core.mods[j + 1] = module_core.mods[j];
      j--;
    }
    module_core.mods[j + 1] = m;
  }
  module_core.nmod = n;
}

// Called once by main(), after the memory allocator, scheduler tables,
// interrupt controller, buffer cache and file table are up.  Every driver that
// used to have a hand-written call there now runs from its table entry.
void
do_initcalls(void)
{
  dynamic_modules_init();
  modules_collect();
  printf("driver modules: %d entries in the initcall table\n",
         module_core.nmod);
  int level = -1;
  for(int i = 0; i < module_core.nmod; i++){
    const struct module_desc *m = module_core.mods[i];
    if(m->level != level){
      level = m->level;
      printf("initcall level %d (%s)\n", level, level_name[level]);
    }
    printf("  %s\n", m->name);
    m->init();
    module_core.done[i] = 1;
    module_core.nrun++;
  }
}

// ---------------------------------------------------------------------------
// Loadable modules (.ko): the runtime half.
//
// A built-in driver is linked in, so ld already knows every branch target and
// its struct module_desc sits in the kernel's own .initcall.list.  A .ko is the
// opposite: `ld -r` leaves it a relocatable object full of R_AARCH64_* holes,
// because at build time nobody knew where it would land or which kernel routine
// each `bl` would reach.  load_module_image() fills those holes: it reserves a
// contiguous run of arena memory for every SHF_ALLOC section, resolves each
// symbol either inside the module or against the kernel's export table, patches
// the relocations to point at their final home, then runs the descriptor the
// module left in its own .initcall.list.  Only after that is the driver live,
// exactly the do_init_module() -> do_one_initcall() ordering Linux uses.
// ---------------------------------------------------------------------------

#define DYN_NAME   32
#define DYN_MAX    8      // concurrently loaded .ko modules
#define SECT_MAX   32     // ELF sections a single module may declare

static struct {
  struct spinlock lock;
  uint64 top;             // bump offset into MODAREA, bytes used so far
  int nloaded;            // modules currently live
} modules;

struct loaded_module {
  int  used;
  char name[DYN_NAME];
  void (*exit)(void);     // cleanup registered by the module, may be 0
  uint64 base;            // arena address the module occupies
  uint64 size;            // bytes of arena in use
};

static struct loaded_module loaded[DYN_MAX];

static void
dynamic_modules_init(void)
{
  initlock(&modules.lock, "modules");
  modules.top = 0;
  modules.nloaded = 0;
  for(int i = 0; i < DYN_MAX; i++)
    loaded[i].used = 0;
  kmod_init();
}

static void seterr(char *err, int errlen, char *msg){
  if(err && errlen > 0){
    safestrcpy(err, msg, errlen);
  }
}

static int
str_eq(const char *a, const char *b)
{
  while(*a && *a == *b){ a++; b++; }
  return *a == 0 && *b == 0;
}

// Sign-safe 64-bit difference, the way the relocations below reason about
// distances between kernel code and module code.
typedef long s64;

// Read the symbol's value after folding in the relocation addend.  Defined
// symbols point into one of the module's own loaded sections (STT_SECTION
// symbols carry the whole section as base, value 0); undefined ones must be
// satisfied from the kernel's export table or the module is rejected.
static int
sym_value(struct elf64_sym *s, char *strtab, uint64 *sec_base,
          long addend, uint64 *out, char *err, int errlen)
{
  if(s->shndx == SHN_UNDEF){
    void *k = find_kernel_symbol(strtab + s->name);
    if(k == 0){
      seterr(err, errlen, "unresolved symbol");
      return -1;
    }
    *out = (uint64)k + addend;
    return 0;
  }
  if(s->shndx == SHN_ABS){
    *out = s->value + addend;
    return 0;
  }
  if(s->shndx >= SECT_MAX || sec_base[s->shndx] == 0){
    seterr(err, errlen, "symbol in unloaded section");
    return -1;
  }
  *out = sec_base[s->shndx] + s->value + addend;
  return 0;
}

// Patch one instruction so its embedded immediate becomes (S - P).  Used by the
// PC-relative relocs; AArch64 splits these immediates across two bit ranges, so
// each helper reassembles the fields after clearing them.
static void
set_bitfield(uint32 *w, uint32 mask, int shift, uint64 value)
{
  *w = (*w & ~mask) | ((uint32)value << shift);
}

static int
apply_rela(uint32 type, uint64 P, uint64 S, char *err, int errlen)
{
  s64 delta = (s64)S - (s64)P;
  switch(type){
  case R_AARCH64_NONE:
    return 0;
  case R_AARCH64_ABS64:
    *(uint64 *)P = S;
    return 0;
  case R_AARCH64_ABS32:
    *(uint32 *)P = (uint32)S;
    return 0;
  case R_AARCH64_ABS16:
    *(uint16 *)P = (uint16)S;
    return 0;
  case R_AARCH64_PREL64:
    *(uint64 *)P = (uint64)delta;
    return 0;
  case R_AARCH64_PREL32:
    *(uint32 *)P = (uint32)delta;
    return 0;
  case R_AARCH64_PREL16:
    *(uint16 *)P = (uint16)delta;
    return 0;
  case R_AARCH64_ADR_PREL_PG_HI21: {
    // adrp: page-aligned difference, 21-bit signed, split into immlo[30:29]
    // and immhi[23:5].
    s64 pages = (((s64)S & ~0xffLL) - ((s64)P & ~0xffLL)) >> 12;
    if(pages < -(1LL<<20) || pages >= (1LL<<20)){
      seterr(err, errlen, "adrp out of range");
      return -1;
    }
    uint32 *w = (uint32 *)P;
    set_bitfield(w, 0x60000000u, 29, (uint64)pages & 0x3);
    set_bitfield(w, 0x001ffffeu, 5, ((uint64)pages >> 2) & 0x7ffff);
    return 0;
  }
  case R_AARCH64_ADR_PREL_LO21: {
    // adr: byte difference within +/- 1MB, same two-field split, no page shift.
    if(delta < -(1LL<<20) || delta >= (1LL<<20)){
      seterr(err, errlen, "adr out of range");
      return -1;
    }
    uint32 *w = (uint32 *)P;
    set_bitfield(w, 0x60000000u, 29, (uint64)delta & 0x3);
    set_bitfield(w, 0x001ffffeu, 5, ((uint64)delta >> 2) & 0x7ffff);
    return 0;
  }
  case R_AARCH64_ADD_ABS_LO12_NC: {
    // add: low 12 bits of the absolute target, complementing the adrp above it.
    uint32 *w = (uint32 *)P;
    set_bitfield(w, 0x00fff000u, 10, S & 0xfff);
    return 0;
  }
  case R_AARCH64_JUMP26:
  case R_AARCH64_CALL26: {
    // b / bl: 26-bit signed *word* offset, so the reach is 2^25 words = 2^27
    // bytes (~128MB).  delta is a byte difference, so it must be tested against
    // the byte limit before the >>2 encodes it into the instruction.
    if(delta < -(1LL<<27) || delta >= (1LL<<27)){
      seterr(err, errlen, "branch out of range");
      return -1;
    }
    uint32 *w = (uint32 *)P;
    set_bitfield(w, 0x03ffffffu, 0, ((uint64)(delta >> 2)) & 0x03ffffff);
    return 0;
  }
  default:
    seterr(err, errlen, "unsupported relocation");
    return -1;
  }
}

// Make freshly written module instructions visible to the CPU.  The arena is
// cacheable normal memory, so the copy and relocation pass land in the D-cache
// while the I-cache may still hold the stale junk kalloc left behind.  Clean
// each line out, invalidate the I-cache, then context-sync, matching the
// sequence uvmsync_icache() uses before running a user image.
static void
sync_module_icache(uint64 start, uint64 end)
{
  for(uint64 a = start; a < end; a += 64){
    asm volatile("dc civac, %0" :: "r"(a));
    asm volatile("ic ivau, %0" :: "r"(a));
  }
  asm volatile("dsb sy");
  asm volatile("isb");
}

int
load_module_image(char *image, int len, char *err, int errlen)
{
  struct elfhdr *eh = (struct elfhdr *)image;
  struct spinlock *lk = &modules.lock;

  seterr(err, errlen, "ok");
  if(len < (int)sizeof(struct elfhdr) || eh->magic != ELF_MAGIC){
    seterr(err, errlen, "not an ELF file");
    return -1;
  }
  if(eh->shentsize != sizeof(struct elf64_shdr) ||
     eh->shstrndx >= eh->shnum || eh->shnum > SECT_MAX){
    seterr(err, errlen, "bad section table");
    return -1;
  }

  struct elf64_shdr *sh = (struct elf64_shdr *)(image + eh->shoff);
  char *secstr = image + sh[eh->shstrndx].offset;

  // Locate the symbol table and the string table it references.
  struct elf64_shdr *symtab = 0;
  char *strtab = 0;
  for(int i = 0; i < eh->shnum; i++){
    if(sh[i].type == SHT_SYMTAB){
      symtab = &sh[i];
      strtab = image + sh[symtab->link].offset;
      break;
    }
  }
  if(symtab == 0){
    seterr(err, errlen, "no symbol table");
    return -1;
  }
  struct elf64_sym *syms = (struct elf64_sym *)(image + symtab->offset);

  // Reserve arena memory for every section the module will actually occupy,
  // then copy its bytes in.  Recording a per-section base lets a relocation
  // that names a section symbol resolve to base + addend with no further work.
  acquire(lk);
  uint64 start_top = modules.top;
  uint64 sec_base[SECT_MAX];
  for(int i = 0; i < SECT_MAX; i++)
    sec_base[i] = 0;

  int initcall_sec = -1;
  for(int i = 0; i < eh->shnum; i++){
    if((sh[i].flags & SHF_ALLOC) == 0 || sh[i].size == 0)
      continue;
    uint64 align = sh[i].addralign ? sh[i].addralign : 1;
    uint64 off = (modules.top + align - 1) & ~(align - 1);
    if(off + sh[i].size > MODAREA_SIZE){
      seterr(err, errlen, "out of module memory");
      goto fail_lifo;
    }
    uint64 dst = MODAREA + off;
    if(sh[i].type == SHT_NOBITS){
      memset((void *)dst, 0, sh[i].size);         // .bss has no file bytes
    } else {
      if(sh[i].offset + sh[i].size > (uint64)len){
        seterr(err, errlen, "section past end of image");
        goto fail_lifo;
      }
      memmove((void *)dst, image + sh[i].offset, sh[i].size);
    }
    sec_base[i] = dst;
    modules.top = off + sh[i].size;
    if(initcall_sec < 0 && str_eq(secstr + sh[i].name, ".initcall.list"))
      initcall_sec = i;
  }

  if(initcall_sec < 0){
    seterr(err, errlen, "no .initcall.list");
    goto fail_lifo;
  }

  // Apply every relocation whose target is a section we loaded.  RELA entries
  // living in .rela.debug_* point at non-alloc sections and are skipped.
  for(int i = 0; i < eh->shnum; i++){
    if(sh[i].type != SHT_RELA)
      continue;
    int tgt = sh[i].info;
    if(tgt >= SECT_MAX || sec_base[tgt] == 0)
      continue;
    struct elf64_rela *rela = (struct elf64_rela *)(image + sh[i].offset);
    int n = sh[i].size / sizeof(struct elf64_rela);
    for(int k = 0; k < n; k++){
      uint32 symidx = ELF64_R_SYM(rela[k].info);
      uint64 S;
      if(symidx >= symtab->size / sizeof(struct elf64_sym)){
        seterr(err, errlen, "reloc past symbol table");
        goto fail_lifo;
      }
      if(sym_value(&syms[symidx], strtab, sec_base, (long)rela[k].addend,
                   &S, err, errlen) < 0)
        goto fail_lifo;
      uint64 P = sec_base[tgt] + rela[k].offset;
      if(apply_rela(ELF64_R_TYPE(rela[k].info), P, S, err, errlen) < 0)
        goto fail_lifo;
    }
  }

  sync_module_icache(MODAREA + start_top, MODAREA + modules.top);

  // The module's own, now-relocated initcall entries are its contract with us.
  struct module_desc *descs = (struct module_desc *)sec_base[initcall_sec];
  int ndesc = sh[initcall_sec].size / sizeof(struct module_desc);

  // Find a free slot; a .ko is identified by the name its descriptor carries.
  int slot = -1;
  for(int i = 0; i < DYN_MAX; i++)
    if(!loaded[i].used){ slot = i; break; }
  if(slot < 0){
    seterr(err, errlen, "module table full");
    goto fail_lifo;
  }
  if(ndesc < 1 || str_eq(descs[0].name, "")){
    seterr(err, errlen, "module has no initcall");
    goto fail_lifo;
  }
  safestrcpy(loaded[slot].name, descs[0].name, DYN_NAME);
  loaded[slot].base = MODAREA + start_top;
  loaded[slot].size = modules.top - start_top;
  loaded[slot].exit = 0;
  loaded[slot].used = 1;
  modules.nloaded++;

  // Record the cleanup the module registered, then publish the slot so a
  // concurrent /proc/modules listing sees the module as live.
  for(int i = 0; i < ndesc; i++)
    if(loaded[slot].exit == 0)
      loaded[slot].exit = descs[i].exit;

  // Release the arena lock BEFORE running init.  modules.lock is a spinlock, so
  // interrupts are off while it is held; a loadable driver's init registers
  // itself on a bus and, if its device is already present, probes it.  The DWC2
  // transfer completion a probe waits on is interrupt-driven, so it must not be
  // asked to finish with IRQs masked -- and Linux likewise runs
  // do_one_initcall()/driver probe in process context, not under a spinlock.
  // Everything the lock actually guarded (the arena bump, the slot, the
  // relocation pass) is complete by now.
  release(lk);

  for(int i = 0; i < ndesc; i++)
    if(descs[i].init)
      descs[i].init();

  printf("module: loaded %s (%d bytes)\n", loaded[slot].name,
         (int)loaded[slot].size);
  return 0;

 fail_lifo:
  modules.top = start_top;      // the arena bump was never consumed by anyone
  release(lk);
  return -1;
}

int
unload_module(const char *name, char *err, int errlen)
{
  struct spinlock *lk = &modules.lock;
  acquire(lk);
  int slot = -1;
  for(int i = 0; i < DYN_MAX; i++)
    if(loaded[i].used && str_eq(loaded[i].name, name)){ slot = i; break; }
  if(slot < 0){
    seterr(err, errlen, "module not loaded");
    release(lk);
    return -1;
  }
  // The arena is a stack: only the most recently loaded module sits on top, so
  // only it can be popped without leaving a gap.  rmmod in module load order
  // always satisfies this; unloading a lower module is refused rather than
  // silently fragmenting the arena.
  if(loaded[slot].base + loaded[slot].size != MODAREA + modules.top){
    seterr(err, errlen, "in use by a newer module");
    release(lk);
    return -1;
  }
  if(loaded[slot].exit)
    loaded[slot].exit();
  modules.top = loaded[slot].base - MODAREA;
  loaded[slot].used = 0;
  loaded[slot].exit = 0;
  modules.nloaded--;
  printf("module: unloaded %s\n", loaded[slot].name);
  release(lk);
  return 0;
}

// ---------------------------------------------------------------------------
// /proc/modules formatting helpers.
// ---------------------------------------------------------------------------

static char *
putstr(char *p, char *end, char *s)
{
  while(*s && p < end)
    *p++ = *s++;
  return p;
}

// Left-aligned column, the way lsmod pads its fields.
static char *
putfield(char *p, char *end, char *s, int width)
{
  char *start = p;
  p = putstr(p, end, s);
  while(p - start < width && p < end)
    *p++ = ' ';
  return p;
}

static char *
putnum(char *p, char *end, uint64 value, int hex)
{
  char tmp[24], *digits = "0123456789abcdef";
  int base = hex ? 16 : 10, n = 0;
  if(hex)
    p = putstr(p, end, "0x");
  do {
    tmp[n++] = digits[value % base];
    value /= base;
  } while(value && n < sizeof(tmp));
  while(n && p < end)
    *p++ = tmp[--n];
  return p;
}

// The lsmod view: built-in entries in the order do_initcalls() used, then the
// loadable modules that were relocated into the arena at runtime.
int
module_format(char *buf, int max)
{
  char *p = buf, *end = buf + max;
  p = putstr(p, end, "Module             Lvl Ord Init              State\n");
  for(int i = 0; i < module_core.nmod; i++){
    const struct module_desc *m = module_core.mods[i];
    p = putfield(p, end, m->name, 19);
    p = putnum(p, end, m->level, 0);
    p = putstr(p, end, "   ");
    p = putnum(p, end, m->order, 0);
    p = putstr(p, end, "   ");
    p = putnum(p, end, (uint64)m->init, 1);
    p = putstr(p, end, "   ");
    p = putstr(p, end, module_core.done[i] ? "init" : "pending");
    p = putstr(p, end, "\n");
  }

  acquire(&modules.lock);
  for(int i = 0; i < DYN_MAX; i++){
    if(!loaded[i].used)
      continue;
    p = putfield(p, end, loaded[i].name, 19);
    p = putstr(p, end, "-    -    ");
    p = putnum(p, end, loaded[i].base, 1);
    p = putstr(p, end, "   ");
    p = putstr(p, end, "live");
    p = putstr(p, end, "\n");
  }
  int nl = modules.nloaded;
  uint64 used = modules.top;
  release(&modules.lock);

  p = putstr(p, end, "\n");
  p = putstr(p, end, "initcalls run: ");
  p = putnum(p, end, module_core.nrun, 0);
  p = putstr(p, end, " of ");
  p = putnum(p, end, module_core.nmod, 0);
  p = putstr(p, end, " (levels 0..");
  p = putnum(p, end, INIT_LEVEL_COUNT - 1, 0);
  p = putstr(p, end, ")\n");
  p = putstr(p, end, "loadable modules live: ");
  p = putnum(p, end, nl, 0);
  p = putstr(p, end, ", arena used: ");
  p = putnum(p, end, used, 0);
  p = putstr(p, end, " of ");
  p = putnum(p, end, MODAREA_SIZE, 0);
  p = putstr(p, end, " bytes\n");
  return p - buf;
}

// ---------------------------------------------------------------------------
// request_module: auto-loading a driver off /lib/modules when a device that no
// loaded driver claims appears.  This is the Linux cold/hot-plug chain
// (drivers/base/uevent.c -> kmod -> /sbin/modprobe) collapsed into the kernel,
// because xv6 has no netlink uevent socket and no udev/mdev in user space.
//
// Linux does not resolve a device to a module by probing every .ko; it uses an
// *index*.  depmod reads each driver's MODULE_DEVICE_TABLE at install time and
// writes /lib/modules/modules.alias, mapping a device MODALIAS string to the
// .ko that handles it.  On add the kernel fires a uevent carrying MODALIAS=...,
// udev runs modprobe on that alias, and modprobe opens the mapped .ko.  Here
// request_module() is the index plus the modprobe step inlined: the table below
// stands in for modules.alias, and once a device's alias matches an entry the
// kmod workqueue reads that .ko off the root filesystem and relocates it with
// load_module_image().  When the module's init runs usb_register_driver(),
// driver_register()'s bind pass re-scans the registered devices and binds the
// very device that triggered the request -- no insmod typed by hand.
//
// Only out-of-tree (.ko) drivers belong in the table.  A =y driver is already
// registered before its device enumerates, so bind() matches it directly and
// request_module() is never consulted.
// ---------------------------------------------------------------------------

#define MODULE_DIR "/lib/modules/"

// One line of modules.alias each: the MODALIAS a device carries, and the .ko
// under /lib/modules that claims it.  Add an entry when a driver moves =y -> =m.
static const struct {
  char *modalias;
  char *kofile;
} module_aliases[] = {
  { "usb:c03s01p02", "usbmouse.ko" },  // HID boot mouse: class 3, sub 1, proto 2
  { 0, 0 },
};

// The image a request_module loads, read in one piece so the relocation pass can
// index sections by file offset.  One autoload runs at a time on the kmod
// kworker, so a single static buffer stands in for Linux's per-module copy.
static char ko_image[MODULE_IMG_MAX];

#define KMOD_SLOTS 4
#define KO_NAME    32
struct kmod_slot {
  char kofile[KO_NAME];
  int inuse;
  struct work_struct work;
};
static struct kmod_slot kmod_slots[KMOD_SLOTS];
static struct workqueue kmod_wq;
static struct spinlock kmod_lock;

// Bring up the kmod workqueue during do_initcalls(), before any driver that
// could enumerate a device runs.  The kmod kworker only starts once the
// scheduler is running, which is exactly right: an autoload queued during boot
// enumeration executes after the root filesystem is available.
static void
kmod_init(void)
{
  initlock(&kmod_lock, "kmod");
  init_workqueue(&kmod_wq, "kmod", 1);
  for(int i = 0; i < KMOD_SLOTS; i++){
    kmod_slots[i].inuse = 0;
    init_work(&kmod_slots[i].work, kmod_run);
  }
}

// Read /lib/modules/<kofile> into ko_image and hand it to the loader.  Runs on
// the kmod kworker, so the begin_op()/namei()/bread() sleeps in the read path
// are legal here -- which is the whole reason the load is deferred to a
// workqueue instead of done inline in device_register().
static void
kmod_load(char *kofile)
{
  char path[MAXPATH];
  char err[64];
  char *dir = MODULE_DIR;
  int i = 0, n, got, ok;

  while(*dir)
    path[i++] = *dir++;
  for(n = 0; kofile[n] && i < MAXPATH - 1; n++)
    path[i++] = kofile[n];
  path[i] = 0;

  begin_op();
  struct inode *ip = namei(path);
  if(ip == 0){
    end_op();
    printf("request_module: %s not found (root mounted?)\n", path);
    return;
  }
  ilock(ip);
  uint sz = ip->size;
  ok = sz > 0 && sz <= MODULE_IMG_MAX;
  // Read through the inode's filesystem ops, not the classic xv6fs readi():
  // the Pi's root is ext2, and readi()'s bmap()->getsb() would panic because
  // the ext2 device has no xv6 superblock.  This is the same dispatch fileread
  // uses, so a .ko under /lib/modules loads on either root filesystem.
  got = ok ? ip->iop->read(ip, 0, (uint64)ko_image, 0, sz) : -1;
  iunlockput(ip);
  end_op();

  if(!ok || got != (int)sz){
    printf("request_module: cannot read %s\n", path);
    return;
  }
  printf("request_module: loading %s\n", path);
  if(load_module_image(ko_image, sz, err, sizeof(err)) < 0)
    printf("request_module: %s: %s\n", path, err);
}

// A queued autoload: copy the .ko name out, free the slot, then do the
// (potentially sleeping) read and load in the kworker's own process context.
static void
kmod_run(struct work_struct *work)
{
  struct kmod_slot *s =
    (struct kmod_slot *)((char *)work -
      __builtin_offsetof(struct kmod_slot, work));
  char kofile[KO_NAME];

  acquire(&kmod_lock);
  safestrcpy(kofile, s->kofile, sizeof(kofile));
  s->inuse = 0;
  release(&kmod_lock);

  kmod_load(kofile);
}

// The public entry point.  Resolve a device's MODALIAS against the alias table;
// if a loadable .ko claims it, schedule that .ko to be read and relocated.
// Returns 0 when an autoload is queued, -1 when no module answers the alias (so
// the caller knows the device genuinely has no driver here, as when a mouse is
// plugged in before the root filesystem is mounted).
int
request_module(const char *modalias)
{
  char *kofile = 0;
  struct kmod_slot *s = 0;

  for(int i = 0; module_aliases[i].modalias; i++)
    if(str_eq(module_aliases[i].modalias, modalias)){
      kofile = module_aliases[i].kofile;
      break;
    }
  if(kofile == 0)
    return -1;

  // Claim a slot and record the request under kmod_lock.  queue_work() takes
  // the workqueue's own lock, so it runs after kmod_lock is dropped.
  acquire(&kmod_lock);
  for(int i = 0; i < KMOD_SLOTS; i++)
    if(!kmod_slots[i].inuse){ s = &kmod_slots[i]; break; }
  if(s == 0){
    release(&kmod_lock);
    return -1;                 // all slots busy; retried on the next event
  }
  s->inuse = 1;
  safestrcpy(s->kofile, kofile, sizeof(s->kofile));
  release(&kmod_lock);

  queue_work(&kmod_wq, &s->work);
  return 0;
}
