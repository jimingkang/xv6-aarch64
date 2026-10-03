// taskset: show or change a process's CPU affinity mask.
//
//   taskset -p PID              show the mask of PID
//   taskset -p MASK PID         change it (MASK like 0x8 = CPU3 only)
//   taskset MASK CMD [ARG...]   run CMD restricted to MASK
#include "kernel/types.h"
#include "user/user.h"
#include "user/schedutil.h"

static void
usage(void)
{
  fprintf(2, "usage: taskset -p PID\n"
             "       taskset -p MASK PID\n"
             "       taskset MASK CMD [ARG...]\n");
  exit(1);
}

static void
show(int pid, const char *what)
{
  struct sched_info si;
  if(sched_getinfo(pid, &si) < 0){
    fprintf(2, "taskset: no process %d\n", pid);
    exit(1);
  }
  printf("pid %d's %s affinity mask: %x\n", si.pid, what, si.cpumask);
}

int
main(int argc, char *argv[])
{
  int mask, pid;

  if(argc == 3 && strcmp(argv[1], "-p") == 0){
    if(!parse_num(argv[2], &pid))
      usage();
    show(pid, "current");
    exit(0);
  }
  if(argc == 4 && strcmp(argv[1], "-p") == 0){
    if(!parse_num(argv[2], &mask) || !parse_num(argv[3], &pid))
      usage();
    show(pid, "current");
    if(sched_setaffinity(pid, mask) < 0){
      fprintf(2, "taskset: failed to set pid %d's affinity to %x\n", pid, mask);
      exit(1);
    }
    show(pid, "new");
    exit(0);
  }
  if(argc < 3 || !parse_num(argv[1], &mask))
    usage();
  if(sched_setaffinity(0, mask) < 0){
    fprintf(2, "taskset: invalid mask %x\n", mask);
    exit(1);
  }
  exec_cmd(&argv[2]);
  fprintf(2, "taskset: exec %s failed\n", argv[2]);
  exit(1);
}
