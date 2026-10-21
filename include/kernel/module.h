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
  void (*exit)(void);  // built-in drivers have none; a .ko must undo its init
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

// A loadable (.ko) module also declares how to undo itself, because rmmod has
// something to call once the driver is no longer built in.  Linux pairs
// module_init() with module_exit() in the same module; here the two are folded
// into one descriptor so the loader keeps init and exit together.  Built-in
// drivers leave exit NULL: they are never unloaded.
#define __module_desc_exit(str, fn, xf, lvl, ord)                     \
  static const struct module_desc __module_##fn                       \
    __attribute__((used, section(".initcall.list"), aligned(8))) =     \
      { str, (lvl), (ord), fn, xf }

#define module_init_exit(str, fn, xf)                                 \
  __module_desc_exit(str, fn, xf, INIT_LEVEL_DEVICE, 0)
#define module_init_exit_ord(str, fn, xf, ord)                        \
  __module_desc_exit(str, fn, xf, INIT_LEVEL_DEVICE, ord)

// Runtime (dynamic) module interface, implemented in module.c and reached from
// the init_module()/delete_module() system calls.  A .ko is handed over as the
// raw ELF bytes a user process read off /lib/modules.
//
// A loadable module is not allowed to grow without bound: it must land in one
// contiguous kernel buffer because the relocation pass indexes symtab/strtab
// and rela sections by file offset.  Both the user (insmod) and the kernel
// (request_module autoload) paths share this ceiling.
#define MODULE_IMG_MAX (64 * 1024)

// The MODALIAS a bus generates for a device, and that request_module() resolves
// against the modules.alias table.  "usb:c03s01p02" is 13 bytes; 32 is roomy.
#define MODALIAS_MAX 32

void do_initcalls(void);
int  module_format(char *buf, int max);
int  load_module_image(char *image, int len, char *err, int errlen);
int  unload_module(const char *name, char *err, int errlen);

// Linux's kmod entry point: when a device is enumerated that no loaded driver
// claims, the driver core asks the kernel to bring in the module whose
// modules.alias entry matches the device's MODALIAS.  The load itself runs
// asynchronously on the kmod workqueue (call_usermodehelper here, a kworker
// there), because reading the .ko off /lib/modules must sleep.
int  request_module(const char *modalias);

// Provided by export.c: resolve an undefined module symbol against the set of
// kernel routines modules are allowed to bind (the __ksymtab analogue).
void *find_kernel_symbol(const char *name);

#endif
