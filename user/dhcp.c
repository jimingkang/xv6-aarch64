#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  if(argc != 2){
    printf("usage: dhcp interface\n");
    exit(1);
  }
  if(net_dhcp(argv[1]) < 0){
    printf("dhcp: failed on %s\n", argv[1]);
    exit(1);
  }
  exit(0);
}
