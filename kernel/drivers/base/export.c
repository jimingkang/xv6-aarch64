// The kernel's exported symbol table: the .ko loader's copy of Linux's
// __ksymtab section.
//
// A relocatable module is full of `bl` instructions that point at kernel code
// the module cannot see at compile time.  Linux makes each such routine
// available by tagging it EXPORT_SYMBOL, which drops a {name, address} pair
// into a linker section the module loader can search.  Here the same table is
// written out once, in this file, so the set of symbols a module is allowed to
// bind is explicit and reviewable rather than spread across the tree: add an
// EXPORT(name) line here when a new driver moves out of the built-in image.
//
// Only symbols a loadable driver actually references belong in the table.
// That keeps the exported kernel ABI small, exactly the discipline Linux
// enforces with EXPORT_SYMBOL vs EXPORT_SYMBOL_GPL.

#include <kernel/types.h>
#include <kernel/param.h>
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/defs.h>
#include <kernel/input.h>
#include <kernel/usb.h>
#include <kernel/workqueue.h>

struct kexport {
  const char *name;
  void *addr;
};

// EXPORT takes the address of a declared kernel function, so a rename in the
// prototype fails the build instead of silently handing a module the wrong
// symbol.  The name string and the pointer stay in step by construction.
#define EXPORT(fn)  { #fn, (void *) fn }

static const struct kexport kernel_exports[] = {
  // core: printing, memory, locking, string helpers
  EXPORT(printf),
  EXPORT(kalloc),
  EXPORT(kfree),
  EXPORT(memset),
  EXPORT(acquire),
  EXPORT(release),
  EXPORT(initlock),

  // work queue used by HID drivers to defer report parsing to process context
  EXPORT(init_work),
  EXPORT(init_workqueue),
  EXPORT(queue_work),
  EXPORT(cancel_work_sync),

  // input core (registered built-in at INIT_LEVEL_SUBSYS)
  EXPORT(input_allocate_device),
  EXPORT(input_free_device),
  EXPORT(input_register_device),
  EXPORT(input_unregister_device),
  EXPORT(input_report_key),
  EXPORT(input_report_rel),
  EXPORT(input_set_capability),
  EXPORT(input_sync),

  // USB core (registered built-in): a HID driver binds a driver and owns URBs
  EXPORT(usb_register_driver),
  EXPORT(usb_unregister_driver),
  EXPORT(usb_alloc_urb),
  EXPORT(usb_free_urb),
  EXPORT(usb_fill_int_urb),
  EXPORT(usb_submit_urb),
  EXPORT(usb_kill_urb),
};

// Resolve one undefined module symbol.  Returns the kernel address, or 0 when
// the routine was never exported -- the loader then refuses the module rather
// than letting a branch land at a wild address.
void *
find_kernel_symbol(const char *name)
{
  for(int i = 0; i < (int)(sizeof(kernel_exports)/sizeof(kernel_exports[0])); i++){
    const char *a = kernel_exports[i].name, *b = name;
    while(*a && *a == *b){ a++; b++; }
    if(*a == 0 && *b == 0)
      return kernel_exports[i].addr;
  }
  return 0;
}
