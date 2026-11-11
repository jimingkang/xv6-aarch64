// fptest: check that every task keeps its own FP/SIMD registers.
//
//   fptest [nproc [rounds [spins]]]
//
// Each child fills q0..q31 and FPCR.RMode with a per-child pattern, spins
// long enough to be preempted (more children than CPUs), then checks every
// register.  It also checks that fork() hands the parent's live registers
// to the child, and that ordinary double arithmetic gives the same answer
// in every task.
#include <uapi/types.h>
#include "user/user.h"

#define FILL(i)                                                    \
  "add x9, %[s], #" #i "\n"                                        \
  "mvn x10, x9\n"                                                  \
  "fmov d" #i ", x9\n"                                             \
  "mov v" #i ".d[1], x10\n"
#define CHECK(i)                                                   \
  "add x9, %[s], #" #i "\n"                                        \
  "mvn x10, x9\n"                                                  \
  "fmov x11, d" #i "\n"                                            \
  "cmp x11, x9\n"                                                  \
  "cinc %[bad], %[bad], ne\n"                                      \
  "mov x11, v" #i ".d[1]\n"                                        \
  "cmp x11, x10\n"                                                 \
  "cinc %[bad], %[bad], ne\n"
#define ALL(M)                                                     \
  M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) M(10) M(11)    \
  M(12) M(13) M(14) M(15) M(16) M(17) M(18) M(19) M(20) M(21)      \
  M(22) M(23) M(24) M(25) M(26) M(27) M(28) M(29) M(30) M(31)

// Returns the number of mismatching 64-bit halves (+1 for FPCR).
static int
hold_registers(unsigned long seed, unsigned long spins)
{
  unsigned long bad, n = spins, rmode = (seed & 3) << 22;

  asm volatile(
    ALL(FILL)
    "msr fpcr, %[rm]\n"
    "1: subs %[n], %[n], #1\n"
    "b.ne 1b\n"
    "mov %[bad], #0\n"
    ALL(CHECK)
    "mrs x11, fpcr\n"
    "and x11, x11, #(3 << 22)\n"
    "cmp x11, %[rm]\n"
    "cinc %[bad], %[bad], ne\n"
    "msr fpcr, xzr\n"
    : [bad] "=&r"(bad), [n] "+r"(n)
    : [s] "r"(seed), [rm] "r"(rmode)
    : "x9", "x10", "x11", "cc", "memory",
      "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10",
      "v11", "v12", "v13", "v14", "v15", "v16", "v17", "v18", "v19", "v20",
      "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30",
      "v31");
  return (int)bad;
}

// sum 1/k^2 -> pi^2/6; returned scaled by 1e9.
static long
basel(int terms)
{
  double s = 0;
  for(int k = terms; k >= 1; k--)
    s += 1.0 / ((double)k * (double)k);
  return (long)(s * 1e9);
}

// Put a pattern in v16, call fork() with nothing in between, read v16 back
// in both parent and child.
static int
fork_keeps_registers(void)
{
  unsigned long got, want = 0x0123456789abcdefUL;
  int pid;

  asm volatile(
    "fmov d16, %[w]\n"
    "bl fork\n"
    "mov %w[pid], w0\n"
    "fmov %[got], d16\n"
    : [pid] "=r"(pid), [got] "=r"(got)
    : [w] "r"(want)
    : "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10",
      "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18", "x30",
      "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18",
      "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28",
      "v29", "v30", "v31", "cc", "memory");
  if(pid < 0){
    printf("fptest: fork failed\n");
    return 1;
  }
  if(pid == 0)
    exit(got == want ? 0 : 1);
  int status = -1;
  wait(&status);
  if(got != want || status != 0){
    printf("fptest: fork: parent %s, child %s\n",
           got == want ? "ok" : "BAD", status == 0 ? "ok" : "BAD");
    return 1;
  }
  return 0;
}

int
main(int argc, char *argv[])
{
  int nproc = argc > 1 ? atoi(argv[1]) : 8;
  int rounds = argc > 2 ? atoi(argv[2]) : 5;
  unsigned long spins = argc > 3 ? atoi(argv[3]) : 20000000;
  long ref = basel(200000);
  int failures = 0, status;

  failures += fork_keeps_registers();

  for(int c = 0; c < nproc; c++){
    int pid = fork();
    if(pid < 0){
      printf("fptest: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      int bad = 0;
      for(int r = 0; r < rounds; r++){
        bad += hold_registers(0x1000UL * (c + 1) + r, spins);
        if(basel(200000) != ref)
          bad++;
        sched_yield();
      }
      if(bad)
        printf("fptest: child %d: %d corrupted values\n", c, bad);
      exit(bad ? 1 : 0);
    }
  }
  for(int c = 0; c < nproc; c++){
    wait(&status);
    if(status != 0)
      failures++;
  }
  printf("fptest: basel(200000) = %d.%d (x1e-9), %d tasks x %d rounds\n",
         (int)(ref / 1000000000), (int)(ref % 1000000000), nproc, rounds);
  printf("fptest: %s\n", failures ? "FAILED" : "ok");
  exit(failures ? 1 : 0);
}
