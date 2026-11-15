#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TFTP_PORT 69
// make tiny210-tftp serves build/tiny210 on an unprivileged port, so the
// server may be given as a.b.c.d:port (in /etc/tftp.conf or on the command line).
static uint16 tftp_port = TFTP_PORT;
#define LOCAL_PORT 49152
#define TFTP_BLOCK_SIZE 512
#define TFTP_PACKET_SIZE (TFTP_BLOCK_SIZE + 4)
#define TFTP_RETRIES 5
#define TFTP_TIMEOUT_TICKS 10
#define TFTP_PROGRESS_BYTES (16 * 1024)
#define TFTP_PATH_SIZE 128
#define TFTP_DEFAULT_FILE "xv6_tiny210.img"   // what BL1 loads from the FAT32 partition
#define TFTP_CONF "/etc/tftp.conf"           // "server=a.b.c.d", written by make tiny210
// Downloads land in an 8.3 temporary file first and are copied over the
// target only when complete, so a dropped transfer never leaves a half
// kernel where BL1 will look for it.
#define TFTP_TEMP "/boot/TFTP.TMP"

// Keep transfer buffers out of the small user call stack.  The client binds a
// fixed UDP port, so only one active instance is supported anyway and
// process-private BSS storage is sufficient.
static char tftp_final_path[TFTP_PATH_SIZE];
static uchar tftp_rrq[256];
static uchar tftp_packet[TFTP_PACKET_SIZE];
static char tftp_copybuf[4096];
static char tftp_conf[128];

static int
parse_ipv4(const char *text, uint32 *address)
{
  uint32 result = 0;
  int octets = 0;
  const char *p = text;

  while(*p){
    int value = 0, digits = 0;
    while(*p >= '0' && *p <= '9'){
      value = value * 10 + *p++ - '0';
      if(value > 255)
        return -1;
      digits++;
    }
    if(digits == 0)
      return -1;
    result = (result << 8) | value;
    octets++;
    if(octets < 4){
      if(*p++ != '.')
        return -1;
    } else if(*p != 0){
      return -1;
    }
  }
  if(octets != 4)
    return -1;
  *address = result;
  return 0;
}

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

static int
send_ack(uint32 server, uint16 server_port, uint16 block)
{
  uchar packet[4];
  put16(packet, 4);
  put16(packet + 2, block);
  return udp_send(server, LOCAL_PORT, server_port, packet, sizeof(packet)) == 4
           ? 0 : -1;
}

static void
print_progress(int total, int start_ticks)
{
  int elapsed = uptime() - start_ticks;
  int rate_tenths = elapsed > 0 ? (total / 1024 * 100) / elapsed : 0;
  printf("\rtftp: %d bytes, %d.%d KB/s", total,
         rate_tenths / 10, rate_tenths % 10);
}

static int
wait_packet(uint32 server, uint16 *server_port, int port,
            uchar *packet, int capacity)
{
  for(;;){
    uint32 source;
    uint16 source_port;
    int n = udp_recv_timeout(port, &source, &source_port, packet, capacity,
                             TFTP_TIMEOUT_TICKS);
    if(n < 0)
      return -1;
    if(n == 0)
      return 0;
    if(n > 0){
      if(source != server)
        continue;
      if(*server_port != 0 && source_port != *server_port)
        continue;
      if(*server_port == 0)
        *server_port = source_port;
      return n;
    }
  }
}

static int
make_rrq(uchar *packet, int capacity, const char *filename)
{
  const char mode[] = "octet";
  int name_len = strlen(filename);
  int mode_len = sizeof(mode);
  int length = 2 + name_len + 1 + mode_len;
  if(name_len == 0 || length > capacity)
    return -1;
  put16(packet, 1);
  memmove(packet + 2, filename, name_len);
  packet[2 + name_len] = 0;
  memmove(packet + 3 + name_len, mode, mode_len);
  return length;
}

static int
receive_first_data(uint32 server, int port, const uchar *rrq, int rrq_len,
                   uchar *packet, int *packet_len, uint16 *server_port)
{
  for(int retry = 0; retry < TFTP_RETRIES; retry++){
    if(udp_send(server, LOCAL_PORT, tftp_port, rrq, rrq_len) != rrq_len)
      return -1;
    int n = wait_packet(server, server_port, port, packet, TFTP_PACKET_SIZE);
    if(n < 0)
      return -1;
    if(n > 0){
      *packet_len = n;
      return 0;
    }
    printf("tftp: timeout waiting for server response (%d/%d)\n",
           retry + 1, TFTP_RETRIES);
  }
  return -1;
}

