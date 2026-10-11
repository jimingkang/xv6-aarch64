// rmmod NAME -- unload a loadable module.
//
// Asks the kernel to run the module's exit routine and reclaim its arena space
// through delete_module(); this is Linux's rmmod / delete_module path.  The
// name is the one /proc/modules reports, which a driver chooses in its
// module_init() descriptor and is not necessarily the .ko file name.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc != 2){
    fprintf(2, "usage: rmmod NAME        (NAME is the column in /proc/modules)\n");
    exit(1);
  }

  if(delete_module(argv[1]) < 0){
    fprintf(2, "rmmod: cannot unload %s\n", argv[1]);
    exit(1);
  }

  printf("rmmod: unloaded %s\n", argv[1]);
  exit(0);
}
