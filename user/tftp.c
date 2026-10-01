#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define TFTP_PORT 69
#define LOCAL_PORT 49152
#define TFTP_BLOCK_SIZE 512
#define TFTP_PACKET_SIZE (TFTP_BLOCK_SIZE + 4)
#define TFTP_RETRIES 5
#define TFTP_TIMEOUT_TICKS 10
#define TFTP_PROGRESS_BYTES (16 * 1024)

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
  for(int tick = 0; tick < TFTP_TIMEOUT_TICKS; tick++){
    uint32 source;
    uint16 source_port;
    int n = udp_tryrecv(port, &source, &source_port, packet, capacity);
    if(n < 0)
      return -1;
    if(n > 0){
      if(source != server)
        continue;
      if(*server_port != 0 && source_port != *server_port)
        continue;
      if(*server_port == 0)
        *server_port = source_port;
      return n;
    }
    sleep(1);
  }
  return 0;
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
    if(udp_send(server, LOCAL_PORT, TFTP_PORT, rrq, rrq_len) != rrq_len)
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
  static const char temporary_path[] = "/boot/KERNL8.TMP";
  static const char final_path[] = "/boot/KERNEL8.IMG";
  uchar rrq[256], packet[TFTP_PACKET_SIZE];
  uint16 server_port = 0, expected_block = 1, last_ack = 0;
  int rrq_len, packet_len, fd, total = 0, retries = 0;
  int start_ticks, last_progress = 0;

  rrq_len = make_rrq(rrq, sizeof(rrq), remote_name);
  if(rrq_len < 0){
    printf("tftp: remote filename is empty or too long\n");
    return -1;
  }
  fd = open(temporary_path, O_WRONLY | O_CREATE | O_TRUNC);
  if(fd < 0){
    printf("tftp: cannot create %s (check that /boot is mounted rw)\n",
           temporary_path);
    return -1;
  }
  if(udp_bind(LOCAL_PORT) < 0){
    printf("tftp: cannot bind local UDP port %d\n", LOCAL_PORT);
    close(fd);
    return -1;
  }

  start_ticks = uptime();
  printf("tftp: requesting %s from %d.%d.%d.%d\n", remote_name,
         (server >> 24) & 255, (server >> 16) & 255,
         (server >> 8) & 255, server & 255);
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
  if(rename(temporary_path, final_path) < 0){
    printf("tftp: download complete but cannot replace %s; "
           "file remains at %s\n", final_path, temporary_path);
    return -1;
  }
  if(total != last_progress)
    print_progress(total, start_ticks);
  int elapsed = uptime() - start_ticks;
  int rate_tenths = elapsed > 0 ? (total / 1024 * 100) / elapsed : 0;
  printf("\ntftp: downloaded %s (%d bytes in %d.%d s, avg %d.%d KB/s) to %s\n",
         remote_name, total, elapsed / 10, elapsed % 10,
         rate_tenths / 10, rate_tenths % 10, final_path);
  return 0;

failed:
  close(fd);
  udp_unbind(LOCAL_PORT);
  return -1;
}

int
main(int argc, char **argv)
{
  uint32 server;
  const char *remote_name = "kernel8.img";
  if(argc < 2 || argc > 3 || parse_ipv4(argv[1], &server) < 0){
    printf("usage: tftp server-ip [remote-filename]\n");
    exit(1);
  }
  if(argc == 3)
    remote_name = argv[2];
  exit(download(server, remote_name) < 0 ? 1 : 0);
}
