#include "kernel/types.h"
#include "user/user.h"

#define IP_LOOPBACK 0x7f000001U

int
main(void)
{
  char tx[] = "hello from xv6 UDP";
  char rx[64];
  uint32 src;
  uint16 sport;
  int pid, n;

  if(udp_bind(2000) < 0){
    printf("nettest: bind failed\n");
    exit(1);
  }
  pid = fork();
  if(pid < 0){
    printf("nettest: fork failed\n");
    exit(1);
  }
  if(pid == 0){
    if(udp_send(IP_LOOPBACK, 1000, 2000, tx, sizeof(tx)) < 0){
      printf("nettest: send failed\n");
      exit(1);
    }
    exit(0);
  }

  n = udp_recv(2000, &src, &sport, rx, sizeof(rx) - 1);
  if(n < 0){
    printf("nettest: recv failed\n");
    exit(1);
  }
  rx[n] = 0;
  printf("nettest: src=%d.%d.%d.%d:%d len=%d data='%s'\n",
         (src >> 24) & 255, (src >> 16) & 255,
         (src >> 8) & 255, src & 255, sport, n, rx);
  wait(0);
  udp_unbind(2000);
  printf("nettest: ok\n");
  exit(0);
}
