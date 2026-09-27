#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "net.h"

struct udp_dgram {
  struct udp_dgram *next;
  uint32 src;
  uint16 sport;
  uint16 len;
  uchar data[UDP_MAX_PAYLOAD];
};

struct udp_port {
  struct spinlock lock;
  int used;
  int port;
  int owner;
  int waiters;
  int queued;
  struct udp_dgram *head;
  struct udp_dgram *tail;
};

static struct udp_port ports[NUDPPORT];
static struct spinlock porttable_lock;
static int (*netdev_xmit)(void*, int);
static uint16 ip_id;
static uchar local_mac[ETH_ADDR_LEN] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
// QEMU SLIRP's Ethernet gateway address for the default 10.0.2.0/24 LAN.
static uchar gateway_mac[ETH_ADDR_LEN] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

static uint16
swap16(uint16 x)
{
  return (x << 8) | (x >> 8);
}

static uint32
swap32(uint32 x)
{
  return ((x & 0x000000ffU) << 24) | ((x & 0x0000ff00U) << 8) |
         ((x & 0x00ff0000U) >> 8) | ((x & 0xff000000U) >> 24);
}

static uint16
checksum(void *data, int len)
{
  uchar *p = data;
  uint32 sum = 0;
  while(len > 1){
    sum += ((uint16)p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if(len)
    sum += (uint16)p[0] << 8;
  while(sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16)~sum;
}

void
netinit(void)
{
  int i;
  initlock(&porttable_lock, "udp ports");
  for(i = 0; i < NUDPPORT; i++)
    initlock(&ports[i].lock, "udp port");
}

void
net_set_xmit(int (*fn)(void*, int))
{
  netdev_xmit = fn;
}

static struct udp_port*
findport(int port)
{
  int i;
  for(i = 0; i < NUDPPORT; i++){
    acquire(&ports[i].lock);
    if(ports[i].used && ports[i].port == port)
      return &ports[i];
    release(&ports[i].lock);
  }
  return 0;
}

int
net_udp_bind(int port)
{
  int i;
  struct udp_port *p;
  if(port <= 0 || port > 65535)
    return -1;

  acquire(&porttable_lock);
  for(i = 0; i < NUDPPORT; i++){
    acquire(&ports[i].lock);
    if(ports[i].used && ports[i].port == port){
      release(&ports[i].lock);
      release(&porttable_lock);
      return -1;
    }
    release(&ports[i].lock);
  }
  for(i = 0; i < NUDPPORT; i++){
    p = &ports[i];
    acquire(&p->lock);
    if(!p->used){
      p->used = 1;
      p->port = port;
      p->owner = myproc()->pid;
      p->waiters = p->queued = 0;
      p->head = p->tail = 0;
      release(&p->lock);
      release(&porttable_lock);
      return 0;
    }
    release(&p->lock);
  }
  release(&porttable_lock);
  return -1;
}

int
net_udp_unbind(int port)
{
  struct udp_port *p = findport(port);
  struct udp_dgram *d, *next;
  if(p == 0)
    return -1;
  if(p->owner != myproc()->pid || p->waiters != 0){
    release(&p->lock);
    return -1;
  }
  for(d = p->head; d; d = next){
    next = d->next;
    kfree(d);
  }
  p->used = 0;
  p->head = p->tail = 0;
  p->queued = 0;
  release(&p->lock);
  return 0;
}

static void
udp_rx(uint32 src, uint16 sport, uint16 dport, uchar *data, int len)
{
  struct udp_port *p;
  struct udp_dgram *d;
  if(len < 0 || len > UDP_MAX_PAYLOAD)
    return;
  p = findport(dport);
  if(p == 0 || p->queued >= NUDPQUEUE){
    if(p)
      release(&p->lock);
    return;
  }
  d = kalloc();
  if(d == 0){
    release(&p->lock);
    return;
  }
  d->next = 0;
  d->src = src;
  d->sport = sport;
  d->len = len;
  memmove(d->data, data, len);
  if(p->tail)
    p->tail->next = d;
  else
    p->head = d;
  p->tail = d;
  p->queued++;
  wakeup(p);
  release(&p->lock);
}

static void
arp_rx(struct ethhdr *eth, int len)
{
  struct arphdr *arp, *reply;
  struct ethhdr *reth;
  uchar *packet;
  int framelen = sizeof(*reth) + sizeof(*reply);

  if(len < framelen || netdev_xmit == 0)
    return;
  arp = (struct arphdr*)(eth + 1);
  if(swap16(arp->htype) != ARP_HTYPE_ETH ||
     swap16(arp->ptype) != ETH_TYPE_IP || arp->hlen != ETH_ADDR_LEN ||
     arp->plen != 4 || swap16(arp->op) != ARP_OP_REQUEST ||
     swap32(arp->tpa) != NET_IP_LOCAL)
    return;

  packet = kalloc();
  if(packet == 0)
    return;
  memset(packet, 0, framelen);
  reth = (struct ethhdr*)packet;
  memmove(reth->dst, arp->sha, ETH_ADDR_LEN);
  memmove(reth->src, local_mac, ETH_ADDR_LEN);
  reth->type = swap16(ETH_TYPE_ARP);
  reply = (struct arphdr*)(reth + 1);
  reply->htype = swap16(ARP_HTYPE_ETH);
  reply->ptype = swap16(ETH_TYPE_IP);
  reply->hlen = ETH_ADDR_LEN;
  reply->plen = 4;
  reply->op = swap16(ARP_OP_REPLY);
  memmove(reply->sha, local_mac, ETH_ADDR_LEN);
  reply->spa = swap32(NET_IP_LOCAL);
  memmove(reply->tha, arp->sha, ETH_ADDR_LEN);
  reply->tpa = arp->spa;
  netdev_xmit(packet, framelen);
  kfree(packet);
}

void
net_rx(void *packet, int len)
{
  struct ethhdr *eth;
  struct iphdr *ip;
  struct udphdr *udp;
  int iplen, udplen, hlen;
  uchar *p = packet;

  if(len < (int)(sizeof(*eth) + sizeof(*ip)))
    return;
  eth = (struct ethhdr*)p;
  if(swap16(eth->type) == ETH_TYPE_ARP){
    arp_rx(eth, len);
    return;
  }
  if(swap16(eth->type) != ETH_TYPE_IP)
    return;
  ip = (struct iphdr*)(eth + 1);
  hlen = (ip->vhl & 0xf) * 4;
  if((ip->vhl >> 4) != 4 || hlen < 20 || ip->proto != IP_PROTO_UDP)
    return;
  iplen = swap16(ip->len);
  if(iplen < hlen + (int)sizeof(*udp) ||
     iplen > len - (int)sizeof(*eth))
    return;
  if(checksum(ip, hlen) != 0)
    return;
  udp = (struct udphdr*)((uchar*)ip + hlen);
  udplen = swap16(udp->len);
  if(udplen < (int)sizeof(*udp) || udplen > iplen - hlen)
    return;
  udp_rx(swap32(ip->src), swap16(udp->sport), swap16(udp->dport),
         (uchar*)(udp + 1), udplen - sizeof(*udp));
}

int
net_udp_send(uint32 dst, int sport, int dport, uint64 uaddr, int len)
{
  uchar *packet;
  struct ethhdr *eth;
  struct iphdr *ip;
  struct udphdr *udp;
  int framelen;

  if(sport <= 0 || sport > 65535 || dport <= 0 || dport > 65535 ||
     len < 0 || len > UDP_MAX_PAYLOAD)
    return -1;
  framelen = sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + len;
  packet = kalloc();
  if(packet == 0)
    return -1;
  memset(packet, 0, framelen);
  eth = (struct ethhdr*)packet;
  memmove(eth->src, local_mac, ETH_ADDR_LEN);
  if(dst == NET_IP_LOOPBACK || dst == NET_IP_LOCAL)
    memmove(eth->dst, local_mac, ETH_ADDR_LEN);
  else
    memmove(eth->dst, gateway_mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  ip->vhl = 0x45;
  ip->len = swap16(sizeof(*ip) + sizeof(*udp) + len);
  ip->id = swap16(++ip_id);
  ip->ttl = 64;
  ip->proto = IP_PROTO_UDP;
  ip->src = swap32(NET_IP_LOCAL);
  ip->dst = swap32(dst == NET_IP_LOOPBACK ? NET_IP_LOCAL : dst);
  ip->sum = swap16(checksum(ip, sizeof(*ip)));
  udp = (struct udphdr*)(ip + 1);
  udp->sport = swap16(sport);
  udp->dport = swap16(dport);
  udp->len = swap16(sizeof(*udp) + len);
  udp->sum = 0; // IPv4 permits a zero UDP checksum.
  if(copyin(myproc()->pagetable, (char*)(udp + 1), uaddr, len) < 0){
    kfree(packet);
    return -1;
  }

  if(dst == NET_IP_LOOPBACK || dst == NET_IP_LOCAL){
    net_rx(packet, framelen);
    kfree(packet);
    return len;
  }
  if(netdev_xmit){
    int r = netdev_xmit(packet, framelen);
    kfree(packet);
    return r < 0 ? -1 : len;
  }
  kfree(packet);
  return -1;
}

int
net_udp_recv(int port, uint64 srcaddr, uint64 sportaddr, uint64 uaddr, int maxlen)
{
  struct udp_port *p = findport(port);
  struct udp_dgram *d;
  int n;
  if(p == 0 || maxlen < 0){
    if(p)
      release(&p->lock);
    return -1;
  }
  p->waiters++;
  while(p->head == 0){
    if(myproc()->killed){
      p->waiters--;
      release(&p->lock);
      return -1;
    }
    sleep(p, &p->lock);
  }
  d = p->head;
  p->head = d->next;
  if(p->head == 0)
    p->tail = 0;
  p->queued--;
  p->waiters--;
  release(&p->lock);

  n = d->len < maxlen ? d->len : maxlen;
  if((srcaddr && copyout(myproc()->pagetable, srcaddr, (char*)&d->src,
                         sizeof(d->src)) < 0) ||
     (sportaddr && copyout(myproc()->pagetable, sportaddr, (char*)&d->sport,
                           sizeof(d->sport)) < 0) ||
     copyout(myproc()->pagetable, uaddr, (char*)d->data, n) < 0){
    kfree(d);
    return -1;
  }
  kfree(d);
  return n;
}
