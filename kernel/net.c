#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "device.h"
#include "net.h"
#include "workqueue.h"

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
#define NTCP 16
#define TCP_BACKLOG_MAX 16
#define TCP_RX_SIZE 2048
#define TCP_TXQ_SIZE 8192
#define TCP_CLOSED 0
#define TCP_LISTEN 1
#define TCP_SYN_RCVD 2
#define TCP_ESTABLISHED 3
#define TCP_CLOSE_WAIT 4
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_PSH 0x08
#define TCP_ACK 0x10
struct tcp_conn {
  struct spinlock lock;
  int state;
  int owner;
  int accepted;
  int parent;
  int backlog;
  int pending;
  int aq_head;
  int aq_tail;
  int aq_count;
  int acceptq[TCP_BACKLOG_MAX];
  uint16 local_port;
  uint16 remote_port;
  uint32 remote_ip;
  uint32 snd_nxt;
  uint32 rcv_nxt;
  struct net_device *dev;
  uchar remote_mac[ETH_ADDR_LEN];
  int rxlen;
  uchar rx[TCP_RX_SIZE];
  uint32 ooo_seq;
  int ooo_len;
  uchar ooo[TCP_RX_SIZE];
  int tx_unacked;
  uint32 tx_seq;
  uint8 tx_flags;
  int tx_len;
  uchar tx[NET_MTU - 40];
  int txq_len;
  uchar txq[TCP_TXQ_SIZE];
  uint64 tx_deadline;
  uint8 tx_retries;
  int tx_failed;
};
static struct tcp_conn tcp_connections[NTCP];
static struct workqueue net_wq;
static struct delayed_work tcp_retransmit_work;
static void net_tcp_workfn(struct work_struct*);
static struct tcp_conn *tcp_handle(int);
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
#define NETDEV_MAX 4
#define ROUTE_MAX  16
#define ARP_MAX    32
#define ARP_REACHABLE 1
static struct net_device *netdevices[NETDEV_MAX];
static int nnetdev;

