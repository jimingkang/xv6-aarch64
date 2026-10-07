// Minimal read-only TFTP server.  RRQ requests arrive on UDP 69 and each
// transfer uses its own server TID port, as required by RFC 1350.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TFTP_PORT             69
#define TFTP_TID_FIRST        50000
#define TFTP_TID_LAST         50100
#define TFTP_BLOCK_SIZE       512
#define TFTP_PACKET_SIZE      (TFTP_BLOCK_SIZE + 4)
#define TFTP_TIMEOUT_TICKS    10
#define TFTP_RETRIES          5
#define TFTP_PATH_SIZE        192
#define TFTP_PROGRESS_BYTES   (16 * 1024)

static uchar request_packet[TFTP_PACKET_SIZE];
static uchar data_packet[TFTP_PACKET_SIZE];
static char request_name[128];
static char serve_path[TFTP_PATH_SIZE];

static void
put16(uchar *p, uint16 value)
{
  p[0] = value >> 8;
  p[1] = value;
}

static uint16
get16(const uchar *p)
{
  return ((uint16)p[0] << 8) | p[1];
}

static void
print_ip(uint32 ip)
{
  printf("%d.%d.%d.%d", (ip >> 24) & 255, (ip >> 16) & 255,
         (ip >> 8) & 255, ip & 255);
}

static int
send_error(uint32 client, uint16 client_port, uint16 local_port,
           uint16 code, const char *message)
{
  uchar packet[132];
  int length = strlen(message);
  if(length > (int)sizeof(packet) - 5)
    length = sizeof(packet) - 5;
  put16(packet, 5);
  put16(packet + 2, code);
  memmove(packet + 4, message, length);
  packet[4 + length] = 0;
  return udp_send(client, local_port, client_port, packet, 5 + length);
}

