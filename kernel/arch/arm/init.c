#include "kernel/types.h"
#include "kernel/fcntl.h"

#include "user/user.h"
int main(void) {
  mkdir("/dev");
  mknod("/dev/console",1,0);
  mknod("/dev/ttyS0",3,0);
  mknod("/dev/fb0",5,0);    /* FB: present when a framebuffer driver registered */
  /* The SD card's FAT32 boot partition at /boot (kernels with the VFS). */
  mkdir("/boot");
  if(mount("sd0p1","/boot","fat32",0)==0)
    printf("init: /boot is the SD card's FAT32 partition\n");
  if(tty_attach(0)<0) exit(1);
  if(open("/dev/ttyS0",O_RDWR)<0) exit(1);
  dup(0); dup(0);
  /* With /etc/wifi.conf (ssid=, psk=) bring the network up in the
     background: retry wifi until the USB adapter is ready, then DHCP. */
  int cfd=open("/etc/wifi.conf",O_RDONLY);
  if(cfd>=0) {
    close(cfd);
    if(fork()==0) {
      char *wargs[]={"wifi",0}, *dargs[]={"dhcp","wlan0",0};
      for(int tries=0; tries<10; tries++) {
        int st=-1, pid=fork();
        if(pid==0) { exec("/bin/wifi",wargs); exit(1); }
        wait(&st);
        if(st==0) { exec("/bin/dhcp",dargs); exit(1); }
        sleep_ticks(30);
      }
      exit(1);
    }
  }
  char *args[]={"sh",0};
  for(;;) {
    printf("init: starting sh\n");
    int pid=fork();
    if(pid<0) exit(1);
    if(pid==0) { exec("/bin/sh",args); printf("init: exec sh failed\n"); exit(1); }
    int reaped; do { reaped=wait(0); } while(reaped>=0 && reaped!=pid);
  }
}
