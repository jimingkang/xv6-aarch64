// do_initcalls(): run every module_desc the linker gathered into
// .initcall.list (see kernel/module.h), in ascending (level, order).
// Entries with the same key keep their table (link) order.
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
#include "kernel/module.h"

extern const struct module_desc __initcall_start[], __initcall_end[];
#define MAX_INITCALLS 64

static int
before(const struct module_desc *a, const struct module_desc *b)
{
  return a->level < b->level || (a->level == b->level && a->order < b->order);
}

void
do_initcalls(void)
{
  static const struct module_desc *list[MAX_INITCALLS];
  int n = 0;

  for(const struct module_desc *m = __initcall_start; m < __initcall_end; m++){
    if(n == MAX_INITCALLS)
      panic("do_initcalls: too many modules");
    int i = n++;
    while(i > 0 && before(m, list[i-1])){   // stable insertion sort
      list[i] = list[i-1];
      i--;
    }
    list[i] = m;
  }
  for(int i = 0; i < n; i++){
    printf("initcall: %s (level %d)\n", list[i]->name, list[i]->level);
    list[i]->init();
  }
}
