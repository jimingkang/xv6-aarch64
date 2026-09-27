#include "kernel/types.h"
#include "user/user.h"

#define QEMU_DNS 0x0a000203U
#define DNS_PORT 53001

static int
parse_ip(const char *s, uint32 *ip)
{
  uint32 v = 0;
  int part = 0, dots = 0, digit = 0;

  for(;; s++){
    if(*s >= '0' && *s <= '9'){
      part = part * 10 + *s - '0';
      if(part > 255)
        return -1;
      digit = 1;
    } else if(*s == '.' || *s == 0){
      if(!digit)
        return -1;
      v = (v << 8) | part;
      if(*s == 0)
        break;
      dots++;
      part = digit = 0;
    } else {
      return -1;
    }
  }
  if(dots != 3)
    return -1;
  *ip = v;
  return 0;
}

static int
skip_name(uchar *msg, int len, int off)
{
  while(off < len){
    int n = msg[off++];
    if((n & 0xc0) == 0xc0)
      return off < len ? off + 1 : -1;
    if(n == 0)
      return off;
    if(n > 63 || off + n > len)
      return -1;
    off += n;
  }
  return -1;
}

static int
resolve(const char *name, uint32 *ip)
{
  uchar query[256], reply[512];
  uint32 src;
  uint16 sport;
  int q = 12, start, n, off, answers, rdlen;
  const char *p = name;

  memset(query, 0, sizeof(query));
  query[0] = 0x51;
  query[1] = 0x71;
  query[2] = 0x01;
  query[5] = 0x01;
  while(*p){
    start = q++;
    query[start] = 0;
    while(*p && *p != '.'){
      if(q >= (int)sizeof(query) - 5 || query[start] == 63)
        return -1;
      query[q++] = *p++;
      query[start]++;
    }
    if(*p == '.')
      p++;
    if(query[start] == 0)
      return -1;
  }
  query[q++] = 0;
  query[q++] = 0;
  query[q++] = 1; // A
  query[q++] = 0;
  query[q++] = 1; // IN

  if(udp_bind(DNS_PORT) < 0)
    return -1;
  // On a fresh QEMU SLIRP network the first outbound frame triggers an ARP
  // request for the guest.  Give that exchange time to complete, then send
  // the query that we wait for; otherwise the first UDP packet can be lost.
  if(udp_send(QEMU_DNS, DNS_PORT, 53, query, q) < 0){
    udp_unbind(DNS_PORT);
    return -1;
  }
  sleep(5);
  if(udp_send(QEMU_DNS, DNS_PORT, 53, query, q) < 0){
    udp_unbind(DNS_PORT);
    return -1;
  }
  n = udp_recv(DNS_PORT, &src, &sport, reply, sizeof(reply));
  udp_unbind(DNS_PORT);
  if(n < 12 || src != QEMU_DNS || sport != 53 ||
     reply[0] != 0x51 || reply[1] != 0x71 || (reply[3] & 15) != 0)
    return -1;
  answers = ((int)reply[6] << 8) | reply[7];
  off = skip_name(reply, n, 12);
  if(off < 0 || off + 4 > n)
    return -1;
  off += 4;
  while(answers-- > 0){
    off = skip_name(reply, n, off);
    if(off < 0 || off + 10 > n)
      return -1;
    rdlen = ((int)reply[off + 8] << 8) | reply[off + 9];
    if(off + 10 + rdlen > n)
      return -1;
    if(reply[off] == 0 && reply[off + 1] == 1 &&
       reply[off + 2] == 0 && reply[off + 3] == 1 && rdlen == 4){
      uchar *a = reply + off + 10;
      *ip = ((uint32)a[0] << 24) | ((uint32)a[1] << 16) |
            ((uint32)a[2] << 8) | a[3];
      return 0;
    }
    off += 10 + rdlen;
  }
  return -1;
}

static void
print_ip(uint32 ip)
{
  printf("%d.%d.%d.%d", (ip >> 24) & 255, (ip >> 16) & 255,
         (ip >> 8) & 255, ip & 255);
}

int
main(int argc, char **argv)
{
  uchar tx[32], rx[64];
  uint32 dst, src;
  uint16 rseq;
  int id, seq, n, before, after;

  if(argc != 2){
    fprintf(2, "usage: ping host\n");
    exit(1);
  }
  if(parse_ip(argv[1], &dst) < 0 && resolve(argv[1], &dst) < 0){
    fprintf(2, "ping: cannot resolve %s\n", argv[1]);
    exit(1);
  }
  printf("PING %s (", argv[1]);
  print_ip(dst);
  printf("): %d data bytes\n", (int)sizeof(tx));
  id = getpid() & 0xffff;
  for(seq = 1; seq <= 4; seq++){
    for(n = 0; n < (int)sizeof(tx); n++)
      tx[n] = n + seq;
    before = uptime();
    if(icmp_send(dst, id, seq, tx, sizeof(tx)) < 0 ||
       (n = icmp_recv(id, &src, &rseq, rx, sizeof(rx))) < 0){
      fprintf(2, "ping: send/receive failed\n");
      exit(1);
    }
    after = uptime();
    printf("%d bytes from ", n);
    print_ip(src);
    printf(": icmp_seq=%d time=%d ms\n", rseq, (after - before) * 100);
    if(seq != 4)
      sleep(10);
  }
  exit(0);
}
