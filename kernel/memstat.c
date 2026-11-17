// /proc/meminfo and /proc/iomem, in the format of Linux's fs/proc/meminfo.c
// and kernel/resource.c.
//
// /proc/iomem is a resource tree built at read time from
//   - the board's RAM banks and reserved carve-outs (arch_iomem_regions(),
//     weak default: one System RAM range EXTMEM..PHYSTOP),
//   - the kernel image sections (linker symbols),
//   - the page allocator's range (kalloc.c),
//   - the MEM resources of every registered platform device (device.c),
// sorted by start address and nested by containment, two spaces per level.
//
// /proc/meminfo reports the page allocator (MemTotal is what kalloc
// manages, as Linux's MemTotal excludes the kernel image and reserved
// memory), plus where the other kernel memory goes: buffer cache, kernel
// stacks, page tables, user pages.
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "aarch64.h"
#include "proc.h"
#include "device.h"
#include "memstat.h"
#include "defs.h"

extern char etext[], end[];
// Linker symbols of kernel.ld; ramdisk_* exist only in builds that embed
// the root file system (QEMU stand-in).
extern char data_end[] __attribute__((weak));
extern char bss_start[] __attribute__((weak));
extern char ramdisk_start[] __attribute__((weak));
extern char ramdisk_end[] __attribute__((weak));

__attribute__((weak)) int
arch_iomem_regions(struct iomem_region *r, int max)
{
  if(max < 1)
    return 0;
  r[0].start = EXTMEM;
  r[0].end = PHYSTOP - 1;
  r[0].name = "System RAM";
  return 1;
}

// ---- small formatting helpers ----

struct out {
  char *p, *end;
};

static void
put(struct out *o, const char *s)
{
  while(*s && o->p < o->end)
    *o->p++ = *s++;
}

static void
puthexw(struct out *o, uint64 v, int width)
{
  static const char digits[] = "0123456789abcdef";
  char tmp[16];
  int n = 0;
  do {
    tmp[n++] = digits[v & 15];
    v >>= 4;
  } while(v && n < (int)sizeof(tmp));
  while(n < width)
    tmp[n++] = '0';
  while(n && o->p < o->end)
    *o->p++ = tmp[--n];
}

// Right-aligned decimal in a field of `width` characters.
static void
putdecw(struct out *o, uint64 v, int width)
{
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  } while(v && n < (int)sizeof(tmp));
  for(int pad = width - n; pad > 0 && o->p < o->end; pad--)
    *o->p++ = ' ';
  while(n && o->p < o->end)
    *o->p++ = tmp[--n];
}

// "Name:           12345 kB\n", as Linux's show_val_kb().
static void
putkb(struct out *o, const char *name, uint64 kb)
{
  int len = strlen(name);
  put(o, name);
  put(o, ":");
  for(int pad = 15 - len; pad > 0 && o->p < o->end; pad--)
    *o->p++ = ' ';
  putdecw(o, kb, 8);
  put(o, " kB\n");
}

// ---- /proc/meminfo ----

int
meminfo_format(char *buf, int max)
{
  struct out o = { buf, buf + max };
  uint64 managed, nfree, lo, hi;
  uint nroots, nl2;
  uint64 kstacks = 0, userpages = 0, nproc = 0;

  kmemstat(&managed, &nfree, &lo, &hi);
  vm_pagetable_stats(&nroots, &nl2);

  // Kernel stacks (one kalloc page each) and user memory. Threads share
  // their leader's vmspace, so only non-thread processes count user pages.
  for(struct proc *p = proc_head; p; p = p->next){
    acquire(&p->lock);
    if(p->state != UNUSED){
      nproc++;
      if(p->kstack_pa)
        kstacks++;
      if(!p->is_thread && p->vm)
        userpages += PGROUNDUP(p->vm->sz) / PGSIZE;
    }
    release(&p->lock);
  }

  uint64 ktext = (uint64)(etext - (char*)KERNLINK);
  uint64 kdata = (uint64)((bss_start ? bss_start : end) - etext);
  uint64 kbss = bss_start ? (uint64)(end - bss_start) : 0;
  uint64 ramdisk = (ramdisk_start && ramdisk_end) ?
                   (uint64)(ramdisk_end - ramdisk_start) : 0;

  putkb(&o, "MemTotal", managed * PGSIZE / 1024);
  putkb(&o, "MemFree", nfree * PGSIZE / 1024);
  putkb(&o, "MemAvailable", nfree * PGSIZE / 1024);
  putkb(&o, "Buffers", (uint64)NBUF * BSIZE / 1024);
  putkb(&o, "Cached", 0);
  putkb(&o, "SwapTotal", 0);
  putkb(&o, "SwapFree", 0);
  putkb(&o, "AnonPages", userpages * PGSIZE / 1024);
  putkb(&o, "KernelStack", kstacks * PGSIZE / 1024);
  // L1 roots are 16 KiB each from a static pool; every L2 table takes a
  // whole kalloc page although only 1 KiB of it is used.
  putkb(&o, "PageTables", (uint64)nroots * 16 + (uint64)nl2 * PGSIZE / 1024);
  // xv6-specific lines (Linux has no exact equivalents).
  putkb(&o, "KernelText", ktext / 1024);
  putkb(&o, "KernelData", (kdata - ramdisk) / 1024);
  putkb(&o, "KernelBss", kbss / 1024);
  if(ramdisk)
    putkb(&o, "RamdiskRoot", ramdisk / 1024);
  put(&o, "KallocRange:    ");
  puthexw(&o, lo, 8);
  put(&o, "-");
  puthexw(&o, hi ? hi - 1 : 0, 8);
  put(&o, "\nProcesses:      ");
  putdecw(&o, nproc, 8);
  put(&o, "\n");
  return o.p - buf;
}

