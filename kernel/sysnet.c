#include "types.h"
#include "aarch64.h"
#include "defs.h"

uint64
sys_udp_bind(void)
{
  int port;
  if(argint(0, &port) < 0)
    return -1;
  return net_udp_bind(port);
}

uint64
sys_udp_unbind(void)
{
  int port;
  if(argint(0, &port) < 0)
    return -1;
  return net_udp_unbind(port);
}

uint64
sys_udp_send(void)
{
  int dst, sport, dport, len;
  uint64 buf;
  if(argint(0, &dst) < 0 || argint(1, &sport) < 0 ||
     argint(2, &dport) < 0 || argaddr(3, &buf) < 0 ||
     argint(4, &len) < 0)
    return -1;
  return net_udp_send((uint32)dst, sport, dport, buf, len);
}

uint64
sys_udp_recv(void)
{
  int port, maxlen;
  uint64 src, sport, buf;
  if(argint(0, &port) < 0 || argaddr(1, &src) < 0 ||
     argaddr(2, &sport) < 0 || argaddr(3, &buf) < 0 ||
     argint(4, &maxlen) < 0)
    return -1;
  return net_udp_recv(port, src, sport, buf, maxlen);
}

uint64
sys_icmp_send(void)
{
  int dst, id, seq, len;
  uint64 buf;
  if(argint(0, &dst) < 0 || argint(1, &id) < 0 ||
     argint(2, &seq) < 0 || argaddr(3, &buf) < 0 ||
     argint(4, &len) < 0)
    return -1;
  return net_icmp_send((uint32)dst, id, seq, buf, len);
}

uint64
sys_icmp_recv(void)
{
  int id, maxlen;
  uint64 src, seq, buf;
  if(argint(0, &id) < 0 || argaddr(1, &src) < 0 ||
     argaddr(2, &seq) < 0 || argaddr(3, &buf) < 0 ||
     argint(4, &maxlen) < 0)
    return -1;
  return net_icmp_recv(id, src, seq, buf, maxlen);
}