// Only a basename is accepted.  In particular, neither absolute paths nor
// ../ traversal can escape the configured service directory.
static int
safe_basename(const char *name)
{
  int length = strlen(name);
  if(length == 0 || length >= (int)sizeof(request_name) ||
     strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    return 0;
  for(int i = 0; i < length; i++)
    if(name[i] == '/' || name[i] == '\\' ||
       (uchar)name[i] < 0x20 || (uchar)name[i] > 0x7e)
      return 0;
  return 1;
}

static int
parse_rrq(const uchar *packet, int n)
{
  int p = 2, name_length = 0;
  char mode[16];
  int mode_length = 0;

  if(n < 4 || get16(packet) != 1)
    return -1;
  while(p < n && packet[p] && name_length < (int)sizeof(request_name) - 1)
    request_name[name_length++] = packet[p++];
  if(p >= n || packet[p] != 0)
    return -1;
  request_name[name_length] = 0;
  p++;
  while(p < n && packet[p] && mode_length < (int)sizeof(mode) - 1){
    char c = packet[p++];
    if(c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    mode[mode_length++] = c;
  }
  if(p >= n || packet[p] != 0)
    return -1;
  mode[mode_length] = 0;
  if(strcmp(mode, "octet") != 0 || !safe_basename(request_name))
    return -1;
  return 0;
}

static int
make_path(const char *root, const char *name)
{
  int root_length = strlen(root), name_length = strlen(name);
  int slash = root_length > 0 && root[root_length - 1] != '/';
  if(root_length == 0 || root_length + slash + name_length + 1 >
     (int)sizeof(serve_path))
    return -1;
  memmove(serve_path, root, root_length);
  if(slash)
    serve_path[root_length++] = '/';
  memmove(serve_path + root_length, name, name_length + 1);
  return 0;
}

static int
bind_tid(void)
{
  static int next_tid = TFTP_TID_FIRST;
  for(int i = TFTP_TID_FIRST; i <= TFTP_TID_LAST; i++){
    int tid = next_tid++;
    if(next_tid > TFTP_TID_LAST)
      next_tid = TFTP_TID_FIRST;
    if(udp_bind(tid) == 0)
      return tid;
  }
  return -1;
}

static int
wait_ack(uint32 client, uint16 client_port, uint16 tid, uint16 block)
{
  uchar ack[64];
  for(;;){
    uint32 source;
    uint16 source_port;
    int n = udp_recv_timeout(tid, &source, &source_port, ack, sizeof(ack),
                             TFTP_TIMEOUT_TICKS);
    if(n <= 0)
      return n;
    if(source == client && source_port == client_port && n >= 4 &&
       get16(ack) == 4 && get16(ack + 2) == block)
      return 1;
    if(source != client || source_port != client_port)
      send_error(source, source_port, tid, 5, "unknown transfer ID");
  }
}

static int
serve_file(const char *root, uint32 client, uint16 client_port)
{
  int fd = -1, tid = -1, total = 0, last_progress = 0;
  uint16 block = 1;
  int started = uptime();

  if(make_path(root, request_name) < 0){
    send_error(client, client_port, TFTP_PORT, 2, "invalid path");
    return -1;
  }
  fd = open(serve_path, O_RDONLY);
  if(fd < 0){
    send_error(client, client_port, TFTP_PORT, 1, "file not found");
    printf("tftpd: not found %s\n", serve_path);
    return -1;
  }
  tid = bind_tid();
  if(tid < 0){
    send_error(client, client_port, TFTP_PORT, 0, "no transfer port");
    close(fd);
    return -1;
  }

  printf("tftpd: RRQ %s from ", serve_path);
  print_ip(client);
  printf(":%d tid=%d\n", client_port, tid);

  for(;;){
    int count = read(fd, data_packet + 4, TFTP_BLOCK_SIZE);
    if(count < 0){
      send_error(client, client_port, tid, 0, "file read failed");
      goto failed;
    }
    put16(data_packet, 3);
    put16(data_packet + 2, block);

    int acknowledged = 0;
    for(int retry = 0; retry < TFTP_RETRIES; retry++){
      if(udp_send(client, tid, client_port, data_packet, count + 4) !=
         count + 4)
        goto failed;
      int ack = wait_ack(client, client_port, tid, block);
      if(ack < 0)
        goto failed;
      if(ack > 0){
        acknowledged = 1;
        break;
      }
      printf("tftpd: retry block %d (%d/%d)\n",
             block, retry + 1, TFTP_RETRIES);
    }
    if(!acknowledged){
      printf("tftpd: client timed out at block %d\n", block);
      goto failed;
    }
    total += count;
    if(total - last_progress >= TFTP_PROGRESS_BYTES){
      printf("tftpd: sent %d bytes\n", total);
      last_progress = total;
    }
    block++;
    if(count < TFTP_BLOCK_SIZE)
      break;
  }

  close(fd);
  udp_unbind(tid);
  int elapsed = uptime() - started;
  printf("tftpd: completed %s, %d bytes in %d.%d s\n",
         request_name, total, elapsed / 10, elapsed % 10);
  return 0;

failed:
  if(fd >= 0)
    close(fd);
  if(tid >= 0)
    udp_unbind(tid);
  printf("tftpd: transfer of %s failed after %d bytes\n",
         request_name, total);
  return -1;
}

int
main(int argc, char **argv)
{
  const char *root = "/mnt/ext2/video";
  if(argc > 2){
    printf("usage: tftpd [service-directory]\n");
    exit(1);
  }
  if(argc == 2)
    root = argv[1];
  if(root[0] != '/'){
    printf("tftpd: service directory must be an absolute path\n");
    exit(1);
  }
  if(udp_bind(TFTP_PORT) < 0){
    printf("tftpd: cannot bind UDP port 69\n");
    exit(1);
  }
  printf("tftpd: serving %s read-only on UDP port 69\n", root);

  for(;;){
    uint32 client;
    uint16 client_port;
    int n = udp_recv(TFTP_PORT, &client, &client_port,
                     request_packet, sizeof(request_packet));
    if(n < 0){
      printf("tftpd: UDP receive failed\n");
      break;
    }
    if(parse_rrq(request_packet, n) < 0){
      send_error(client, client_port, TFTP_PORT, 4,
                 "only basename octet RRQ is supported");
      continue;
    }
    serve_file(root, client, client_port);
  }
  udp_unbind(TFTP_PORT);
  exit(1);
}
