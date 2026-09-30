#ifndef XV6_NET_H
#define XV6_NET_H

#include "device.h"

#define ETH_ADDR_LEN 6
#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806
#define IP_PROTO_UDP 17
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define ARP_HTYPE_ETH 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#define NET_MTU         1500
#define UDP_MAX_PAYLOAD (NET_MTU - 20 - 8)

// Host-order IPv4 constants used by the syscall API.
#define NET_IP_LOOPBACK 0x7f000001U
#define NET_IP_LOCAL    0x0a00020fU
#define NET_IP_GATEWAY  0x0a000202U

struct net_device;

struct net_device_ops {
  int (*start_xmit)(struct net_device *dev, void *frame, int length);
  void (*poll)(struct net_device *dev);
  int (*open)(struct net_device *dev);
  void (*stop)(struct net_device *dev);
};

struct net_device {
  struct device dev;
  const struct net_device_ops *ops;
  void *priv;
  uint8 mac[ETH_ADDR_LEN];
  int mtu;
  int running;
  uint32 ip_addr;
  uint32 netmask;
  uint32 gateway;
  uint32 dns;
};

struct route {
  uint32 destination;
  uint32 netmask;
  uint32 gateway;
  uint32 metric;
  struct net_device *dev;
  int valid;
};

struct arp_entry {
  uint32 ip;
  uint8 mac[ETH_ADDR_LEN];
  struct net_device *dev;
  uint64 updated;
  int state;
  int valid;
};

int register_netdev(struct net_device *dev);
int unregister_netdev(struct net_device *dev);
struct net_device *netdev_default(void);
struct net_device *netdev_find(char *name);
void netdev_poll_all(void);
void netdev_poll_one(struct net_device *dev);
void net_rx_dev(struct net_device *dev, void *packet, int len);
int net_dhcp_dev(struct net_device *dev);

struct ethhdr {
  uint8 dst[ETH_ADDR_LEN];
  uint8 src[ETH_ADDR_LEN];
  uint16 type;
} __attribute__((packed));

struct iphdr {
  uint8 vhl;
  uint8 tos;
  uint16 len;
  uint16 id;
  uint16 off;
  uint8 ttl;
  uint8 proto;
  uint16 sum;
  uint32 src;
  uint32 dst;
} __attribute__((packed));

struct udphdr {
  uint16 sport;
  uint16 dport;
  uint16 len;
  uint16 sum;
} __attribute__((packed));

struct tcphdr {
  uint16 sport;
  uint16 dport;
  uint32 seq;
  uint32 ack;
  uint8 offset;
  uint8 flags;
  uint16 window;
  uint16 sum;
  uint16 urgent;
} __attribute__((packed));

struct icmphdr {
  uint8 type;
  uint8 code;
  uint16 sum;
  uint16 id;
  uint16 seq;
} __attribute__((packed));

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8
#define ICMP_MAX_PAYLOAD  (NET_MTU - 20 - 8)

struct arphdr {
  uint16 htype;
  uint16 ptype;
  uint8 hlen;
  uint8 plen;
  uint16 op;
  uint8 sha[ETH_ADDR_LEN];
  uint32 spa;
  uint8 tha[ETH_ADDR_LEN];
  uint32 tpa;
} __attribute__((packed));

#endif
