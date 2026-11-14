#include "kernel/types.h"
#include "kernel/fcntl.h"

#include "user/user.h"
int main(void) {
  mkdir("/dev");
  mknod("/dev/console",1,0);
  mknod("/dev/ttyS0",3,0);
  if(tty_attach(0)<0) exit(1);
  if(open("/dev/ttyS0",O_RDWR)<0) exit(1);
  dup(0); dup(0);
  char *args[]={"sh",0};
  for(;;) {
    printf("init: starting sh\n");
    int pid=fork();
    if(pid<0) exit(1);
    if(pid==0) { exec("/bin/sh",args); printf("init: exec sh failed\n"); exit(1); }
    int reaped; do { reaped=wait(0); } while(reaped>=0 && reaped!=pid);
  }
}
