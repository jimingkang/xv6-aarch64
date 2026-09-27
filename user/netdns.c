#include "kernel/types.h"
#include "user/user.h"

#define QEMU_DNS 0x0a000203U
#define LOCAL_PORT 53000

// DNS query for A google.com, transaction id 0x1234.
static uchar query[] = {
  0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x06, 'g','o','o','g','l','e', 0x03, 'c','o','m', 0x00,
  0x00, 0x01, 0x00, 0x01
};

int
main(void)
{
  uchar response[512];
  uint32 src;
  uint16 sport;
  int n, answers;

  if(udp_bind(LOCAL_PORT) < 0){
    printf("netdns: bind failed\n");
    exit(1);
  }
  if(udp_send(QEMU_DNS, LOCAL_PORT, 53, query, sizeof(query)) < 0){
    printf("netdns: send failed (is make qemu-net in use?)\n");
    udp_unbind(LOCAL_PORT);
    exit(1);
  }
  printf("netdns: query sent to 10.0.2.3:53, waiting...\n");
  n = udp_recv(LOCAL_PORT, &src, &sport, response, sizeof(response));
  if(n < 12){
    printf("netdns: short response len=%d\n", n);
    exit(1);
  }
  answers = ((int)response[6] << 8) | response[7];
  printf("netdns: reply from %d.%d.%d.%d:%d len=%d answers=%d rcode=%d\n",
         (src >> 24) & 255, (src >> 16) & 255,
         (src >> 8) & 255, src & 255, sport, n, answers,
         response[3] & 0x0f);
  udp_unbind(LOCAL_PORT);
  exit((response[3] & 0x0f) == 0 && answers > 0 ? 0 : 1);
}