static int
download(uint32 server, const char *remote_name)
{
  char *final_path = tftp_final_path;
  uchar *rrq = tftp_rrq;
  uchar *packet = tftp_packet;
  uint16 server_port = 0, expected_block = 1, last_ack = 0;
  int rrq_len, packet_len, fd, total = 0, retries = 0, name_len;
  int start_ticks, last_progress = 0;

  memset(final_path, 0, TFTP_PATH_SIZE);
  memmove(final_path, TFTP_TEMP, sizeof(TFTP_TEMP));
  rrq_len = make_rrq(rrq, 256, remote_name);
  name_len = strlen(remote_name);
  if(rrq_len < 0 || name_len == 0 ||
     name_len + sizeof("/boot/") > TFTP_PATH_SIZE ||
     strchr(remote_name, '/') != 0){
    printf("tftp: invalid remote basename or filename is too long\n");
    return -1;
  }
  fd = open(final_path, O_WRONLY | O_CREATE | O_TRUNC);
  if(fd < 0){
    printf("tftp: cannot create %s (check that /boot is mounted rw)\n",
           final_path);
    return -1;
  }
  if(udp_bind(LOCAL_PORT) < 0){
    printf("tftp: cannot bind local UDP port %d\n", LOCAL_PORT);
    close(fd);
    return -1;
  }

  start_ticks = uptime();
  printf("tftp: requesting %s from %d.%d.%d.%d port %d\n", remote_name,
         (server >> 24) & 255, (server >> 16) & 255,
         (server >> 8) & 255, server & 255, tftp_port);
  if(receive_first_data(server, LOCAL_PORT, rrq, rrq_len, packet,
                        &packet_len, &server_port) < 0){
    printf("tftp: no response from server\n");
    goto failed;
  }

  for(;;){
    uint16 opcode;
    if(packet_len < 2){
      printf("tftp: malformed packet\n");
      goto failed;
    }
    opcode = get16(packet);
    if(opcode == 5){
      char message[128];
      int code = packet_len >= 4 ? get16(packet + 2) : -1;
      int length = 0;
      if(packet_len < 4){
        memmove(message, "malformed error", sizeof("malformed error"));
      } else {
        while(length < packet_len - 4 && length < sizeof(message) - 1 &&
              packet[length + 4] != 0){
          message[length] = packet[length + 4];
          length++;
        }
        message[length] = 0;
      }
      printf("tftp: server error %d: %s\n", code, message);
      goto failed;
    }
    if(opcode != 3 || packet_len < 4 ||
       packet_len > TFTP_PACKET_SIZE){
      printf("tftp: unexpected opcode or packet size\n");
      goto failed;
    }

    uint16 block = get16(packet + 2);
    if(block == expected_block){
      int data_len = packet_len - 4;
      if(data_len > 0 && write(fd, packet + 4, data_len) != data_len){
        printf("tftp: failed writing downloaded data\n");
        goto failed;
      }
      total += data_len;
      if(total - last_progress >= TFTP_PROGRESS_BYTES){
        print_progress(total, start_ticks);
        last_progress = total;
      }
      if(send_ack(server, server_port, block) < 0){
        printf("tftp: failed sending ACK for block %d\n", block);
        goto failed;
      }
      last_ack = block;
      expected_block++;
      retries = 0;
      if(data_len < TFTP_BLOCK_SIZE)
        break;
    } else if(block == last_ack){
      if(send_ack(server, server_port, last_ack) < 0)
        goto failed;
    }

    for(;;){
      packet_len = wait_packet(server, &server_port, LOCAL_PORT,
                               packet, TFTP_PACKET_SIZE);
      if(packet_len < 0){
        printf("tftp: UDP receive failed\n");
        goto failed;
      }
      if(packet_len > 0)
        break;
      if(++retries > TFTP_RETRIES){
        printf("tftp: transfer timed out\n");
        goto failed;
      }
      printf("tftp: retrying block %d (%d/%d)\n",
             last_ack, retries, TFTP_RETRIES);
      if(send_ack(server, server_port, last_ack) < 0)
        goto failed;
    }
  }

  if(close(fd) < 0){
    printf("tftp: close failed\n");
    udp_unbind(LOCAL_PORT);
    return -1;
  }
  if(udp_unbind(LOCAL_PORT) < 0){
    printf("tftp: failed to release UDP port\n");
    return -1;
  }
  if(total != last_progress)
    print_progress(total, start_ticks);
  int elapsed = uptime() - start_ticks;
  int rate_tenths = elapsed > 0 ? (total / 1024 * 100) / elapsed : 0;
  printf("\ntftp: downloaded %s (%d bytes in %d.%d s, avg %d.%d KB/s)\n",
         remote_name, total, elapsed / 10, elapsed % 10,
         rate_tenths / 10, rate_tenths % 10);
  return total;

failed:
  close(fd);
  udp_unbind(LOCAL_PORT);
  printf("tftp: incomplete download left in %s; /boot/%s is unchanged\n",
         final_path, remote_name);
  return -1;
}

