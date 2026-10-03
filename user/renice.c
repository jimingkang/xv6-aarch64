// renice: set the nice value of a running process.
//
//   renice N PID
#include "kernel/types.h"
#include "user/user.h"
#include "user/schedutil.h"

int
main(int argc, char *argv[])
{
  int n, pid;
  struct sched_info si;

  if(argc != 3 || !parse_num(argv[1], &n) || !parse_num(argv[2], &pid)){
    fprintf(2, "usage: renice N PID\n");
    exit(1);
  }
  if(sched_getinfo(pid, &si) < 0 || setnice(pid, n) < 0){
    fprintf(2, "renice: failed to set pid %d to nice %d\n", pid, n);
    exit(1);
  }
  printf("%d (process ID) old priority %d, new priority %d\n", pid, si.nice, n);
  exit(0);
}
