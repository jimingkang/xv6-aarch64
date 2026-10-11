#ifndef XV6_MODULE_H
#define XV6_MODULE_H

// Built-in driver modules, collected by the linker.
//
// A driver no longer needs a hand-written call in main().  It drops one
// read-only struct module_desc into the .initcall.list section with
// module_init() (or a level macro for the frameworks that must come earlier),
// and do_initcalls() runs the whole set once the core kernel is up.  This is
// the Linux initcall table: the section *is* the module registry, ld decides
// which entries exist, and main() stops knowing anything about drivers.
//
// Linux breaks ties inside one level by link order, which silently depends on
// the order objects appear on the link line.  Here the boot sequence must stay
// reproducible, so entries that share a level also carry an explicit order and
// run in ascending (level, order).

struct module_desc {
  char *name;          // shown by cat /proc/modules
  int level;           // enum init_level: coarse boot phase
  int order;           // tie-break among modules of the same level
  void (*init)(void);  // registers the driver, whose probe touches hardware
};

// Level numbers mirror Linux's eight initcall sections so that a driver moved
// between the two trees keeps the same phase.  This port only needs the four
// lowest phases of that range; INIT_LEVEL_COUNT stays 8 because /proc/modules
// prints the Linux number in the level column.
enum init_level {
  INIT_LEVEL_SUBSYS = 4,   // class and bus frameworks (input core)
  INIT_LEVEL_FS = 5,       // filesystem drivers, before their root is mounted
  INIT_LEVEL_DEVICE = 6,   // module_init(): ordinary device drivers
  INIT_LEVEL_LATE = 7,     // needs modules of earlier levels already bound
  INIT_LEVEL_COUNT = 8
};

#define __module_desc(str, fn, lvl, ord)                                  \
  static const struct module_desc __module_##fn                           \
    __attribute__((used, section(".initcall.list"), aligned(8))) =         \
      { str, (lvl), (ord), fn }

#define subsys_initcall(str, fn)   __module_desc(str, fn, INIT_LEVEL_SUBSYS, 0)
#define fs_initcall(str, fn)       __module_desc(str, fn, INIT_LEVEL_FS, 0)
#define module_init(str, fn)       __module_desc(str, fn, INIT_LEVEL_DEVICE, 0)
#define late_initcall(str, fn)     __module_desc(str, fn, INIT_LEVEL_LATE, 0)

// The plain macros take order 0, which is the first slot of their level.  The
// _ord variants pin a module later inside that level, which is needed whenever
// peers share it: a class driver must be registered before its host controller
// enumerates, and the root block device must exist before the image updater
// guards the partition.
#define module_init_ord(str, fn, ord)      \
  __module_desc(str, fn, INIT_LEVEL_DEVICE, ord)
#define late_initcall_ord(str, fn, ord)    \
  __module_desc(str, fn, INIT_LEVEL_LATE, ord)

void do_initcalls(void);
int  module_format(char *buf, int max);

#endif
