#ifndef XV6_MODULE_H
#define XV6_MODULE_H

// Built-in driver modules, collected by the linker (the same interface as
// rpi3-lock's include/kernel/module.h, used here by the ARM32 ports).
//
// A driver does not need a hand-written call in the board's main(): it drops
// one read-only struct module_desc into the .initcall.list section with
// module_init() (or a level macro for frameworks that must come earlier), and
// do_initcalls() runs the whole set once the core kernel is up.  Entries that
// share a level carry an explicit order and run in ascending (level, order),
// so the boot sequence does not depend on link order.

struct module_desc {
  char *name;          // shown in the boot log
  int level;           // enum init_level: coarse boot phase
  int order;           // tie-break among modules of the same level
  void (*init)(void);  // registers the driver, whose probe touches hardware
};

// Level numbers follow Linux's initcall sections.
enum init_level {
  INIT_LEVEL_CORE = 1,     // core_initcall(): clock controllers
  INIT_LEVEL_ARCH = 3,     // arch_initcall(): interrupt controllers
  INIT_LEVEL_SUBSYS = 4,   // subsys_initcall(): clocksources, frameworks
  INIT_LEVEL_FS = 5,
  INIT_LEVEL_DEVICE = 6,   // module_init(): ordinary device drivers
  INIT_LEVEL_LATE = 7,
  INIT_LEVEL_COUNT = 8
};

#define __module_desc(str, fn, lvl, ord)                                  \
  static const struct module_desc __module_##fn                           \
    __attribute__((used, section(".initcall.list"), aligned(4))) =         \
      { str, (lvl), (ord), fn }

#define core_initcall(str, fn)     __module_desc(str, fn, INIT_LEVEL_CORE, 0)
#define arch_initcall(str, fn)     __module_desc(str, fn, INIT_LEVEL_ARCH, 0)
#define subsys_initcall(str, fn)   __module_desc(str, fn, INIT_LEVEL_SUBSYS, 0)
#define fs_initcall(str, fn)       __module_desc(str, fn, INIT_LEVEL_FS, 0)
#define module_init(str, fn)       __module_desc(str, fn, INIT_LEVEL_DEVICE, 0)
#define late_initcall(str, fn)     __module_desc(str, fn, INIT_LEVEL_LATE, 0)
#define module_init_ord(str, fn, ord)  __module_desc(str, fn, INIT_LEVEL_DEVICE, ord)

void do_initcalls(void);

#endif
