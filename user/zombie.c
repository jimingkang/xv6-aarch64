// Create a zombie process that
// must be reparented at exit.

#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"

int
main(void)
{
  if(fork() > 0)
    sleep_ticks(5);  // Let child exit before parent.
  exit(0);
}
