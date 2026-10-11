// The built-in module table: walk what the linker collected, run it in boot
// order, and format it for /proc/modules.

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "defs.h"
#include "module.h"

// kernel.ld gathers every .initcall.list object between these two symbols.
// They are addresses, not sized arrays, so the table length is the span
// divided by the element size.
extern const struct module_desc __initcall_start[];
extern const struct module_desc __initcall_end[];

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

// The lsmod view: one line per table entry, in the order do_initcalls() used.
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
  p = putstr(p, end, "\n");
  p = putstr(p, end, "initcalls run: ");
  p = putnum(p, end, module_core.nrun, 0);
  p = putstr(p, end, " of ");
  p = putnum(p, end, module_core.nmod, 0);
  p = putstr(p, end, " (levels 0..");
  p = putnum(p, end, INIT_LEVEL_COUNT - 1, 0);
  p = putstr(p, end, ")\n");
  return p - buf;
}
