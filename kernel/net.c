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
static struct {
  struct spinlock lock;
  int owner;
  uint16 id;
  int ready;
  uint32 src;
  uint16 seq;
  uint16 len;
  uchar data[ICMP_MAX_PAYLOAD];
} pingq;
static int (*netdev_xmit)(void*, int);
static void (*netdev_poll)(void);
static uint16 ip_id;
static uchar local_mac[ETH_ADDR_LEN] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
// QEMU SLIRP's Ethernet gateway address for the default 10.0.2.0/24 LAN.
static uchar gateway_mac[ETH_ADDR_LEN] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static uint32 local_ip = NET_IP_LOCAL;
static uint32 gateway_ip = NET_IP_GATEWAY;
static uint32 netmask = 0xffffff00U;
static uint32 dns_ip = 0x0a000203U;
static int gateway_resolved = 1;
static struct {
  uint32 xid;
  int waiting;
  int type;
  uint32 yiaddr;
  uint32 server;
  uint32 router;
  uint32 mask;
  uint32 dns;
  uchar server_mac[ETH_ADDR_LEN];
} dhcp;

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
  initlock(&pingq.lock, "icmp reply");
  for(i = 0; i < NUDPPORT; i++)
    initlock(&ports[i].lock, "udp port");
}

static void
icmp_rx(struct ethhdr *eth, struct iphdr *ip, int hlen, int iplen)
{
  struct icmphdr *icmp, *reply;
  struct ethhdr *reth;
  struct iphdr *rip;
  uchar *packet;
  int icmplen = iplen - hlen;
  int framelen;

  if(icmplen < (int)sizeof(*icmp))
    return;
  icmp = (struct icmphdr*)((uchar*)ip + hlen);
  if(checksum(icmp, icmplen) != 0 || icmp->code != 0)
    return;

  if(icmp->type == ICMP_ECHO_REPLY){
    acquire(&pingq.lock);
    if(pingq.owner != 0 && pingq.id == swap16(icmp->id)){
      int n = icmplen - sizeof(*icmp);
      if(n > ICMP_MAX_PAYLOAD)
        n = ICMP_MAX_PAYLOAD;
      pingq.src = swap32(ip->src);
      pingq.seq = swap16(icmp->seq);
      pingq.len = n;
      memmove(pingq.data, icmp + 1, n);
      pingq.ready = 1;
      wakeup(&pingq);
    }
    release(&pingq.lock);
    return;
  }

  if(icmp->type != ICMP_ECHO_REQUEST || swap32(ip->dst) != local_ip ||
     netdev_xmit == 0)
    return;
  framelen = sizeof(*reth) + sizeof(*rip) + icmplen;
  packet = kalloc();
  if(packet == 0)
    return;
  reth = (struct ethhdr*)packet;
  memmove(reth->dst, eth->src, ETH_ADDR_LEN);
  memmove(reth->src, local_mac, ETH_ADDR_LEN);
  reth->type = swap16(ETH_TYPE_IP);
  rip = (struct iphdr*)(reth + 1);
  memmove(rip, ip, hlen + icmplen);
  rip->src = ip->dst;
  rip->dst = ip->src;
  rip->ttl = 64;
  rip->id = swap16(++ip_id);
  rip->sum = 0;
  rip->sum = swap16(checksum(rip, hlen));
  reply = (struct icmphdr*)((uchar*)rip + hlen);
  reply->type = ICMP_ECHO_REPLY;
  reply->sum = 0;
  reply->sum = swap16(checksum(reply, icmplen));
  netdev_xmit(packet, framelen);
  kfree(packet);
}

void
net_set_xmit(int (*fn)(void*, int))
{
  netdev_xmit = fn;
}

void
net_set_poll(void (*fn)(void))
{
  netdev_poll = fn;
}