// Copy the finished download over /boot/<name> and check the length.
static int
install(const char *remote_name, int total)
{
  char *target = tftp_final_path;
  int in, out, n, copied = 0;
  memmove(target, "/boot/", sizeof("/boot/"));
  memmove(target + sizeof("/boot/") - 1, remote_name, strlen(remote_name) + 1);
  in = open(TFTP_TEMP, O_RDONLY);
  if(in < 0){
    printf("tftp: cannot reopen %s\n", TFTP_TEMP);
    return -1;
  }
  out = open(target, O_WRONLY | O_CREATE | O_TRUNC);
  if(out < 0){
    printf("tftp: cannot write %s\n", target);
    close(in);
    return -1;
  }
  while((n = read(in, tftp_copybuf, sizeof(tftp_copybuf))) > 0){
    if(write(out, tftp_copybuf, n) != n){
      printf("tftp: write to %s failed after %d bytes\n", target, copied);
      close(in);
      close(out);
      return -1;
    }
    copied += n;
  }
  close(in);
  if(close(out) < 0 || n < 0 || copied != total){
    printf("tftp: copy to %s incomplete (%d of %d bytes)\n", target, copied, total);
    return -1;
  }
  printf("tftp: installed %s (%d bytes); run reboot to boot it\n", target, copied);
  return 0;
}

// Server address from /etc/tftp.conf ("server=a.b.c.d"), or 0.
static const char *
conf_server(void)
{
  int fd = open(TFTP_CONF, O_RDONLY), n;
  if(fd < 0)
    return 0;
  n = read(fd, tftp_conf, sizeof(tftp_conf) - 1);
  close(fd);
  if(n <= 0)
    return 0;
  tftp_conf[n] = 0;
  for(char *p = tftp_conf; *p; ){
    char *line = p;
    while(*p && *p != '\n' && *p != '\r')
      p++;
    if(*p)
      *p++ = 0;
    if(strlen(line) > 7 && memcmp(line, "server=", 7) == 0)
      return line + 7;
  }
  return 0;
}

static int
is_root_image(const char *name)
{
  const char *fs = "fs.img";
  int i;
  for(i = 0; fs[i]; i++){
    char c = name[i];
    if(c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
    if(c != fs[i])
      return 0;
  }
  return name[i] == 0;
}

int
main(int argc, char **argv)
{
  uint32 server;
  const char *server_name = conf_server();
  const char *remote_name = TFTP_DEFAULT_FILE;
  int total;

  if(argc > 3){
    printf("usage: tftp [server-ip[:port] [remote-filename]]\n");
    exit(1);
  }
  if(argc >= 2)
    server_name = argv[1];
  if(argc == 3)
    remote_name = argv[2];
  if(server_name == 0){
    printf("tftp: no server: give one, or put server=a.b.c.d in %s\n", TFTP_CONF);
    printf("usage: tftp [server-ip[:port] [remote-filename]]   (default file %s)\n",
           TFTP_DEFAULT_FILE);
    exit(1);
  }
  static char host[32];
  int hl = 0;
  while(server_name[hl] && server_name[hl] != ':' && hl < (int)sizeof(host) - 1){
    host[hl] = server_name[hl];
    hl++;
  }
  host[hl] = 0;
  if(server_name[hl] == ':'){
    int port = atoi(server_name + hl + 1);
    if(port <= 0 || port > 65535){
      printf("tftp: invalid port in %s\n", server_name);
      exit(1);
    }
    tftp_port = port;
  }
  if(parse_ipv4(host, &server) < 0){
    printf("tftp: invalid server IPv4 address: %s\n", server_name);
    printf("usage: tftp [server-ip[:port] [remote-filename]]\n");
    exit(1);
  }
  if(is_root_image(remote_name)){
    printf("tftp: refusing to overwrite FS.IMG, the mounted root file system\n");
    exit(1);
  }
  total = download(server, remote_name);
  if(total < 0 || install(remote_name, total) < 0)
    exit(1);
  exit(0);
}
