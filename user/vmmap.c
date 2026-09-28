#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  int pid = 0;
  if(argc > 2){
    fprintf(2, "usage: vmmap [pid]\n");
    exit(1);
  }
  if(argc == 2){
    pid = atoi(argv[1]);
    if(pid <= 0){
      fprintf(2, "vmmap: invalid pid\n");
      exit(1);
    }
  }
  if(vmdump(pid) < 0){
    fprintf(2, "vmmap: process not found\n");
    exit(1);
  }
  exit(0);
}