void
net_set_mac(uint8 *mac)
{
  memmove(local_mac, mac, ETH_ADDR_LEN);
  local_ip = gateway_ip = netmask = dns_ip = 0;
  gateway_resolved = 0;
  memset(gateway_mac, 0, sizeof(gateway_mac));
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

static uint32
dhcp_ip(uchar *p)
{
  return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
         ((uint32)p[2] << 8) | p[3];
}

static void
dhcp_rx(struct ethhdr *eth, uchar *data, int len)
{
  int pos, end, type = 0;
  uint32 xid;

  if(!dhcp.waiting || len < 240 || data[0] != 2 ||
     data[1] != 1 || data[2] != 6 ||
     data[236] != 99 || data[237] != 130 ||
     data[238] != 83 || data[239] != 99)
    return;
  xid = dhcp_ip(data + 4);
  if(xid != dhcp.xid)
    return;
  dhcp.yiaddr = dhcp_ip(data + 16);
  dhcp.server = dhcp.router = dhcp.mask = dhcp.dns = 0;
  for(pos = 240; pos < len;){
    int code = data[pos++];
    if(code == 0) continue;
    if(code == 255) break;
    if(pos >= len) break;
    end = pos + 1 + data[pos];
    if(end > len) break;
    int n = data[pos++];
    if(code == 53 && n >= 1) type = data[pos];
    else if(code == 1 && n >= 4) dhcp.mask = dhcp_ip(data + pos);
    else if(code == 3 && n >= 4) dhcp.router = dhcp_ip(data + pos);
    else if(code == 6 && n >= 4) dhcp.dns = dhcp_ip(data + pos);
    else if(code == 54 && n >= 4) dhcp.server = dhcp_ip(data + pos);
    pos += n;
  }
  if(type == 2 || type == 5){
    dhcp.type = type;
    memmove(dhcp.server_mac, eth->src, ETH_ADDR_LEN);
  }
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
     arp->plen != 4)
    return;
  if(swap16(arp->op) == ARP_OP_REPLY && swap32(arp->spa) == gateway_ip){
    memmove(gateway_mac, arp->sha, ETH_ADDR_LEN);
    gateway_resolved = 1;
    return;
  }
  if(swap16(arp->op) != ARP_OP_REQUEST || swap32(arp->tpa) != local_ip)
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
  reply->spa = swap32(local_ip);
  memmove(reply->tha, arp->sha, ETH_ADDR_LEN);
  reply->tpa = arp->spa;
  netdev_xmit(packet, framelen);
  kfree(packet);
}

static int
arp_request(uint32 target)
{
  uchar packet[sizeof(struct ethhdr) + sizeof(struct arphdr)];
  struct ethhdr *eth = (struct ethhdr*)packet;
  struct arphdr *arp = (struct arphdr*)(eth + 1);
  memset(packet, 0, sizeof(packet));
  memset(eth->dst, 0xff, ETH_ADDR_LEN);
  memmove(eth->src, local_mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_ARP);
  arp->htype = swap16(ARP_HTYPE_ETH);
  arp->ptype = swap16(ETH_TYPE_IP);
  arp->hlen = ETH_ADDR_LEN; arp->plen = 4;
  arp->op = swap16(ARP_OP_REQUEST);
  memmove(arp->sha, local_mac, ETH_ADDR_LEN);
  arp->spa = swap32(local_ip);
  arp->tpa = swap32(target);
  return netdev_xmit ? netdev_xmit(packet, sizeof(packet)) : -1;
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
  if((ip->vhl >> 4) != 4 || hlen < 20)
    return;
  iplen = swap16(ip->len);
  if(iplen < hlen + (int)sizeof(*udp) ||
     iplen > len - (int)sizeof(*eth))
    return;
  if(checksum(ip, hlen) != 0)
    return;
  if(ip->proto == IP_PROTO_ICMP){
    icmp_rx(eth, ip, hlen, iplen);
    return;
  }
  if(ip->proto != IP_PROTO_UDP)
    return;
  udp = (struct udphdr*)((uchar*)ip + hlen);
  udplen = swap16(udp->len);
  if(udplen < (int)sizeof(*udp) || udplen > iplen - hlen)
    return;
  if(swap16(udp->sport) == 67 && swap16(udp->dport) == 68)
    dhcp_rx(eth, (uchar*)(udp + 1), udplen - sizeof(*udp));
  uint32 udp_src = swap32(ip->src);
  // Preserve the original QEMU-facing user ABI: programs historically use
  // 10.0.2.3 as the DNS endpoint.  On a DHCP-configured interface translate
  // that stable alias to/from the real resolver learned from option 6.
  if(dns_ip && dns_ip != 0x0a000203U && udp_src == dns_ip &&
     swap16(udp->sport) == 53)
    udp_src = 0x0a000203U;
  udp_rx(udp_src, swap16(udp->sport), swap16(udp->dport),
         (uchar*)(udp + 1), udplen - sizeof(*udp));
}

