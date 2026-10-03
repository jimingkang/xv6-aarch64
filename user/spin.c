// spin: busy-loop for a number of seconds (scheduler testing).
//
//   spin [SECONDS]      default 5.  Prints its CPU, the CPU time it got and
//                       when (clock_us, ms) its first and last instruction ran.
#include "kernel/types.h"
#include "user/user.h"
#include "user/schedutil.h"

int
main(int argc, char *argv[])
{
  int secs = 5;
  struct sched_info si;

  if(argc > 2 || (argc == 2 && !parse_num(argv[1], &secs))){
    fprintf(2, "usage: spin [SECONDS]\n");
    exit(1);
  }
  uint64 start = clock_us(), end = start + (uint64)secs * 1000000;
  while(clock_us() < end)
    ;
  sched_getinfo(0, &si);
  uint64 done = clock_us();
  printf("spin pid %d: %s prio %d on CPU%d, ran %d ms, wall %d..%d ms\n",
         si.pid, policy_name(si.policy), si.priority, getcpu(),
         (int)(si.run_ticks * SCHED_TICK_MS), (int)(start / 1000 % 100000),
         (int)(done / 1000 % 100000));
  exit(0);
}
