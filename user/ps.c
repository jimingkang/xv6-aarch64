#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  static char buf[4096];
  int n = ps(buf, sizeof(buf));
  if(n < 0 || write(1, buf, n) != n){
    fprintf(2, "ps: cannot read process table\n");
    exit(1);
  }
  exit(0);
}
