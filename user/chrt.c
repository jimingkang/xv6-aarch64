// chrt: show or change a process's real-time scheduling policy.
//
//   chrt -p PID                        show policy and priority of PID
//   chrt [-f|-r|-o] -p PRIO PID        change PID
//   chrt [-f|-r|-o] PRIO CMD [ARG...]  run CMD with the given policy
//
//   -f SCHED_FIFO   -r SCHED_RR (default)   -o SCHED_OTHER (PRIO must be 0)
#include "kernel/types.h"
#include "user/user.h"
#include "user/schedutil.h"

static void
usage(void)
{
  fprintf(2, "usage: chrt -p PID\n"
             "       chrt [-f|-r|-o] -p PRIO PID\n"
             "       chrt [-f|-r|-o] PRIO CMD [ARG...]\n");
  exit(1);
}

static void
show(int pid)
{
  struct sched_info si;
  if(sched_getinfo(pid, &si) < 0){
    fprintf(2, "chrt: no process %d\n", pid);
    exit(1);
  }
  printf("pid %d's current scheduling policy: %s\n", si.pid,
         policy_name(si.policy));
  printf("pid %d's current scheduling priority: %d\n", si.pid, si.priority);
}

int
main(int argc, char *argv[])
{
  int policy = SCHED_RR, pflag = 0, i = 1, prio, pid;

  for(; i < argc && argv[i][0] == '-'; i++){
    if(strcmp(argv[i], "-f") == 0) policy = SCHED_FIFO;
    else if(strcmp(argv[i], "-r") == 0) policy = SCHED_RR;
    else if(strcmp(argv[i], "-o") == 0) policy = SCHED_OTHER;
    else if(strcmp(argv[i], "-p") == 0) pflag = 1;
    else usage();
  }

  if(pflag && argc - i == 1){                 // chrt -p PID
    if(!parse_num(argv[i], &pid))
      usage();
    show(pid);
    exit(0);
  }
  if(argc - i < 2 || !parse_num(argv[i], &prio))
    usage();

  if(pflag){                                  // chrt -x -p PRIO PID
    if(!parse_num(argv[i + 1], &pid))
      usage();
    if(sched_setscheduler(pid, policy, prio) < 0){
      fprintf(2, "chrt: failed to set pid %d to %s priority %d\n",
              pid, policy_name(policy), prio);
      exit(1);
    }
    show(pid);
    exit(0);
  }

  // chrt -x PRIO CMD ...: change ourselves, then exec (attributes survive exec).
  if(sched_setscheduler(0, policy, prio) < 0){
    fprintf(2, "chrt: invalid %s priority %d\n", policy_name(policy), prio);
    exit(1);
  }
  exec_cmd(&argv[i + 1]);
  fprintf(2, "chrt: exec %s failed\n", argv[i + 1]);
  exit(1);
}
