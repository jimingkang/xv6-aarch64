// Verify that the growable process table can exceed xv6's former 64-slot
// limit, and that every dynamically allocated slot can be reaped.

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define N  128
#define OLD_NPROC 64

void
print(const char *s)
{
  write(1, s, strlen(s));
}

void
forktest(void)
{
  int n, pid, created;

  print("fork test\n");

  for(n=0; n<N; n++){
    pid = fork();
    if(pid < 0)
      break;
    if(pid == 0)
      exit(0);
  }

  created = n;
  for(; n > 0; n--){
    if(wait(0) < 0){
      print("wait stopped early\n");
      exit(1);
    }
  }

  if(wait(0) != -1){
    print("wait got too many\n");
    exit(1);
  }

  if(created <= OLD_NPROC){
    print("fork stopped at old static limit\n");
    exit(1);
  }

  print("dynamic fork test OK\n");
}

int
main(void)
{
  forktest();
  exit(0);
}
