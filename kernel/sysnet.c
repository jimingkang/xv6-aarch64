#include "types.h"
#include "aarch64.h"
#include "defs.h"
#include "net.h"

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

uint64
sys_wifi_connect(void)
{
  char ssid[33];
  char passphrase[64];
  int result;
  if(argstr(0, ssid, sizeof(ssid)) < 0 ||
     argstr(1, passphrase, sizeof(passphrase)) < 0)
    return -1;
  // BCM43455 control requests and MT7601U scans are both bounded polling
  // transactions.  Do not let timer-driven USB channel changes stretch a
  // FullMAC BCDC response past its deadline.
  mt7601u_pause(1);
  result = brcmfmac_connect(ssid, passphrase);
  mt7601u_pause(0);
  return result;
}

uint64
sys_net_dhcp(void)
{
  char name[16];
  struct net_device *dev;
  if(argstr(0, name, sizeof(name)) < 0)
    return -1;
  dev = netdev_find(name);
  if(dev == 0)
    return -1;
  return net_dhcp_dev(dev);
}

uint64 sys_tcp_listen(void) { int p; return argint(0, &p) < 0 ? -1 : net_tcp_listen(p, 4); }
uint64 sys_tcp_accept(void) { int h; return argint(0, &h) < 0 ? -1 : net_tcp_accept(h, 0); }
uint64 sys_tcp_read(void) {
  int h, n; uint64 p;
  return argint(0, &h) < 0 || argaddr(1, &p) < 0 || argint(2, &n) < 0 ?
         -1 : net_tcp_read(h, p, n, 0);
}
uint64 sys_tcp_write(void) {
  int h, n; uint64 p;
  return argint(0, &h) < 0 || argaddr(1, &p) < 0 || argint(2, &n) < 0 ?
         -1 : net_tcp_write(h, p, n, 0);
}
uint64 sys_tcp_close(void) { int h; return argint(0, &h) < 0 ? -1 : net_tcp_close(h); }
