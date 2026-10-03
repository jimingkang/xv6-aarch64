// rtlat: measure wakeup latency of a task that shares a CPU with busy loops.
//
//   rtlat [-p fifo|rr|other] [-P PRIO] [-c CPU] [-l HOGS] [-n SAMPLES]
//
// Defaults: SCHED_FIFO priority 80 on CPU3, 3 busy SCHED_OTHER loops pinned
// to the same CPU, 30 samples.  A waker on another CPU sends clock_us()
// through a pipe every 100 ms; the reader reports how long it took to run
// after the write.  Compare `rtlat -p fifo` with `rtlat -p other`.
#include "kernel/types.h"
#include "kernel/param.h"
#include "user/user.h"
#include "user/schedutil.h"

static void
usage(void)
{
  fprintf(2, "usage: rtlat [-p fifo|rr|other] [-P PRIO] [-c CPU] [-l HOGS] "
             "[-n SAMPLES]\n");
  exit(1);
}

static void
print_us(char *label, uint64 us)
{
  printf("  %s %l.%l%l%l ms\n", label, us / 1000, (us / 100) % 10,
         (us / 10) % 10, us % 10);
}

int
main(int argc, char *argv[])
{
  int policy = SCHED_FIFO, prio = 80, cpu = 3, hogs = 3, samples = 30;
  int fds[2], hog_pid[8], reader;

  for(int i = 1; i < argc; i++){
    if(i + 1 >= argc)
      usage();
    char *opt = argv[i], *val = argv[++i];
    if(strcmp(opt, "-p") == 0){
      if(strcmp(val, "fifo") == 0) policy = SCHED_FIFO;
      else if(strcmp(val, "rr") == 0) policy = SCHED_RR;
      else if(strcmp(val, "other") == 0) policy = SCHED_OTHER;
      else usage();
    } else if(strcmp(opt, "-P") == 0){
      if(!parse_num(val, &prio)) usage();
    } else if(strcmp(opt, "-c") == 0){
      if(!parse_num(val, &cpu) || cpu < 0 || cpu >= NCPU) usage();
    } else if(strcmp(opt, "-l") == 0){
      if(!parse_num(val, &hogs) || hogs < 0 || hogs > 8) usage();
    } else if(strcmp(opt, "-n") == 0){
      if(!parse_num(val, &samples) || samples < 1) usage();
    } else {
      usage();
    }
  }
  if(policy == SCHED_OTHER)
    prio = 0;

  printf("rtlat: reader %s prio %d on CPU%d, %d busy loop(s) on CPU%d, "
         "%d samples\n", policy_name(policy), prio, cpu, hogs, cpu, samples);

  if(pipe(fds) < 0){
    fprintf(2, "rtlat: pipe failed\n");
    exit(1);
  }

  // CPU hogs: ordinary time-sharing tasks that never block.
  for(int h = 0; h < hogs; h++){
    if((hog_pid[h] = fork()) == 0){
      close(fds[0]);
      close(fds[1]);
      sched_setaffinity(0, 1U << cpu);
      sched_setscheduler(0, SCHED_OTHER, 0);
      for(volatile uint64 n = 0; ; n++)
        ;
    }
  }

  if((reader = fork()) == 0){
    uint64 ts, lat, min = ~0ULL, max = 0, sum = 0;
    int over1ms = 0, wrong_cpu = 0;

    close(fds[1]);
    sched_setaffinity(0, 1U << cpu);
    if(sched_setscheduler(0, policy, prio) < 0){
      fprintf(2, "rtlat: cannot set reader policy\n");
      exit(1);
    }
    for(int s = 0; s < samples; s++){
      if(read(fds[0], &ts, sizeof(ts)) != sizeof(ts))
        break;
      lat = clock_us() - ts;
      if(getcpu() != cpu)
        wrong_cpu++;
      if(lat < min) min = lat;
      if(lat > max) max = lat;
      if(lat > 1000) over1ms++;
      sum += lat;
    }
    printf("rtlat: %s wakeup latency over %d samples\n",
           policy_name(policy), samples);
    print_us("min", min);
    print_us("avg", sum / samples);
    print_us("max", max);
    printf("  samples > 1 ms: %d, ran on a CPU other than CPU%d: %d\n",
           over1ms, cpu, wrong_cpu);
    exit(0);
  }

  // Waker: never on the measured CPU.
  close(fds[0]);
  sched_setaffinity(0, ((1U << NCPU) - 1) & ~(1U << cpu));
  sleep(2);                         // let the hogs and reader settle
  for(int s = 0; s < samples; s++){
    sleep(1);                       // one xv6 tick, 100 ms
    uint64 now = clock_us();
    write(fds[1], &now, sizeof(now));
  }
  close(fds[1]);
  wait(0);                          // reader
  for(int h = 0; h < hogs; h++){
    kill(hog_pid[h]);
    wait(0);
  }
  exit(0);
}
