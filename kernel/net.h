#ifndef XV6_NET_H
#define XV6_NET_H

#define ETH_ADDR_LEN 6
#define ETH_TYPE_IP  0x0800
#define ETH_TYPE_ARP 0x0806
#define IP_PROTO_UDP 17
#define IP_PROTO_ICMP 1
#define ARP_HTYPE_ETH 1
#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#define NET_MTU         1500
#define UDP_MAX_PAYLOAD (NET_MTU - 20 - 8)

// Host-order IPv4 constants used by the syscall API.
#define NET_IP_LOOPBACK 0x7f000001U
#define NET_IP_LOCAL    0x0a00020fU
#define NET_IP_GATEWAY  0x0a000202U

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
