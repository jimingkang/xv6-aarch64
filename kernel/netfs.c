// Minimal read-only network filesystem client.
//
// NetFS deliberately starts with stat/read/readdir and an idempotent UDP RPC
// protocol.  It gives VFS a real remote backend while leaving room for a
// future distributed layer (node identity, leases, cache validation, write
// intents and replication) without coupling those policies to FAT32 or ext2.

#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "stat.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "defs.h"
#include "net.h"
#include "netfs.h"

static struct {
  struct sleeplock lock;
  int configured;
  int bound;
  uint32 server_ip;
  uint16 server_port;
  uint32 xid;
  uchar request[UDP_MAX_PAYLOAD];
  uchar response[UDP_MAX_PAYLOAD];
} netfs;

static uint16
be16(uint16 v)
{
  return (v << 8) | (v >> 8);
}

static uint32
be32(uint32 v)
{
  return (v << 24) | ((v << 8) & 0x00ff0000U) |
         ((v >> 8) & 0x0000ff00U) | (v >> 24);
}

static int
parse_ipv4(char **cursor, uint32 *address)
{
  char *p = *cursor;
  uint32 result = 0;

  for(int part = 0; part < 4; part++){
    int value = 0, digits = 0;
    while(*p >= '0' && *p <= '9'){
      value = value * 10 + *p++ - '0';
      if(++digits > 3 || value > 255)
        return -1;
    }
    if(digits == 0 || (part < 3 && *p++ != '.'))
      return -1;
    result = (result << 8) | value;
  }
  *cursor = p;
  *address = result;
  return 0;
}

void
netfsinit(void)
{
  initsleeplock(&netfs.lock, "netfs");
  netfs.configured = 0;
  netfs.bound = 0;
  netfs.xid = 0;
}

int
netfs_configure(char *source)
{
  char *p = source;
  uint32 ip;
  int port = NETFS_DEFAULT_PORT;

  if(source == 0 || parse_ipv4(&p, &ip) < 0)
    return -1;
  if(*p == ':'){
    int digits = 0;
    port = 0;
    for(p++; *p >= '0' && *p <= '9'; p++){
      port = port * 10 + *p - '0';
      digits++;
      if(port > 65535)
        return -1;
    }
    if(digits == 0)
      return -1;
  }
  if(*p != 0 || port <= 0)
    return -1;

  acquiresleep(&netfs.lock);
  if(netfs.configured){
    releasesleep(&netfs.lock);
    return -1;                    // one remote superblock in the first version
  }
  netfs.server_ip = ip;
  netfs.server_port = port;
  netfs.configured = 1;
  releasesleep(&netfs.lock);
  printf("netfs: configured server %d.%d.%d.%d:%d read-only\n",
         ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
  return 0;
}

static int
netfs_rpc(int op, char *path, uint32 offset, uint32 want,
          struct netfs_wire *reply, void *payload, int payload_max)
{
  struct netfs_wire *request = (struct netfs_wire*)netfs.request;
  struct netfs_wire *response = (struct netfs_wire*)netfs.response;
  uint32 xid, src;
  uint16 sport;
  int path_len, request_len, got, data_len, result = -1;

  if(!netfs.configured || path == 0 || path[0] != '/')
    return -1;
  path_len = strlen(path);
  if(path_len <= 0 || path_len >= MAXPATH ||
     sizeof(*request) + path_len > sizeof(netfs.request))
    return -1;

  acquiresleep(&netfs.lock);
  if(!netfs.bound){
    if(net_udp_bind_kernel(NETFS_CLIENT_PORT) < 0)
      goto out;
    netfs.bound = 1;
  }
  xid = ++netfs.xid;
  memset(request, 0, sizeof(*request));
  request->magic = be32(NETFS_MAGIC);
  request->version = NETFS_VERSION;
  request->op = op;
  request->xid = be32(xid);
  request->offset = be32(offset);
  request->length = be32(want);
  request->path_len = be16(path_len);
  memmove(request + 1, path, path_len);
  request_len = sizeof(*request) + path_len;

  for(int attempt = 0; attempt < 3; attempt++){
    if(net_udp_send_kernel(netfs.server_ip, NETFS_CLIENT_PORT,
                           netfs.server_port, request, request_len) < 0)
      continue;
    for(;;){
      got = net_udp_recv_kernel_timeout(NETFS_CLIENT_PORT, &src, &sport,
                                        response, sizeof(netfs.response), 50);
      if(got <= 0)
        break;
      if(got < (int)sizeof(*response) || src != netfs.server_ip ||
         sport != netfs.server_port || be32(response->magic) != NETFS_MAGIC ||
         response->version != NETFS_VERSION || response->op != op ||
         be32(response->xid) != xid)
        continue;                  // stale or unrelated datagram
      if((int)be32(response->status) < 0)
        goto out;
      data_len = be32(response->length);
      if(data_len > (uint32)(got - sizeof(*response)) ||
         data_len > (uint32)payload_max)
        goto out;
      if(reply)
        *reply = *response;
      if(data_len && payload)
        memmove(payload, response + 1, data_len);
      result = data_len;
      goto out;
    }
  }
  printf("netfs: RPC timeout op=%d path=%s server=%d.%d.%d.%d:%d\n",
         op, path, netfs.server_ip >> 24, (netfs.server_ip >> 16) & 255,
         (netfs.server_ip >> 8) & 255, netfs.server_ip & 255,
         netfs.server_port);
out:
  releasesleep(&netfs.lock);
  return result;
}

int
netfsstat(char *path, struct stat *st)
{
  struct netfs_wire reply;
  if(st == 0 || netfs_rpc(NETFS_OP_STAT, path, 0, 0, &reply, 0, 0) < 0)
    return -1;
  memset(st, 0, sizeof(*st));
  st->dev = 5;
  st->ino = be32(reply.ino);
  st->type = be16(reply.type);
  st->nlink = 1;
  st->size = be32(reply.size);
  return st->type == T_FILE || st->type == T_DIR ? 0 : -1;
}

int
netfsread(char *path, uint64 off, void *dst, int n)
{
  int done = 0;
  if(dst == 0 || n < 0 || off > 0xffffffffU ||
     (uint64)n > 0xffffffffU - off)
    return -1;
  while(done < n){
    int chunk = n - done;
    if(chunk > NETFS_DATA_MAX)
      chunk = NETFS_DATA_MAX;
    int got = netfs_rpc(NETFS_OP_READ, path, off + done, chunk, 0,
                        (uchar*)dst + done, chunk);
    if(got < 0)
      return done ? done : -1;
    done += got;
    if(got < chunk)
      break;
  }
  return done;
}

int
netfsreaddir(char *path, int index, struct netfs_dirent *de)
{
  struct netfs_wire reply;
  int n;
  if(index < 0 || de == 0)
    return -1;
  memset(de, 0, sizeof(*de));
  n = netfs_rpc(NETFS_OP_READDIR, path, index, 14, &reply,
                de->name, sizeof(de->name) - 1);
  if(n < 0)
    return -1;
  if(n == 0)
    return 0;
  de->name[n] = 0;
  de->ino = be32(reply.ino);
  de->size = be32(reply.size);
  de->type = be16(reply.type);
  return 1;
}