// ---- /proc/iomem ----

struct ent {
  uint64 start, end;
  char name[48];
};
#define IOMEM_MAX ((int)(PGSIZE / sizeof(struct ent)))

static void
add(struct ent *e, int *n, uint64 start, uint64 end, const char *a, const char *b)
{
  if(*n >= IOMEM_MAX || end < start)
    return;
  e[*n].start = start;
  e[*n].end = end;
  safestrcpy(e[*n].name, (char*)a, sizeof(e[*n].name));
  if(b && *b){
    int len = strlen(e[*n].name);
    if(len + 2 < (int)sizeof(e[*n].name)){
      e[*n].name[len] = ' ';
      safestrcpy(e[*n].name + len + 1, (char*)b, sizeof(e[*n].name) - len - 1);
    }
  }
  (*n)++;
}

int
iomem_format(char *buf, int max)
{
  struct out o = { buf, buf + max };
  // The tables are too large for a kernel stack: one page each.
  struct ent *e = kalloc();
  struct iomem_res *res = kalloc();
  int resmax = PGSIZE / sizeof(struct iomem_res);
  int n = 0;
  if(e == 0 || res == 0){
    if(e) kfree(e);
    if(res) kfree(res);
    return -1;
  }

  struct iomem_region regions[16];
  int nr = arch_iomem_regions(regions, 16);
  for(int i = 0; i < nr; i++)
    add(e, &n, regions[i].start, regions[i].end, regions[i].name, 0);

  uint64 kbase = V2P(KERNLINK);
  add(e, &n, kbase, V2P(etext) - 1, "Kernel code", 0);
  if(ramdisk_start && ramdisk_end > ramdisk_start){
    add(e, &n, V2P(etext), V2P(data_end ? data_end : ramdisk_start) - 1, "Kernel data", 0);
    add(e, &n, V2P(ramdisk_start), V2P(ramdisk_end) - 1, "Kernel ramdisk (root fs)", 0);
  } else {
    add(e, &n, V2P(etext), V2P(bss_start ? bss_start : end) - 1, "Kernel data", 0);
  }
  if(bss_start)
    add(e, &n, V2P(bss_start), V2P(end) - 1, "Kernel bss", 0);

  uint64 managed, nfree, lo, hi;
  kmemstat(&managed, &nfree, &lo, &hi);
  if(hi > lo)
    add(e, &n, lo, hi - 1, "Page allocator (kalloc)", 0);

  int nres = device_mem_resources(res, resmax);
  for(int i = 0; i < nres; i++)
    add(e, &n, res[i].start, res[i].end, res[i].dev, res[i].res);

  // Sort: by start, then larger ranges first so parents precede children.
  for(int i = 1; i < n; i++){
    struct ent t = e[i];
    int j = i;
    while(j > 0 && (e[j-1].start > t.start ||
                    (e[j-1].start == t.start && e[j-1].end < t.end))){
      e[j] = e[j-1];
      j--;
    }
    e[j] = t;
  }

  // Nest by containment (an identical range is a sibling, not a child).
  int stack[8], depth = 0;
  for(int i = 0; i < n; i++){
    while(depth > 0){
      struct ent *p = &e[stack[depth-1]];
      int contains = p->start <= e[i].start && e[i].end <= p->end &&
                     !(p->start == e[i].start && p->end == e[i].end);
      if(contains)
        break;
      depth--;
    }
    for(int d = 0; d < depth; d++)
      put(&o, "  ");
    puthexw(&o, e[i].start, 8);
    put(&o, "-");
    puthexw(&o, e[i].end, 8);
    put(&o, " : ");
    put(&o, e[i].name);
    put(&o, "\n");
    if(depth < 8)
      stack[depth++] = i;
  }
  kfree(e);
  kfree(res);
  return o.p - buf;
}
