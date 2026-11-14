#include "kernel/types.h"
#include "user/user.h"
static void require(int ok, char *what) {
  if(!ok) { printf("armcheck: FAIL %s\n",what); exit(1); }
}
int main(void) {
  uint before=uptime();
  require(sleep_ticks(2)==0 && uptime()-before>=2,"timer sleep");
  volatile int private=7;
  int pid=fork(), status=0;
  require(pid>=0,"fork");
  if(pid==0) { private=42; exit(23); }
  require(wait(&status)==pid && status==23 && private==7,"fork isolation/status");
  pid=fork(); require(pid>=0,"fault fork");
  if(pid==0) { *(volatile uint*)(uintptr)0x40200000=0; exit(2); }
  require(wait(&status)==pid && status==-1,"kernel memory protection");
  pid=fork(); require(pid>=0,"undefined fork");
  if(pid==0) { asm volatile("udf #0"); exit(2); }
  require(wait(&status)==pid && status==-1,"undefined instruction exit");
  printf("armcheck: OK\n");
  exit(0);
}