static int
dhcp_send(int type, uint32 requested, uint32 server)
{
  uchar *packet, *bootp, *opt;
  struct ethhdr *eth;
  struct iphdr *ip;
  struct udphdr *udp;
  int bootplen, framelen;

  packet = kalloc();
  if(packet == 0)
    return -1;
  memset(packet, 0, PGSIZE);
  eth = (struct ethhdr*)packet;
  memset(eth->dst, 0xff, ETH_ADDR_LEN);
  memmove(eth->src, local_mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  udp = (struct udphdr*)(ip + 1);
  bootp = (uchar*)(udp + 1);
  bootp[0] = 1; bootp[1] = 1; bootp[2] = 6;
  bootp[4] = dhcp.xid >> 24; bootp[5] = dhcp.xid >> 16;
  bootp[6] = dhcp.xid >> 8; bootp[7] = dhcp.xid;
  bootp[10] = 0x80; // broadcast reply flag, network byte order
  memmove(bootp + 28, local_mac, ETH_ADDR_LEN);
  bootp[236] = 99; bootp[237] = 130;
  bootp[238] = 83; bootp[239] = 99;
  opt = bootp + 240;
  *opt++ = 53; *opt++ = 1; *opt++ = type;
  if(requested){
    *opt++ = 50; *opt++ = 4;
    *opt++ = requested >> 24; *opt++ = requested >> 16;
    *opt++ = requested >> 8; *opt++ = requested;
  }
  if(server){
    *opt++ = 54; *opt++ = 4;
    *opt++ = server >> 24; *opt++ = server >> 16;
    *opt++ = server >> 8; *opt++ = server;
  }
  *opt++ = 55; *opt++ = 3; *opt++ = 1; *opt++ = 3; *opt++ = 6;
  *opt++ = 255;
  bootplen = opt - bootp;
  if(bootplen < 300) bootplen = 300;
  udp->sport = swap16(68); udp->dport = swap16(67);
  udp->len = swap16(sizeof(*udp) + bootplen);
  ip->vhl = 0x45;
  ip->len = swap16(sizeof(*ip) + sizeof(*udp) + bootplen);
  ip->id = swap16(++ip_id); ip->ttl = 64; ip->proto = IP_PROTO_UDP;
  ip->src = 0; ip->dst = 0xffffffffU;
  ip->sum = swap16(checksum(ip, sizeof(*ip)));
  framelen = sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + bootplen;
  int result = netdev_xmit ? netdev_xmit(packet, framelen) : -1;
  kfree(packet);
  return result;
}

static int
dhcp_wait(int wanted, uint32 seconds)
{
  uint64 deadline = r_cntvct_el0() + (uint64)r_cntfrq_el0() * seconds;
  while(r_cntvct_el0() < deadline){
    if(dhcp.type == wanted)
      return 0;
    if(netdev_poll)
      netdev_poll();
    asm volatile("yield" ::: "memory");
  }
  return -1;
}

int
net_dhcp(void)
{
  uint32 offered, server;
  if(netdev_xmit == 0 || netdev_poll == 0)
    return -1;
  memset(&dhcp, 0, sizeof(dhcp));
  dhcp.xid = (uint32)r_cntvct_el0() ^
             ((uint32)local_mac[2] << 24) ^
             ((uint32)local_mac[5] << 8);
  dhcp.waiting = 1;
  if(dhcp_send(1, 0, 0) < 0 || dhcp_wait(2, 5) < 0){
    dhcp.waiting = 0;
    printf("dhcp: no offer\n");
    return -1;
  }
  offered = dhcp.yiaddr;
  server = dhcp.server;
  dhcp.type = 0;
  if(dhcp_send(3, offered, server) < 0 || dhcp_wait(5, 5) < 0){
    dhcp.waiting = 0;
    printf("dhcp: no acknowledgement\n");
    return -1;
  }
  local_ip = dhcp.yiaddr;
  netmask = dhcp.mask ? dhcp.mask : 0xffffff00U;
  gateway_ip = dhcp.router ? dhcp.router : dhcp.server;
  dns_ip = dhcp.dns ? dhcp.dns : gateway_ip;
  memmove(gateway_mac, dhcp.server_mac, ETH_ADDR_LEN);
  dhcp.waiting = 0;
  printf("dhcp: address=%d.%d.%d.%d gateway=%d.%d.%d.%d dns=%d.%d.%d.%d\n",
         (local_ip>>24)&255, (local_ip>>16)&255, (local_ip>>8)&255, local_ip&255,
         (gateway_ip>>24)&255, (gateway_ip>>16)&255,
         (gateway_ip>>8)&255, gateway_ip&255,
         (dns_ip>>24)&255, (dns_ip>>16)&255, (dns_ip>>8)&255, dns_ip&255);
  gateway_resolved = 0;
  if(gateway_ip && arp_request(gateway_ip) >= 0){
    uint64 deadline = r_cntvct_el0() + (uint64)r_cntfrq_el0() * 2;
    while(!gateway_resolved && r_cntvct_el0() < deadline){
      netdev_poll();
      asm volatile("yield" ::: "memory");
    }
  }
  if(gateway_resolved)
    printf("arp: gateway %d.%d.%d.%d is %x:%x:%x:%x:%x:%x\n",
           (gateway_ip>>24)&255, (gateway_ip>>16)&255,
           (gateway_ip>>8)&255, gateway_ip&255,
           gateway_mac[0], gateway_mac[1], gateway_mac[2],
           gateway_mac[3], gateway_mac[4], gateway_mac[5]);
  else
    printf("arp: gateway reply timeout; using DHCP server MAC\n");
  return 0;
}

int
net_icmp_send(uint32 dst, int id, int seq, uint64 uaddr, int len)
{
  uchar *packet;
  struct ethhdr *eth;
  struct iphdr *ip;
  struct icmphdr *icmp;
  int framelen;

  if(id < 0 || id > 65535 || seq < 0 || seq > 65535 ||
     len < 0 || len > ICMP_MAX_PAYLOAD)
    return -1;
  framelen = sizeof(*eth) + sizeof(*ip) + sizeof(*icmp) + len;
  packet = kalloc();
  if(packet == 0)
    return -1;
  memset(packet, 0, framelen);
  eth = (struct ethhdr*)packet;
  memmove(eth->src, local_mac, ETH_ADDR_LEN);
  memmove(eth->dst, dst == local_ip ? local_mac : gateway_mac,
          ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  ip->vhl = 0x45;
  ip->len = swap16(sizeof(*ip) + sizeof(*icmp) + len);
  ip->id = swap16(++ip_id);
  ip->ttl = 64;
  ip->proto = IP_PROTO_ICMP;
  ip->src = swap32(local_ip);
  ip->dst = swap32(dst);
  ip->sum = swap16(checksum(ip, sizeof(*ip)));
  icmp = (struct icmphdr*)(ip + 1);
  icmp->type = ICMP_ECHO_REQUEST;
  icmp->id = swap16(id);
  icmp->seq = swap16(seq);
  if(copyin(myproc()->pagetable, (char*)(icmp + 1), uaddr, len) < 0){
    kfree(packet);
    return -1;
  }
  icmp->sum = swap16(checksum(icmp, sizeof(*icmp) + len));

  acquire(&pingq.lock);
  if(pingq.owner != 0 && pingq.owner != myproc()->pid){
    release(&pingq.lock);
    kfree(packet);
    return -1;
  }
  pingq.owner = myproc()->pid;
  pingq.id = id;
  pingq.ready = 0;
  release(&pingq.lock);
  if(netdev_xmit == 0 || netdev_xmit(packet, framelen) < 0){
    acquire(&pingq.lock);
    if(pingq.owner == myproc()->pid && pingq.id == id)
      pingq.owner = 0;
    release(&pingq.lock);
    kfree(packet);
    return -1;
  }
  kfree(packet);
  return len;
}

int
net_icmp_recv(int id, uint64 srcaddr, uint64 seqaddr, uint64 uaddr, int maxlen)
{
  uint32 src;
  uint16 seq;
  int n;

  if(maxlen < 0)
    return -1;
  acquire(&pingq.lock);
  if(pingq.owner != myproc()->pid || pingq.id != id){
    release(&pingq.lock);
    return -1;
  }
  while(!pingq.ready){
    if(myproc()->killed){
      pingq.owner = 0;
      release(&pingq.lock);
      return -1;
    }
    sleep(&pingq, &pingq.lock);
  }
  src = pingq.src;
  seq = pingq.seq;
  n = pingq.len < maxlen ? pingq.len : maxlen;
  pingq.ready = 0;
  pingq.owner = 0;
  if((srcaddr && copyout(myproc()->pagetable, srcaddr, (char*)&src,
                         sizeof(src)) < 0) ||
     (seqaddr && copyout(myproc()->pagetable, seqaddr, (char*)&seq,
                         sizeof(seq)) < 0) ||
     copyout(myproc()->pagetable, uaddr, (char*)pingq.data, n) < 0){
    release(&pingq.lock);
    return -1;
  }
  release(&pingq.lock);
  return n;
}

int
net_udp_send(uint32 dst, int sport, int dport, uint64 uaddr, int len)
{
  uchar *packet;
  struct ethhdr *eth;
  struct iphdr *ip;
  struct udphdr *udp;
  int framelen;
  uint32 wire_dst = dst;

  if(dst == 0x0a000203U && dns_ip)
    wire_dst = dns_ip;

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
  if(dst == NET_IP_LOOPBACK || dst == local_ip)
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
  ip->src = swap32(local_ip);
  ip->dst = swap32(dst == NET_IP_LOOPBACK ? local_ip : wire_dst);
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

  if(dst == NET_IP_LOOPBACK || dst == local_ip){
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
