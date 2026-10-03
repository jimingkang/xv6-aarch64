// nice: run a command with a different nice value (SCHED_OTHER time slice).
//
//   nice [-n N] CMD [ARG...]     N in -20..19, default 10
#include "kernel/types.h"
#include "user/user.h"
#include "user/schedutil.h"

int
main(int argc, char *argv[])
{
  int n = 10, i = 1;
  struct sched_info si;

  if(argc >= 3 && strcmp(argv[1], "-n") == 0){
    if(!parse_num(argv[2], &n))
      goto usage;
    i = 3;
  }
  if(i >= argc)
    goto usage;
  if(sched_getinfo(0, &si) < 0 || setnice(0, si.nice + n) < 0){
    fprintf(2, "nice: invalid nice value %d\n", si.nice + n);
    exit(1);
  }
  exec_cmd(&argv[i]);
  fprintf(2, "nice: exec %s failed\n", argv[i]);
  exit(1);
usage:
  fprintf(2, "usage: nice [-n N] CMD [ARG...]\n");
  exit(1);
}