static void
tcp_epollnotify(struct tcp_conn *c)
{
  epollnotify_socket((int)(c - tcp_connections) + 1);
}
static struct net_device *active_netdev;
static struct route routes[ROUTE_MAX];
static struct arp_entry arp_cache[ARP_MAX];
static struct bus_type net_bus = {
  .name = "net",
};
static uint16 ip_id;
static struct {
  struct net_device *dev;
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

static void
napi_workfn(struct work_struct *work)
{
  struct napi_struct *napi = (struct napi_struct*)
    ((char*)work - __builtin_offsetof(struct napi_struct, work));
  int done;

  acquire(&napi->lock);
  if(!napi->enabled || !napi->scheduled){
    release(&napi->lock);
    return;
  }
  napi->polls++;
  release(&napi->lock);

  done = napi->poll(napi, napi->weight);
  if(done >= napi->weight){
    acquire(&napi->lock);
    napi->budget_exhausted++;
    if(napi->enabled && napi->scheduled)
      queue_work(napi->wq, &napi->work);
    release(&napi->lock);
  }
}

void
netif_napi_add(struct net_device *dev, struct napi_struct *napi,
               struct workqueue *wq, napi_poll_fn poll, int weight)
{
  memset(napi, 0, sizeof(*napi));
  initlock(&napi->lock, "napi");
  init_work(&napi->work, napi_workfn);
  napi->dev = dev;
  napi->wq = wq;
  napi->poll = poll;
  napi->weight = weight > 0 ? weight : 1;
}

void
napi_enable(struct napi_struct *napi)
{
  acquire(&napi->lock);
  napi->enabled = 1;
  napi->scheduled = 0;
  napi->missed = 0;
  release(&napi->lock);
}

void
napi_disable(struct napi_struct *napi)
{
  acquire(&napi->lock);
  napi->enabled = 0;
  napi->scheduled = 0;
  napi->missed = 0;
  release(&napi->lock);
  cancel_work_sync(&napi->work);
}

int
napi_schedule(struct napi_struct *napi)
{
  int schedule = 0;
  acquire(&napi->lock);
  if(napi->enabled && !napi->scheduled){
    napi->scheduled = 1;
    schedule = 1;
  } else if(napi->enabled) {
    // Preserve an IRQ that races with an already scheduled/running poll.
    napi->missed = 1;
  }
  release(&napi->lock);
  if(schedule)
    queue_work(napi->wq, &napi->work);
  return schedule;
}

int
napi_complete_done(struct napi_struct *napi, int work_done)
{
  int complete = 0, reschedule = 0;
  acquire(&napi->lock);
  if(napi->scheduled && work_done < napi->weight){
    if(napi->missed){
      napi->missed = 0;
      reschedule = 1;
    } else {
      napi->scheduled = 0;
      napi->complete++;
      complete = 1;
    }
  }
  if(reschedule)
    queue_work(napi->wq, &napi->work);
  release(&napi->lock);
  return complete;
}

static int
netdev_xmit_on(struct net_device *dev, void *frame, int length)
{
  if(dev == 0 || !dev->running || dev->ops == 0 ||
     dev->ops->start_xmit == 0)
    return -1;
  return dev->ops->start_xmit(dev, frame, length);
}

void
netdev_poll_one(struct net_device *dev)
{
  if(dev && dev->running && dev->ops && dev->ops->poll)
    dev->ops->poll(dev);
}

static void
route_remove_dev(struct net_device *dev)
{
  for(int i = 0; i < ROUTE_MAX; i++)
    if(routes[i].valid && routes[i].dev == dev)
      routes[i].valid = 0;
}

static int
route_add(uint32 destination, uint32 mask, uint32 gateway, uint32 metric,
          struct net_device *dev)
{
  for(int i = 0; i < ROUTE_MAX; i++){
    if(!routes[i].valid){
      routes[i].destination = destination & mask;
      routes[i].netmask = mask;
      routes[i].gateway = gateway;
      routes[i].metric = metric;
      routes[i].dev = dev;
      routes[i].valid = 1;
      return 0;
    }
  }
  return -1;
}

static int
mask_bits(uint32 mask)
{
  int bits = 0;
  while(mask){ bits += mask & 1; mask >>= 1; }
  return bits;
}

static struct route*
route_lookup(uint32 destination)
{
  struct route *best = 0;
  int bestbits = -1;
  for(int i = 0; i < ROUTE_MAX; i++){
    int bits;
    if(!routes[i].valid || routes[i].dev == 0 || !routes[i].dev->running ||
       (destination & routes[i].netmask) != routes[i].destination)
      continue;
    bits = mask_bits(routes[i].netmask);
    if(best == 0 || bits > bestbits ||
       (bits == bestbits && routes[i].metric < best->metric)){
      best = &routes[i];
      bestbits = bits;
    }
  }
  return best;
}

static struct net_device*
netdev_by_ip(uint32 ip)
{
  for(int i = 0; i < nnetdev; i++)
    if(netdevices[i]->running && netdevices[i]->ip_addr == ip)
      return netdevices[i];
  return 0;
}

static uint32
netdev_metric(struct net_device *dev)
{
  for(int i = 0; i < nnetdev; i++)
    if(netdevices[i] == dev)
      return i;
  return NETDEV_MAX;
}

static struct arp_entry*
arp_lookup(uint32 ip, struct net_device *dev)
{
  for(int i = 0; i < ARP_MAX; i++)
    if(arp_cache[i].valid && arp_cache[i].ip == ip &&
       arp_cache[i].dev == dev)
      return &arp_cache[i];
  return 0;
}

static void
arp_update(uint32 ip, uchar *mac, struct net_device *dev)
{
  struct arp_entry *entry = arp_lookup(ip, dev);
  if(entry == 0){
    for(int i = 0; i < ARP_MAX; i++)
      if(!arp_cache[i].valid){ entry = &arp_cache[i]; break; }
  }
  if(entry == 0)
    entry = &arp_cache[0];
  entry->ip = ip;
  memmove(entry->mac, mac, ETH_ADDR_LEN);
  entry->dev = dev;
  entry->updated = r_cntvct_el0();
  entry->state = ARP_REACHABLE;
  entry->valid = 1;
}

static void
arp_remove_dev(struct net_device *dev)
{
  for(int i = 0; i < ARP_MAX; i++)
    if(arp_cache[i].valid && arp_cache[i].dev == dev)
      arp_cache[i].valid = 0;
}

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

static uint32
checksum_add(uint32 sum, void *data, int len)
{
  uchar *p = data;
  while(len > 1){
    sum += ((uint16)p[0] << 8) | p[1];
    p += 2;
    len -= 2;
  }
  if(len)
    sum += (uint16)p[0] << 8;
  return sum;
}

static uint16
tcp_checksum(uint32 src, uint32 dst, void *segment, int len)
{
  uint32 sum = 0;
  uchar tail[4];
  sum = checksum_add(sum, &src, 4);
  sum = checksum_add(sum, &dst, 4);
  tail[0] = 0;
  tail[1] = IP_PROTO_TCP;
  tail[2] = len >> 8;
  tail[3] = len;
  sum = checksum_add(sum, tail, sizeof(tail));
  sum = checksum_add(sum, segment, len);
  while(sum >> 16)
    sum = (sum & 0xffff) + (sum >> 16);
  return (uint16)~sum;
}

static int
tcp_send_at_locked(struct tcp_conn *c, uint32 sequence, uint8 flags,
                   void *data, int len)
{
  uchar *packet;
  struct ethhdr *eth;
  struct iphdr *ip;
  struct tcphdr *tcp;
  int framelen;
  if(c->dev == 0 || len < 0 || len > NET_MTU - 40)
    return -1;
  framelen = sizeof(*eth) + sizeof(*ip) + sizeof(*tcp) + len;
  packet = kalloc();
  if(packet == 0)
    return -1;
  memset(packet, 0, framelen);
  eth = (struct ethhdr*)packet;
  memmove(eth->dst, c->remote_mac, ETH_ADDR_LEN);
  memmove(eth->src, c->dev->mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  ip->vhl = 0x45;
  ip->len = swap16(sizeof(*ip) + sizeof(*tcp) + len);
  ip->id = swap16(++ip_id);
  ip->ttl = 64;
  ip->proto = IP_PROTO_TCP;
  ip->src = swap32(c->dev->ip_addr);
  ip->dst = swap32(c->remote_ip);
  ip->sum = swap16(checksum(ip, sizeof(*ip)));
  tcp = (struct tcphdr*)(ip + 1);
  tcp->sport = swap16(c->local_port);
  tcp->dport = swap16(c->remote_port);
  tcp->seq = swap32(sequence);
  tcp->ack = swap32(c->rcv_nxt);
  tcp->offset = 5 << 4;
  tcp->flags = flags;
  tcp->window = swap16(TCP_RX_SIZE - c->rxlen);
  if(len)
    memmove(tcp + 1, data, len);
  tcp->sum = swap16(tcp_checksum(ip->src, ip->dst, tcp,
                                  sizeof(*tcp) + len));
  int result = netdev_xmit_on(c->dev, packet, framelen);
  kfree(packet);
  return result < 0 ? -1 : len;
}

static int
tcp_send_locked(struct tcp_conn *c, uint8 flags, void *data, int len)
{
  return tcp_send_at_locked(c, c->snd_nxt, flags, data, len);
}

static int
tcp_track_send_locked(struct tcp_conn *c, uint8 flags, void *data, int len)
{
  int sequence_bytes = len + ((flags & TCP_SYN) != 0) +
                       ((flags & TCP_FIN) != 0);
  if(c->tx_unacked || len > (int)sizeof(c->tx))
    return -1;
  c->tx_seq = c->snd_nxt;
  c->tx_flags = flags;
  c->tx_len = len;
  if(len)
    memmove(c->tx, data, len);
  if(tcp_send_at_locked(c, c->tx_seq, flags, c->tx, len) < 0)
    return -1;
  c->snd_nxt += sequence_bytes;
  c->tx_unacked = sequence_bytes != 0;
  c->tx_retries = 0;
  c->tx_failed = 0;
  c->tx_deadline = workqueue_now() + 50; // 500 ms in 10-ms jiffies.
  if(c->tx_unacked)
    queue_delayed_work(&net_wq, &tcp_retransmit_work, 50);
  return len;
}

// Keep one segment in the retransmission slot, while allowing write(2) to
// queue following bytes without waiting for a round-trip ACK.  Interactive
// SSH otherwise pays a full RTT for every echoed character.
static void
tcp_start_queued_locked(struct tcp_conn *c)
{
  int n;

  if(c->tx_unacked || c->tx_failed || c->txq_len == 0 ||
     (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT))
    return;
  n = c->txq_len;
  if(n > (int)sizeof(c->tx))
    n = sizeof(c->tx);
  if(tcp_track_send_locked(c, TCP_ACK | TCP_PSH, c->txq, n) < 0){
    c->tx_failed = 1;
    wakeup(c);
    tcp_epollnotify(c);
    return;
  }
  memmove(c->txq, c->txq + n, c->txq_len - n);
  c->txq_len -= n;
  wakeup(c);
  tcp_epollnotify(c);
}

static void
tcp_rx(struct net_device *dev, struct ethhdr *eth, struct iphdr *ip,
       int hlen, int iplen)
{
  struct tcphdr *tcp = (struct tcphdr*)((uchar*)ip + hlen);
  int tcplen = iplen - hlen;
  int thlen, datalen;
  uint16 dport, sport;
  uint32 seq, ack;
  struct tcp_conn *c = 0;
  if(dev == 0 || tcplen < (int)sizeof(*tcp) ||
     tcp_checksum(ip->src, ip->dst, tcp, tcplen) != 0)
    return;
  thlen = (tcp->offset >> 4) * 4;
  if(thlen < 20 || thlen > tcplen)
    return;
  datalen = tcplen - thlen;
  dport = swap16(tcp->dport);
  sport = swap16(tcp->sport);
  seq = swap32(tcp->seq);
  ack = swap32(tcp->ack);

  // Prefer an existing four-tuple over a listener on the same local port.
  // A listener remains present after accept(), so selecting it first would
  // misdirect data for an established child connection.
  for(int i = 0; i < NTCP; i++){
    acquire(&tcp_connections[i].lock);
    if(tcp_connections[i].state >= TCP_SYN_RCVD &&
       tcp_connections[i].local_port == dport &&
       tcp_connections[i].remote_port == sport &&
       tcp_connections[i].remote_ip == swap32(ip->src)){
      c = &tcp_connections[i];
      break;
    }
    release(&tcp_connections[i].lock);
  }
  if(c == 0){
    for(int i = 0; i < NTCP; i++){
      acquire(&tcp_connections[i].lock);
      if(tcp_connections[i].state == TCP_LISTEN &&
         tcp_connections[i].local_port == dport){
        c = &tcp_connections[i];
        break;
      }
      release(&tcp_connections[i].lock);
    }
  }
  if(c == 0)
    return;

  if(c->state == TCP_LISTEN){
    if(!(tcp->flags & TCP_SYN)){
      release(&c->lock);
      return;
    }
    int parent = (int)(c - tcp_connections) + 1;
    struct tcp_conn *child = 0;
    for(int i = 0; i < NTCP; i++){
      if(i == parent - 1)
        continue;
      acquire(&tcp_connections[i].lock);
      if(tcp_connections[i].state == TCP_CLOSED){
        child = &tcp_connections[i];
        break;
      }
      release(&tcp_connections[i].lock);
    }
    if(child == 0 || c->pending >= c->backlog){
      if(child)
        release(&child->lock);
      release(&c->lock);
      return;
    }
    memset((uchar*)child + sizeof(struct spinlock), 0,
           sizeof(struct tcp_conn) - sizeof(struct spinlock));
    child->state = TCP_SYN_RCVD;
    child->owner = c->owner;
    child->parent = parent;
    child->local_port = dport;
    child->remote_ip = swap32(ip->src);
    child->remote_port = sport;
    child->dev = dev;
    memmove(child->remote_mac, eth->src, ETH_ADDR_LEN);
    child->rcv_nxt = seq + 1;
    child->snd_nxt = ((uint32)r_cntvct_el0() ^ child->remote_ip) | 1;
    c->pending++;
    tcp_track_send_locked(child, TCP_SYN | TCP_ACK, 0, 0);
    release(&child->lock);
    release(&c->lock);
    return;
  }
  if(c->state == TCP_SYN_RCVD && (tcp->flags & TCP_ACK) &&
     ack == c->snd_nxt){
    c->tx_unacked = 0;
    c->state = TCP_ESTABLISHED;
    int parent = c->parent;
    release(&c->lock);
    struct tcp_conn *listener = tcp_handle(parent);
    if(listener){
      acquire(&listener->lock);
      if(listener->state == TCP_LISTEN &&
         listener->aq_count < listener->backlog){
        listener->acceptq[listener->aq_tail] = (int)(c - tcp_connections) + 1;
        listener->aq_tail = (listener->aq_tail + 1) % TCP_BACKLOG_MAX;
        listener->aq_count++;
        wakeup(listener);
        tcp_epollnotify(listener);
      }
      release(&listener->lock);
    }
    return;
  }
  if((tcp->flags & TCP_ACK) && c->tx_unacked && ack >= c->snd_nxt){
    c->tx_unacked = 0;
    c->tx_retries = 0;
    tcp_start_queued_locked(c);
    wakeup(c);
    tcp_epollnotify(c);
  }
  if((c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
     datalen > 0){
    if(seq == c->rcv_nxt){
      int room = TCP_RX_SIZE - c->rxlen;
      int n = datalen < room ? datalen : room;
      if(n > 0){
        memmove(c->rx + c->rxlen, (uchar*)tcp + thlen, n);
        c->rxlen += n;
        c->rcv_nxt += n;
        // One held out-of-order segment is enough to bridge the common case
        // where two adjacent Wi-Fi frames arrive in reverse poll order.
        if(c->ooo_len && c->ooo_seq == c->rcv_nxt &&
           c->ooo_len <= TCP_RX_SIZE - c->rxlen){
          memmove(c->rx + c->rxlen, c->ooo, c->ooo_len);
          c->rxlen += c->ooo_len;
          c->rcv_nxt += c->ooo_len;
          c->ooo_len = 0;
        }
        wakeup(c);
        tcp_epollnotify(c);
      }
    } else if(seq > c->rcv_nxt && datalen <= (int)sizeof(c->ooo) &&
              (c->ooo_len == 0 || seq < c->ooo_seq)){
      c->ooo_seq = seq;
      c->ooo_len = datalen;
      memmove(c->ooo, (uchar*)tcp + thlen, datalen);
    }
    // ACK current rcv_nxt for new, duplicate and out-of-order segments.
    tcp_send_locked(c, TCP_ACK, 0, 0);
  }
  if((tcp->flags & TCP_FIN) && seq + datalen == c->rcv_nxt){
    c->rcv_nxt++;
    c->state = TCP_CLOSE_WAIT;
    tcp_send_locked(c, TCP_ACK, 0, 0);
    wakeup(c);
    tcp_epollnotify(c);
  }
  release(&c->lock);
}

void
netinit(void)
{
  int i;
  init_workqueue(&net_wq, "net_wq", 1);
  init_delayed_work(&tcp_retransmit_work, net_tcp_workfn);
  initlock(&porttable_lock, "udp ports");
  initlock(&pingq.lock, "icmp reply");
  for(i = 0; i < NUDPPORT; i++)
    initlock(&ports[i].lock, "udp port");
  for(i = 0; i < NTCP; i++)
    initlock(&tcp_connections[i].lock, "tcp conn");
  memset(netdevices, 0, sizeof(netdevices));
  memset(routes, 0, sizeof(routes));
  memset(arp_cache, 0, sizeof(arp_cache));
  nnetdev = 0;
  active_netdev = 0;
  bus_register(&net_bus);
}

struct net_device *
netdev_default(void)
{
  return active_netdev;
}

struct net_device *
netdev_find(char *name)
{
  if(name == 0)
    return 0;
  for(int i = 0; i < nnetdev; i++)
    if(strncmp(netdevices[i]->dev.name, name, strlen(name) + 1) == 0)
      return netdevices[i];
  return 0;
}

void
netdev_poll_all(void)
{
  for(int i = 0; i < nnetdev; i++){
    struct net_device *dev = netdevices[i];
    if(dev && dev->running && dev->ops && dev->ops->poll)
      dev->ops->poll(dev);
  }
}

int
register_netdev(struct net_device *dev)
{
  if(dev == 0 || dev->dev.name == 0 || dev->ops == 0 ||
     dev->ops->start_xmit == 0 || nnetdev >= NETDEV_MAX)
    return -1;
  if(netdev_find(dev->dev.name))
    return -1;
  if(dev->mtu == 0)
    dev->mtu = NET_MTU;
  dev->dev.bus = &net_bus;
  dev->dev.driver_data = dev->priv;
  if(device_register(&dev->dev) < 0)
    return -1;
  if(dev->ops->open && dev->ops->open(dev) < 0){
    device_unregister(&dev->dev);
    return -1;
  }
  dev->running = 1;
  netdevices[nnetdev++] = dev;
  // Keep a default interface for legacy callers.  Actual packet output uses
  // the routing table once DHCP has configured an interface.
  active_netdev = dev;
  dev->ip_addr = dev->netmask = dev->gateway = dev->dns = 0;
  return 0;
}

int
unregister_netdev(struct net_device *dev)
{
  int i;
  if(dev == 0)
    return -1;
  for(i = 0; i < nnetdev; i++)
    if(netdevices[i] == dev)
      break;
  if(i == nnetdev)
    return -1;
  dev->running = 0;
  if(dev->ops && dev->ops->stop)
    dev->ops->stop(dev);
  for(; i + 1 < nnetdev; i++)
    netdevices[i] = netdevices[i + 1];
  netdevices[--nnetdev] = 0;
  if(active_netdev == dev)
    active_netdev = nnetdev ? netdevices[nnetdev - 1] : 0;
  route_remove_dev(dev);
  arp_remove_dev(dev);
  device_unregister(&dev->dev);
  return 0;
}

static void
icmp_rx(struct net_device *dev, struct ethhdr *eth, struct iphdr *ip,
        int hlen, int iplen)
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

  if(dev == 0 || icmp->type != ICMP_ECHO_REQUEST ||
     swap32(ip->dst) != dev->ip_addr)
    return;
  framelen = sizeof(*reth) + sizeof(*rip) + icmplen;
  packet = kalloc();
  if(packet == 0)
    return;
  reth = (struct ethhdr*)packet;
  memmove(reth->dst, eth->src, ETH_ADDR_LEN);
  memmove(reth->src, dev->mac, ETH_ADDR_LEN);
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
  netdev_xmit_on(dev, packet, framelen);
  kfree(packet);
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
    if(!p->used && p->waiters == 0){
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

void
net_udp_closeproc(int pid)
{
  struct udp_dgram *d, *next;

  acquire(&porttable_lock);
  for(int i = 0; i < NUDPPORT; i++){
    struct udp_port *p = &ports[i];
    acquire(&p->lock);
    if(p->used && p->owner == pid){
      for(d = p->head; d; d = next){
        next = d->next;
        kfree(d);
      }
      p->used = 0;
      p->head = p->tail = 0;
      p->queued = 0;
      wakeup(p);
    }
    release(&p->lock);
  }
  release(&porttable_lock);
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
dhcp_rx(struct net_device *dev, struct ethhdr *eth, uchar *data, int len)
{
  int pos, end, type = 0;
  uint32 xid;

  if(!dhcp.waiting || dhcp.dev != dev || len < 240 || data[0] != 2 ||
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
arp_rx(struct net_device *dev, struct ethhdr *eth, int len)
{
  struct arphdr *arp, *reply;
  struct ethhdr *reth;
  uchar *packet;
  int framelen = sizeof(*reth) + sizeof(*reply);

  if(len < framelen || dev == 0)
    return;
  arp = (struct arphdr*)(eth + 1);
  if(swap16(arp->htype) != ARP_HTYPE_ETH ||
     swap16(arp->ptype) != ETH_TYPE_IP || arp->hlen != ETH_ADDR_LEN ||
     arp->plen != 4)
    return;
  arp_update(swap32(arp->spa), arp->sha, dev);
  if(swap16(arp->op) == ARP_OP_REPLY)
    return;
  if(swap16(arp->op) != ARP_OP_REQUEST ||
     swap32(arp->tpa) != dev->ip_addr)
    return;

  packet = kalloc();
  if(packet == 0)
    return;
  memset(packet, 0, framelen);
  reth = (struct ethhdr*)packet;
  memmove(reth->dst, arp->sha, ETH_ADDR_LEN);
  memmove(reth->src, dev->mac, ETH_ADDR_LEN);
  reth->type = swap16(ETH_TYPE_ARP);
  reply = (struct arphdr*)(reth + 1);
  reply->htype = swap16(ARP_HTYPE_ETH);
  reply->ptype = swap16(ETH_TYPE_IP);
  reply->hlen = ETH_ADDR_LEN;
  reply->plen = 4;
  reply->op = swap16(ARP_OP_REPLY);
  memmove(reply->sha, dev->mac, ETH_ADDR_LEN);
  reply->spa = swap32(dev->ip_addr);
  memmove(reply->tha, arp->sha, ETH_ADDR_LEN);
  reply->tpa = arp->spa;
  netdev_xmit_on(dev, packet, framelen);
  kfree(packet);
}

static int
arp_request(struct net_device *dev, uint32 target)
{
  uchar packet[sizeof(struct ethhdr) + sizeof(struct arphdr)];
  struct ethhdr *eth = (struct ethhdr*)packet;
  struct arphdr *arp = (struct arphdr*)(eth + 1);
  memset(packet, 0, sizeof(packet));
  memset(eth->dst, 0xff, ETH_ADDR_LEN);
  if(dev == 0)
    return -1;
  memmove(eth->src, dev->mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_ARP);
  arp->htype = swap16(ARP_HTYPE_ETH);
  arp->ptype = swap16(ETH_TYPE_IP);
  arp->hlen = ETH_ADDR_LEN; arp->plen = 4;
  arp->op = swap16(ARP_OP_REQUEST);
  memmove(arp->sha, dev->mac, ETH_ADDR_LEN);
  arp->spa = swap32(dev->ip_addr);
  arp->tpa = swap32(target);
  return netdev_xmit_on(dev, packet, sizeof(packet));
}

void
net_rx_dev(struct net_device *dev, void *packet, int len)
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
    arp_rx(dev, eth, len);
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
    icmp_rx(dev, eth, ip, hlen, iplen);
    return;
  }
  if(ip->proto == IP_PROTO_TCP){
    tcp_rx(dev, eth, ip, hlen, iplen);
    return;
  }
  if(ip->proto != IP_PROTO_UDP)
    return;
  udp = (struct udphdr*)((uchar*)ip + hlen);
  udplen = swap16(udp->len);
  if(udplen < (int)sizeof(*udp) || udplen > iplen - hlen)
    return;
  if(swap16(udp->sport) == 67 && swap16(udp->dport) == 68)
    dhcp_rx(dev, eth, (uchar*)(udp + 1), udplen - sizeof(*udp));
  uint32 udp_src = swap32(ip->src);
  // Preserve the original QEMU-facing user ABI: programs historically use
  // 10.0.2.3 as the DNS endpoint.  On a DHCP-configured interface translate
  // that stable alias to/from the real resolver learned from option 6.
  if(dev && dev->dns && dev->dns != 0x0a000203U && udp_src == dev->dns &&
     swap16(udp->sport) == 53)
    udp_src = 0x0a000203U;
  udp_rx(udp_src, swap16(udp->sport), swap16(udp->dport),
         (uchar*)(udp + 1), udplen - sizeof(*udp));
}

void
net_rx(void *packet, int len)
{
  net_rx_dev(active_netdev, packet, len);
}

static struct tcp_conn*
tcp_handle(int handle)
{
  if(handle <= 0 || handle > NTCP)
    return 0;
  return &tcp_connections[handle - 1];
}

int
net_tcp_listen(int port, int backlog)
{
  if(port <= 0 || port > 65535)
    return -1;
  if(backlog < 1)
    backlog = 1;
  if(backlog > TCP_BACKLOG_MAX)
    backlog = TCP_BACKLOG_MAX;
  for(int i = 0; i < NTCP; i++){
    acquire(&tcp_connections[i].lock);
    if(tcp_connections[i].state == TCP_CLOSED){
      memset((uchar*)&tcp_connections[i] + sizeof(struct spinlock), 0,
             sizeof(struct tcp_conn) - sizeof(struct spinlock));
      tcp_connections[i].state = TCP_LISTEN;
      tcp_connections[i].owner = myproc()->pid;
      tcp_connections[i].local_port = port;
      tcp_connections[i].backlog = backlog;
      release(&tcp_connections[i].lock);
      printf("tcp: listening on %d handle=%d\n", port, i + 1);
      return i + 1;
    }
    release(&tcp_connections[i].lock);
  }
  return -1;
}

int
net_tcp_accept(int handle, int nonblock)
{
  struct tcp_conn *listener = tcp_handle(handle);
  struct tcp_conn *child;
  int child_handle;
  if(listener == 0)
    return -1;
  acquire(&listener->lock);
  if(listener->owner != myproc()->pid || listener->state == TCP_CLOSED ||
     listener->accepted){
    release(&listener->lock);
    return -1;
  }
  if(listener->state != TCP_LISTEN){
    release(&listener->lock);
    return -1;
  }
  while(listener->aq_count == 0){
    if(nonblock){
      release(&listener->lock);
      return -1;
    }
    if(myproc()->killed){
      release(&listener->lock);
      return -1;
    }
    sleep(listener, &listener->lock);
  }

  child_handle = listener->acceptq[listener->aq_head];
  listener->aq_head = (listener->aq_head + 1) % TCP_BACKLOG_MAX;
  listener->aq_count--;
  if(listener->pending > 0)
    listener->pending--;
  release(&listener->lock);
  tcp_epollnotify(listener);
  child = tcp_handle(child_handle);
  if(child == 0)
    return -1;
  acquire(&child->lock);
  if(child->state != TCP_ESTABLISHED || child->parent != handle){
    release(&child->lock);
    return -1;
  }
  child->accepted = 1;
  release(&child->lock);
  return child_handle;
}

int
net_tcp_poll(int handle, int events)
{
  struct tcp_conn *c = tcp_handle(handle);
  int ready = 0;
  if(c == 0)
    return 0;
  acquire(&c->lock);
  if(c->state == TCP_LISTEN){
    if((events & 1) && c->aq_count)
      ready |= 1;
  } else if(c->accepted){
    if((events & 1) && (c->rxlen || c->state == TCP_CLOSE_WAIT))
      ready |= 1;
    if((events & 4) &&
       (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) &&
       c->txq_len < TCP_TXQ_SIZE)
      ready |= 4;
  }
  if(c->tx_failed || c->state == TCP_CLOSED)
    ready |= 8;
  release(&c->lock);
  return ready;
}

int
net_tcp_read(int handle, uint64 uaddr, int maxlen, int nonblock)
{
  struct tcp_conn *c = tcp_handle(handle);
  int n;
  if(c == 0 || maxlen < 0)
    return -1;
  acquire(&c->lock);
  if(c->owner != myproc()->pid || !c->accepted){
    release(&c->lock);
    return -1;
  }
  while(c->rxlen == 0 && c->state == TCP_ESTABLISHED){
    if(nonblock){
      release(&c->lock);
      return -1;
    }
    if(myproc()->killed){
      release(&c->lock);
      return -1;
    }
    sleep(c, &c->lock);
  }
  if(c->rxlen == 0 && c->state == TCP_CLOSE_WAIT){
    release(&c->lock);
    return 0;
  }
  n = c->rxlen < maxlen ? c->rxlen : maxlen;
  if(copyout(myproc()->pagetable, uaddr, (char*)c->rx, n) < 0){
    release(&c->lock);
    return -1;
  }
  memmove(c->rx, c->rx + n, c->rxlen - n);
  c->rxlen -= n;
  release(&c->lock);
  tcp_epollnotify(c);
  return n;
}

int
net_tcp_write(int handle, uint64 uaddr, int len, int nonblock)
{
  struct tcp_conn *c = tcp_handle(handle);
  int done = 0;
  if(c == 0 || len < 0)
    return -1;
  while(done < len){
    acquire(&c->lock);
    if(c->owner != myproc()->pid || !c->accepted ||
       (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT)){
      release(&c->lock);
      return done ? done : -1;
    }
    while(c->txq_len == TCP_TXQ_SIZE && !c->tx_failed){
      if(nonblock){
        release(&c->lock);
        return done ? done : -1;
      }
      if(myproc()->killed){
        release(&c->lock);
        return -1;
      }
      sleep(c, &c->lock);
    }
    if(c->tx_failed){
      release(&c->lock);
      return done ? done : -1;
    }
    int n = len - done;
    int room = TCP_TXQ_SIZE - c->txq_len;
    if(n > room)
      n = room;
    if(copyin(myproc()->pagetable, (char*)c->txq + c->txq_len,
              uaddr + done, n) < 0){
      release(&c->lock);
      return done ? done : -1;
    }
    c->txq_len += n;
    done += n;
    tcp_start_queued_locked(c);
    release(&c->lock);
    tcp_epollnotify(c);
    if(nonblock)
      break;
  }
  return done;
}

int
net_tcp_close(int handle)
{
  struct tcp_conn *c = tcp_handle(handle);
  int was_listener;
  if(c == 0)
    return -1;
  acquire(&c->lock);
  if(c->owner != myproc()->pid){
    release(&c->lock);
    return -1;
  }
  was_listener = c->state == TCP_LISTEN;
  if(c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT){
    while((c->tx_unacked || c->txq_len) && !c->tx_failed)
      sleep(c, &c->lock);
    if(!c->tx_failed && tcp_track_send_locked(c, TCP_FIN | TCP_ACK, 0, 0) >= 0)
      while(c->tx_unacked && !c->tx_failed)
        sleep(c, &c->lock);
  }
  c->state = TCP_CLOSED;
  c->owner = 0;
  c->accepted = 0;
  c->rxlen = 0;
  wakeup(c);
  tcp_epollnotify(c);
  release(&c->lock);
  // Closing a listener drops only queued/half-open children.  Connections
  // already returned by accept() remain independent, as they do on Linux.
  if(was_listener){
    for(int i = 0; i < NTCP; i++){
      struct tcp_conn *child = &tcp_connections[i];
      acquire(&child->lock);
      if(child->parent == handle && !child->accepted){
        child->state = TCP_CLOSED;
        child->owner = 0;
        child->parent = 0;
        child->tx_unacked = 0;
        child->txq_len = 0;
        wakeup(child);
      }
      release(&child->lock);
    }
  }
  return 0;
}

void
net_tcp_tick(void)
{
  uint64 now = workqueue_now();
  uint64 next = 0;
  for(int i = 0; i < NTCP; i++){
    struct tcp_conn *c = &tcp_connections[i];
    acquire(&c->lock);
    if(c->tx_unacked && (long)(now - c->tx_deadline) >= 0){
      if(c->tx_retries >= 5){
        int parent = c->state == TCP_SYN_RCVD ? c->parent : 0;
        c->tx_unacked = 0;
        c->txq_len = 0;
        c->tx_failed = 1;
        printf("tcp: retransmission timeout port=%d\n", c->local_port);
        wakeup(c);
        tcp_epollnotify(c);
        if(parent){
          c->state = TCP_CLOSED;
          c->owner = 0;
          c->parent = 0;
          release(&c->lock);
          struct tcp_conn *listener = tcp_handle(parent);
          if(listener){
            acquire(&listener->lock);
            if(listener->pending > 0)
              listener->pending--;
            release(&listener->lock);
          }
          continue;
        }
      } else {
        c->tx_retries++;
        tcp_send_at_locked(c, c->tx_seq, c->tx_flags,
                           c->tx, c->tx_len);
        c->tx_deadline = now + 50;
      }
    }
    if(c->tx_unacked && (next == 0 ||
       (long)(c->tx_deadline - next) < 0))
      next = c->tx_deadline;
    release(&c->lock);
  }
  if(next)
    mod_delayed_work(&net_wq, &tcp_retransmit_work,
                     (long)(next - now) > 0 ? next - now : 1);
}

static void
net_tcp_workfn(struct work_struct *work)
{
  (void)work;
  net_tcp_tick();
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
  if(dhcp.dev == 0){
    kfree(packet);
    return -1;
  }
  memmove(eth->src, dhcp.dev->mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  udp = (struct udphdr*)(ip + 1);
  bootp = (uchar*)(udp + 1);
  bootp[0] = 1; bootp[1] = 1; bootp[2] = 6;
  bootp[4] = dhcp.xid >> 24; bootp[5] = dhcp.xid >> 16;
  bootp[6] = dhcp.xid >> 8; bootp[7] = dhcp.xid;
  bootp[10] = 0x80; // broadcast reply flag, network byte order
  memmove(bootp + 28, dhcp.dev->mac, ETH_ADDR_LEN);
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
  int result = netdev_xmit_on(dhcp.dev, packet, framelen);
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
    netdev_poll_one(dhcp.dev);
    asm volatile("yield" ::: "memory");
  }
  return -1;
}

int
net_dhcp_dev(struct net_device *dev)
{
  uint32 offered, server;
  struct arp_entry *gateway_entry;
  int attempt;
  if(dev == 0 || dev->ops == 0 || dev->ops->poll == 0)
    return -1;
  memset(&dhcp, 0, sizeof(dhcp));
  dhcp.dev = dev;
  dhcp.xid = (uint32)r_cntvct_el0() ^
             ((uint32)dev->mac[2] << 24) ^
             ((uint32)dev->mac[5] << 8);
  dhcp.waiting = 1;
  for(attempt = 1; attempt <= 3; attempt++){
    dhcp.type = 0;
    if(dhcp_send(1, 0, 0) >= 0 && dhcp_wait(2, 4) == 0)
      break;
    printf("dhcp: discover retry %d dev=%s\n", attempt,
           dev->dev.name);
  }
  if(attempt > 3){
    dhcp.waiting = 0;
    printf("dhcp: no offer\n");
    return -1;
  }
  offered = dhcp.yiaddr;
  server = dhcp.server;
  for(attempt = 1; attempt <= 3; attempt++){
    dhcp.type = 0;
    if(dhcp_send(3, offered, server) >= 0 && dhcp_wait(5, 4) == 0)
      break;
    printf("dhcp: request retry %d dev=%s\n", attempt,
           dev->dev.name);
  }
  if(attempt > 3){
    dhcp.waiting = 0;
    printf("dhcp: no acknowledgement\n");
    return -1;
  }
  dev->ip_addr = dhcp.yiaddr;
  dev->netmask = dhcp.mask ? dhcp.mask : 0xffffff00U;
  dev->gateway = dhcp.router ? dhcp.router : dhcp.server;
  dev->dns = dhcp.dns ? dhcp.dns : dev->gateway;
  route_remove_dev(dev);
  route_add(dev->ip_addr & dev->netmask, dev->netmask, 0, 0, dev);
  route_add(0, 0, dev->gateway, netdev_metric(dev), dev);
  arp_update(dhcp.server, dhcp.server_mac, dev);
  if(dev->gateway && dev->gateway != dhcp.server)
    arp_update(dev->gateway, dhcp.server_mac, dev);
  dhcp.waiting = 0;
  printf("dhcp: dev=%s address=%d.%d.%d.%d gateway=%d.%d.%d.%d dns=%d.%d.%d.%d\n",
         dev->dev.name,
         (dev->ip_addr>>24)&255, (dev->ip_addr>>16)&255,
         (dev->ip_addr>>8)&255, dev->ip_addr&255,
         (dev->gateway>>24)&255, (dev->gateway>>16)&255,
         (dev->gateway>>8)&255, dev->gateway&255,
         (dev->dns>>24)&255, (dev->dns>>16)&255,
         (dev->dns>>8)&255, dev->dns&255);
  if(dev->gateway && arp_request(dev, dev->gateway) >= 0){
    uint64 deadline = r_cntvct_el0() + (uint64)r_cntfrq_el0() * 2;
    while(arp_lookup(dev->gateway, dev) == 0 && r_cntvct_el0() < deadline){
      netdev_poll_one(dev);
      asm volatile("yield" ::: "memory");
    }
  }
  gateway_entry = arp_lookup(dev->gateway, dev);
  if(gateway_entry)
    printf("arp: dev=%s gateway %d.%d.%d.%d is %x:%x:%x:%x:%x:%x\n",
           dev->dev.name, (dev->gateway>>24)&255, (dev->gateway>>16)&255,
           (dev->gateway>>8)&255, dev->gateway&255,
           gateway_entry->mac[0], gateway_entry->mac[1],
           gateway_entry->mac[2], gateway_entry->mac[3],
           gateway_entry->mac[4], gateway_entry->mac[5]);
  else
    printf("arp: gateway reply timeout; using DHCP server MAC\n");
  return 0;
}

int
net_dhcp(void)
{
  return net_dhcp_dev(active_netdev);
}

static int
net_output_path(uint32 destination, struct net_device **devp, uchar **macp)
{
  struct route *route = route_lookup(destination);
  struct arp_entry *entry;
  uint32 next_hop;
  uint64 deadline;

  if(route == 0)
    return -1;
  next_hop = route->gateway ? route->gateway : destination;
  entry = arp_lookup(next_hop, route->dev);
  if(entry == 0 && arp_request(route->dev, next_hop) >= 0){
    deadline = r_cntvct_el0() + (uint64)r_cntfrq_el0();
    while((entry = arp_lookup(next_hop, route->dev)) == 0 &&
          r_cntvct_el0() < deadline){
      netdev_poll_one(route->dev);
      asm volatile("yield" ::: "memory");
    }
  }
  if(entry == 0)
    return -1;
  *devp = route->dev;
  *macp = entry->mac;
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
  struct net_device *dev;
  uchar *destination_mac;

  if(id < 0 || id > 65535 || seq < 0 || seq > 65535 ||
     len < 0 || len > ICMP_MAX_PAYLOAD)
    return -1;
  if(dst == NET_IP_LOOPBACK){
    dev = active_netdev;
    destination_mac = dev ? dev->mac : 0;
  } else if((dev = netdev_by_ip(dst)) != 0){
    destination_mac = dev->mac;
  } else if(net_output_path(dst, &dev, &destination_mac) < 0){
    return -1;
  }
  if(dev == 0 || dev->ip_addr == 0)
    return -1;
  framelen = sizeof(*eth) + sizeof(*ip) + sizeof(*icmp) + len;
  packet = kalloc();
  if(packet == 0)
    return -1;
  memset(packet, 0, framelen);
  eth = (struct ethhdr*)packet;
  memmove(eth->src, dev->mac, ETH_ADDR_LEN);
  memmove(eth->dst, destination_mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  ip->vhl = 0x45;
  ip->len = swap16(sizeof(*ip) + sizeof(*icmp) + len);
  ip->id = swap16(++ip_id);
  ip->ttl = 64;
  ip->proto = IP_PROTO_ICMP;
  ip->src = swap32(dev->ip_addr);
  ip->dst = swap32(dst == NET_IP_LOOPBACK ? dev->ip_addr : dst);
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
  if(netdev_xmit_on(dev, packet, framelen) < 0){
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
  struct net_device *dev = 0;
  uchar *destination_mac = 0;
  struct route *default_route;

  if(dst == 0x0a000203U){
    default_route = route_lookup(0);
    if(default_route && default_route->dev->dns)
      wire_dst = default_route->dev->dns;
  }

  if(sport <= 0 || sport > 65535 || dport <= 0 || dport > 65535 ||
     len < 0 || len > UDP_MAX_PAYLOAD)
    return -1;
  if(dst == NET_IP_LOOPBACK){
    dev = active_netdev;
    destination_mac = dev ? dev->mac : 0;
  } else if((dev = netdev_by_ip(wire_dst)) != 0){
    destination_mac = dev->mac;
  } else if(net_output_path(wire_dst, &dev, &destination_mac) < 0){
    return -1;
  }
  if(dev == 0 || dev->ip_addr == 0)
    return -1;
  framelen = sizeof(*eth) + sizeof(*ip) + sizeof(*udp) + len;
  packet = kalloc();
  if(packet == 0)
    return -1;
  memset(packet, 0, framelen);
  eth = (struct ethhdr*)packet;
  memmove(eth->src, dev->mac, ETH_ADDR_LEN);
  memmove(eth->dst, destination_mac, ETH_ADDR_LEN);
  eth->type = swap16(ETH_TYPE_IP);
  ip = (struct iphdr*)(eth + 1);
  ip->vhl = 0x45;
  ip->len = swap16(sizeof(*ip) + sizeof(*udp) + len);
  ip->id = swap16(++ip_id);
  ip->ttl = 64;
  ip->proto = IP_PROTO_UDP;
  ip->src = swap32(dev->ip_addr);
  ip->dst = swap32(dst == NET_IP_LOOPBACK ? dev->ip_addr : wire_dst);
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

  if(dst == NET_IP_LOOPBACK || dst == dev->ip_addr){
    net_rx_dev(dev, packet, framelen);
    kfree(packet);
    return len;
  }
  if(dev){
    int r = netdev_xmit_on(dev, packet, framelen);
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
  while(p->head == 0 && p->used){
    if(myproc()->killed){
      p->waiters--;
      release(&p->lock);
      return -1;
    }
    sleep(p, &p->lock);
  }
  if(!p->used){
    p->waiters--;
    release(&p->lock);
    return -1;
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

int
net_udp_tryrecv(int port, uint64 srcaddr, uint64 sportaddr, uint64 uaddr,
                int maxlen)
{
  struct udp_port *p = findport(port);
  struct udp_dgram *d;
  int n;
  if(p == 0 || maxlen < 0){
    if(p)
      release(&p->lock);
    return -1;
  }
  if(p->head == 0){
    release(&p->lock);
    return 0;
  }
  d = p->head;
  p->head = d->next;
  if(p->head == 0)
    p->tail = 0;
  p->queued--;
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

// Sleep on the UDP port until a datagram arrives, but retain a tick-based
// deadline for protocols such as TFTP that must retransmit after packet loss.
// udp_rx() provides the fast path by waking the port immediately.  The timer
// calls net_udp_timeout_tick() only so a receiver can notice expiration when
// no packet arrives.
int
net_udp_recv_timeout(int port, uint64 srcaddr, uint64 sportaddr, uint64 uaddr,
                     int maxlen, int timeout_ticks)
{
  struct udp_port *p = findport(port);
  struct udp_dgram *d;
  uint deadline;
  int n;

  if(p == 0 || maxlen < 0 || timeout_ticks < 0){
    if(p)
      release(&p->lock);
    return -1;
  }
  deadline = ticks + timeout_ticks;
  p->waiters++;
  while(p->head == 0 && p->used){
    if(myproc()->killed){
      p->waiters--;
      release(&p->lock);
      return -1;
    }
    if((int)(ticks - deadline) >= 0){
      p->waiters--;
      release(&p->lock);
      return 0;
    }
    sleep(p, &p->lock);
  }
  if(!p->used){
    p->waiters--;
    release(&p->lock);
    return -1;
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

void
net_udp_timeout_tick(void)
{
  for(int i = 0; i < NUDPPORT; i++){
    struct udp_port *p = &ports[i];
    acquire(&p->lock);
    if(p->used && p->waiters)
      wakeup(p);
    release(&p->lock);
  }
}
