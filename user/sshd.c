#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/epoll.h"
#include "user/user.h"
#include "user/ssh_crypto.h"

#define SSH_MSG_DISCONNECT 1
#define SSH_MSG_KEXINIT 20
#define SSH_MSG_NEWKEYS 21
#define SSH_MSG_KEX_ECDH_INIT 30
#define SSH_MSG_KEX_ECDH_REPLY 31
#define SSH_MSG_SERVICE_REQUEST 5
#define SSH_MSG_SERVICE_ACCEPT 6
#define SSH_MSG_USERAUTH_REQUEST 50
#define SSH_MSG_USERAUTH_FAILURE 51
#define SSH_MSG_USERAUTH_SUCCESS 52
#define SSH_MSG_CHANNEL_OPEN 90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93
#define SSH_MSG_CHANNEL_DATA 94
#define SSH_MSG_CHANNEL_EOF 96
#define SSH_MSG_CHANNEL_CLOSE 97
#define SSH_MSG_CHANNEL_REQUEST 98
#define SSH_MSG_CHANNEL_SUCCESS 99
#define SSH_MSG_CHANNEL_FAILURE 100
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED 3
#define SSH_PACKET_MAX 4096
#define SSH_CHANNEL_WINDOW 65536
#define SSH_CHANNEL_PACKET 2048
#define PASSWD_MAX 512

static int ssh_listener = -1;

struct ssh_transport {
  uint32 send_sequence;
  uint32 receive_sequence;
  int encrypt_out;
  int encrypt_in;
  struct ssh_aes128_ctr cipher_out;
  struct ssh_aes128_ctr cipher_in;
  uchar mac_out[32];
  uchar mac_in[32];
};

static void
put32(uchar *p, uint32 v)
{
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}

static uint32
get32(const uchar *p)
{
  return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) |
         ((uint32)p[2] << 8) | p[3];
}

static int
readn(int fd, void *address, int n)
{
  uchar *p = address;
  int done = 0;
  while(done < n){
    int r = read(fd, p + done, n - done);
    if(r <= 0)
      return -1;
    done += r;
  }
  return 0;
}

static int
writen(int fd, const void *address, int n)
{
  const uchar *p = address;
  int done = 0;
  while(done < n){
    int r = write(fd, p + done, n - done);
    if(r <= 0)
      return -1;
    done += r;
  }
  return 0;
}

static int
starts_with(char *s, char *prefix)
{
  while(*prefix)
    if(*s++ != *prefix++)
      return 0;
  return 1;
}

// RFC 4253 section 4.2 permits pre-identification lines. Each line, including
// CRLF, is bounded to 255 bytes; only the SSH- line participates in KEX.
static int
read_identification(int fd, char *ident, int size)
{
  for(int lines = 0; lines < 16; lines++){
    int n = 0;
    while(n < 255){
      char ch;
      if(readn(fd, &ch, 1) < 0)
        return -1;
      if(ch == 0)
        return -1;
      if(ch == '\n')
        break;
      if(ch != '\r'){
        if(n + 1 >= size)
          return -1;
        ident[n++] = ch;
      }
    }
    ident[n] = 0;
    if(starts_with(ident, "SSH-"))
      return starts_with(ident, "SSH-2.0-") ? 0 : -1;
  }
  return -1;
}

static uint32
prng_next(uint32 *state)
{
  uint32 x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

static int
packet_write(int fd, const uchar *payload, int payload_len, uint32 *random,
             struct ssh_transport *transport)
{
  uchar *packet;
  uchar mac_input[4];
  uchar mac[32];
  int padding, packet_len, total, block_size;

  block_size = transport->encrypt_out ? 16 : 8;
  padding = block_size - ((payload_len + 5) % block_size);
  if(padding < 4)
    padding += block_size;
  packet_len = payload_len + padding + 1;
  total = packet_len + 4;
  if(total > SSH_PACKET_MAX || (packet = malloc(total)) == 0)
    return -1;
  put32(packet, packet_len);
  packet[4] = padding;
  memmove(packet + 5, payload, payload_len);
  if(ssh_random(packet + 5 + payload_len, padding) < 0)
    for(int i = 0; i < padding; i++)
      packet[5 + payload_len + i] = prng_next(random);
  int result;
  if(transport->encrypt_out){
    put32(mac_input, transport->send_sequence);
    uchar *authenticated = malloc(4 + total);
    if(authenticated == 0){
      free(packet);
      return -1;
    }
    memmove(authenticated, mac_input, 4);
    memmove(authenticated + 4, packet, total);
    if(ssh_hmac_sha256(mac, transport->mac_out, 32,
                       authenticated, 4 + total) < 0){
      free(authenticated);
      free(packet);
      return -1;
    }
    free(authenticated);
    ssh_aes128_ctr_xor(&transport->cipher_out, packet, total);
    result = writen(fd, packet, total) < 0 || writen(fd, mac, 32) < 0 ? -1 : 0;
  } else {
    result = writen(fd, packet, total);
  }
  if(result == 0)
    transport->send_sequence++;
  free(packet);
  return result;
}

static int
packet_read(int fd, uchar *payload, int capacity,
            struct ssh_transport *transport)
{
  uchar header[16], received_mac[32], computed_mac[32], sequence[4];
  uchar *body;
  uint32 packet_len;
  int padding, payload_len, total;

  if(transport->encrypt_in){
    if(readn(fd, header, 16) < 0)
      return -1;
    ssh_aes128_ctr_xor(&transport->cipher_in, header, 16);
  } else if(readn(fd, header, 4) < 0){
    return -1;
  }
  packet_len = get32(header);
  total = packet_len + 4;
  if(packet_len < 5 || total > SSH_PACKET_MAX ||
     total % (transport->encrypt_in ? 16 : 8) != 0)
    return -1;
  if((body = malloc(total)) == 0)
    return -1;
  memmove(body, header, transport->encrypt_in ? 16 : 4);
  int have = transport->encrypt_in ? 16 : 4;
  if(readn(fd, body + have, total - have) < 0)
    goto bad;
  if(transport->encrypt_in){
    ssh_aes128_ctr_xor(&transport->cipher_in, body + 16, total - 16);
    if(readn(fd, received_mac, sizeof(received_mac)) < 0)
      goto bad;
    put32(sequence, transport->receive_sequence);
    uchar *authenticated = malloc(4 + total);
    if(authenticated == 0)
      goto bad;
    memmove(authenticated, sequence, 4);
    memmove(authenticated + 4, body, total);
    int failed = ssh_hmac_sha256(computed_mac, transport->mac_in, 32,
                                 authenticated, 4 + total);
    free(authenticated);
    uchar difference = 0;
    if(failed == 0)
      for(int i = 0; i < 32; i++)
        difference |= computed_mac[i] ^ received_mac[i];
    if(failed < 0 || difference){
      printf("sshd: encrypted packet MAC failure seq=%d\n",
             transport->receive_sequence);
      goto bad;
    }
  }
  padding = body[4];
  payload_len = packet_len - padding - 1;
  if(padding < 4 || payload_len <= 0 || payload_len > capacity){
    goto bad;
  }
  memmove(payload, body + 5, payload_len);
  transport->receive_sequence++;
  free(body);
  return payload_len;
bad:
  free(body);
  return -1;
}

static int
append_bytes(uchar *p, int capacity, int *offset, const void *src, int n)
{
  if(n < 0 || *offset + n > capacity)
    return -1;
  memmove(p + *offset, src, n);
  *offset += n;
  return 0;
}

static int
append_u32(uchar *p, int capacity, int *offset, uint32 v)
{
  uchar b[4];
  put32(b, v);
  return append_bytes(p, capacity, offset, b, sizeof(b));
}

static int
append_string(uchar *p, int capacity, int *offset, char *s)
{
  int n = strlen(s);
  if(append_u32(p, capacity, offset, n) < 0)
    return -1;
  return append_bytes(p, capacity, offset, s, n);
}

static int
append_blob(uchar *p, int capacity, int *offset, const void *data, int length)
{
  if(append_u32(p, capacity, offset, length) < 0)
    return -1;
  return append_bytes(p, capacity, offset, data, length);
}

static int
parse_u32(const uchar *p, int length, int *offset, uint32 *value)
{
  if(*offset < 0 || *offset + 4 > length)
    return -1;
  *value = get32(p + *offset);
  *offset += 4;
  return 0;
}

static int
parse_string(const uchar *p, int length, int *offset,
             const uchar **value, uint32 *value_length)
{
  uint32 n;
  if(parse_u32(p, length, offset, &n) < 0 ||
     n > (uint32)(length - *offset))
    return -1;
  *value = p + *offset;
  *value_length = n;
  *offset += n;
  return 0;
}

static int
string_is(const uchar *value, uint32 length, const char *expected)
{
  int n = strlen(expected);
  return length == (uint32)n && memcmp(value, expected, n) == 0;
}

static int
copy_string(char *dst, int capacity, const uchar *src, uint32 length)
{
  if(capacity <= 0 || length >= (uint32)capacity)
    return -1;
  memmove(dst, src, length);
  dst[length] = 0;
  return 0;
}

// Educational plaintext passwd database: name:password:uid:gid:gecos:home:shell.
// Password bytes are never printed and the comparison examines the whole field.
static int
authenticate(const uchar *name, uint32 name_length,
             const uchar *password, uint32 password_length,
             char home[64], char shell[64])
{
  char buf[PASSWD_MAX], *line, *end;
  int fd = open("/etc/passwd", O_RDONLY);
  if(fd < 0)
    return -1;
  int n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n <= 0)
    return -1;
  buf[n] = 0;

  for(line = buf; *line; line = end){
    char *field[7], *cursor = line;
    int valid = 1;
    end = strchr(line, '\n');
    if(end)
      *end++ = 0;
    else
      end = line + strlen(line);
    if(*line == 0 || *line == '#')
      continue;
    field[0] = cursor;
    for(int i = 0; i < 6; i++){
      cursor = strchr(cursor, ':');
      if(cursor == 0){
        valid = 0;
        break;
      }
      *cursor++ = 0;
      field[i + 1] = cursor;
    }
    if(!valid)
      continue;
    uint32 user_len = strlen(field[0]);
    uint32 pass_len = strlen(field[1]);
    uchar difference = (user_len != name_length) | (pass_len != password_length);
    uint32 max = pass_len > password_length ? pass_len : password_length;
    for(uint32 i = 0; i < max; i++){
      uchar a = i < pass_len ? (uchar)field[1][i] : 0;
      uchar b = i < password_length ? password[i] : 0;
      difference |= a ^ b;
    }
    if(user_len == name_length && memcmp(field[0], name, name_length) == 0 &&
       difference == 0){
      strncpy(home, field[5], 63);
      home[63] = 0;
      strncpy(shell, field[6], 63);
      shell[63] = 0;
      memset(buf, 0, sizeof(buf));
      return 0;
    }
  }
  memset(buf, 0, sizeof(buf));
  return -1;
}

// RFC 4251 mpint: minimal two's-complement big-endian representation. RFC
// 8731 defines K by interpreting the 32 X25519 output octets as an unsigned
// fixed-length integer in network byte order.
static int
append_shared_secret_mpint(uchar *p, int capacity, int *offset,
                           const uchar shared[32])
{
  int first = 0;
  while(first < 32 && shared[first] == 0)
    first++;
  int length = 32 - first;
  int zero_prefix = length > 0 && (shared[first] & 0x80);
  if(append_u32(p, capacity, offset, length + zero_prefix) < 0)
    return -1;
  if(zero_prefix){
    uchar zero = 0;
    if(append_bytes(p, capacity, offset, &zero, 1) < 0)
      return -1;
  }
  return append_bytes(p, capacity, offset, shared + first, length);
}

static int
load_host_key(uchar seed[32], uchar public_key[32], uchar secret_key[64])
{
  static char path[] = "/etc/ssh_host_ed25519_key";
  int fd = open(path, O_RDONLY);
  if(fd >= 0){
    int n = read(fd, seed, 32);
    uchar extra;
    int trailing = read(fd, &extra, 1);
    close(fd);
    if(n != 32 || trailing != 0){
      printf("sshd: invalid %s (expected 32 raw bytes)\n", path);
      return -1;
    }
  } else {
    if(ssh_random(seed, 32) < 0){
      printf("sshd: hardware RNG unavailable\n");
      return -1;
    }
    fd = open(path, O_CREATE | O_WRONLY | O_TRUNC);
    if(fd < 0 || writen(fd, seed, 32) < 0){
      if(fd >= 0)
        close(fd);
      memset(seed, 0, 32);
      printf("sshd: cannot create %s\n", path);
      return -1;
    }
    close(fd);
    printf("sshd: generated Ed25519 host key %s\n", path);
  }
  if(ssh_ed25519_keypair(public_key, secret_key, seed) < 0){
    memset(seed, 0, 32);
    return -1;
  }
  return 0;
}

static int
build_host_key_blob(uchar *blob, int capacity, const uchar public_key[32])
{
  int off = 0;
  if(append_string(blob, capacity, &off, "ssh-ed25519") < 0 ||
     append_blob(blob, capacity, &off, public_key, 32) < 0)
    return -1;
  return off;
}

static int
build_exchange_hash(uchar hash[32], char *client_ident, char *server_ident,
                    uchar *client_kexinit, int client_kexinit_len,
                    uchar *server_kexinit, int server_kexinit_len,
                    uchar *host_blob, int host_blob_len,
                    uchar client_ephemeral[32], uchar server_ephemeral[32],
                    uchar shared[32])
{
  uchar *encoded = malloc(SSH_PACKET_MAX);
  int off = 0;
  if(encoded == 0)
    return -1;
  if(append_blob(encoded, SSH_PACKET_MAX, &off, client_ident,
                 strlen(client_ident)) < 0 ||
     append_blob(encoded, SSH_PACKET_MAX, &off, server_ident,
                 strlen(server_ident)) < 0 ||
     append_blob(encoded, SSH_PACKET_MAX, &off, client_kexinit,
                 client_kexinit_len) < 0 ||
     append_blob(encoded, SSH_PACKET_MAX, &off, server_kexinit,
                 server_kexinit_len) < 0 ||
     append_blob(encoded, SSH_PACKET_MAX, &off, host_blob, host_blob_len) < 0 ||
     append_blob(encoded, SSH_PACKET_MAX, &off, client_ephemeral, 32) < 0 ||
     append_blob(encoded, SSH_PACKET_MAX, &off, server_ephemeral, 32) < 0 ||
     append_shared_secret_mpint(encoded, SSH_PACKET_MAX, &off, shared) < 0){
    free(encoded);
    return -1;
  }
  int result = ssh_sha256(hash, encoded, off);
  memset(encoded, 0, SSH_PACKET_MAX);
  free(encoded);
  return result;
}

static int
build_kex_reply(uchar *reply, int capacity, uchar *host_blob,
                int host_blob_len, uchar server_ephemeral[32],
                uchar signature[64])
{
  uchar signature_blob[4 + 11 + 4 + 64];
  int signature_len = 0, off = 0;
  if(append_string(signature_blob, sizeof(signature_blob), &signature_len,
                   "ssh-ed25519") < 0 ||
     append_blob(signature_blob, sizeof(signature_blob), &signature_len,
                 signature, 64) < 0)
    return -1;
  reply[off++] = SSH_MSG_KEX_ECDH_REPLY;
  if(append_blob(reply, capacity, &off, host_blob, host_blob_len) < 0 ||
     append_blob(reply, capacity, &off, server_ephemeral, 32) < 0 ||
     append_blob(reply, capacity, &off, signature_blob, signature_len) < 0)
    return -1;
  return off;
}

static int
derive_key(uchar *out, int length, uchar shared[32], uchar hash[32],
           int letter, uchar session_id[32])
{
  uchar encoded[128], digest[32];
  uchar letter_byte = letter;
  int off = 0;
  if(length > 32 ||
     append_shared_secret_mpint(encoded, sizeof(encoded), &off, shared) < 0 ||
     append_bytes(encoded, sizeof(encoded), &off, hash, 32) < 0 ||
     append_bytes(encoded, sizeof(encoded), &off, &letter_byte, 1) < 0 ||
     append_bytes(encoded, sizeof(encoded), &off, session_id, 32) < 0 ||
     ssh_sha256(digest, encoded, off) < 0)
    return -1;
  memmove(out, digest, length);
  memset(encoded, 0, sizeof(encoded));
  memset(digest, 0, sizeof(digest));
  return 0;
}

static int
transport_keys(struct ssh_transport *transport, uchar shared[32],
               uchar hash[32])
{
  uchar client_iv[16], server_iv[16], client_key[16], server_key[16];
  if(derive_key(client_iv, 16, shared, hash, 'A', hash) < 0 ||
     derive_key(server_iv, 16, shared, hash, 'B', hash) < 0 ||
     derive_key(client_key, 16, shared, hash, 'C', hash) < 0 ||
     derive_key(server_key, 16, shared, hash, 'D', hash) < 0 ||
     derive_key(transport->mac_in, 32, shared, hash, 'E', hash) < 0 ||
     derive_key(transport->mac_out, 32, shared, hash, 'F', hash) < 0)
    return -1;
  ssh_aes128_ctr_init(&transport->cipher_in, client_key, client_iv);
  ssh_aes128_ctr_init(&transport->cipher_out, server_key, server_iv);
  memset(client_iv, 0, sizeof(client_iv));
  memset(server_iv, 0, sizeof(server_iv));
  memset(client_key, 0, sizeof(client_key));
  memset(server_key, 0, sizeof(server_key));
  return 0;
}

static int
build_kexinit(uchar *p, int capacity, uint32 *random)
{
  int off = 0;
  p[off++] = SSH_MSG_KEXINIT;
  if(ssh_random(p + off, 16) < 0)
    return -1;
  off += 16;

  // These are the algorithms targeted by the next implementation stage.
  // This bring-up daemon disconnects before accepting a shell because key
  // exchange, host-key signing and encryption are not implemented yet.
  char *lists[] = {
    "curve25519-sha256",
    "ssh-ed25519",
    "aes128-ctr", "aes128-ctr",
    "hmac-sha2-256", "hmac-sha2-256",
    "none", "none",
    "", ""
  };
  for(int i = 0; i < 10; i++)
    if(append_string(p, capacity, &off, lists[i]) < 0)
      return -1;
  if(off + 5 > capacity)
    return -1;
  p[off++] = 0;                 // first_kex_packet_follows = false
  put32(p + off, 0);            // reserved
  off += 4;
  return off;
}

static int
copy_name_list(char *dst, int dst_size, uchar *payload, int payload_len,
               int *offset)
{
  if(*offset + 4 > payload_len)
    return -1;
  uint32 n = get32(payload + *offset);
  *offset += 4;
  if(n > (uint32)(payload_len - *offset))
    return -1;
  int copy = n < (uint32)(dst_size - 1) ? n : dst_size - 1;
  memmove(dst, payload + *offset, copy);
  dst[copy] = 0;
  *offset += n;
  return 0;
}

static int
parse_curve25519_init(uchar *payload, int payload_len, uchar public_key[32])
{
  if(payload_len != 1 + 4 + 32 || payload[0] != SSH_MSG_KEX_ECDH_INIT ||
     get32(payload + 1) != 32)
    return -1;
  memmove(public_key, payload + 5, 32);
  return 0;
}

static void
send_disconnect(int fd, char *description, uint32 *random,
                struct ssh_transport *transport)
{
  uchar payload[256];
  int off = 0;
  payload[off++] = SSH_MSG_DISCONNECT;
  if(append_u32(payload, sizeof(payload), &off,
                SSH_DISCONNECT_KEY_EXCHANGE_FAILED) < 0 ||
     append_string(payload, sizeof(payload), &off, description) < 0 ||
     append_string(payload, sizeof(payload), &off, "") < 0)
    return;
  packet_write(fd, payload, off, random, transport);
}

struct ssh_kex_state {
  struct ssh_transport transport;
  char client_ident[256];
  char client_kex[160];
  char client_hostkey[160];
  uchar client_ephemeral[32];
  uchar server_secret[32];
  uchar server_ephemeral[32];
  uchar shared[32];
  uchar host_seed[32];
  uchar host_public[32];
  uchar host_secret[64];
  uchar host_blob[64];
  uchar hash[32];
  uchar signature[64];
};

static int
send_userauth_failure(int fd, uint32 *random, struct ssh_transport *transport)
{
  uchar p[64];
  int off = 0;
  p[off++] = SSH_MSG_USERAUTH_FAILURE;
  if(append_string(p, sizeof(p), &off, "password") < 0)
    return -1;
  p[off++] = 0; // partial success = false
  return packet_write(fd, p, off, random, transport);
}

static __attribute__((noinline)) int
userauth(int fd, uchar *payload, uint32 *random,
         struct ssh_transport *transport, char home[64], char shell[64])
{
  for(int attempt = 0; attempt < 6; attempt++){
    int n = packet_read(fd, payload, SSH_PACKET_MAX, transport);
    int off = 1;
    const uchar *name, *service, *method, *password;
    uint32 name_len, service_len, method_len, password_len;
    if(n < 1 || payload[0] != SSH_MSG_USERAUTH_REQUEST ||
       parse_string(payload, n, &off, &name, &name_len) < 0 ||
       parse_string(payload, n, &off, &service, &service_len) < 0 ||
       parse_string(payload, n, &off, &method, &method_len) < 0 ||
       !string_is(service, service_len, "ssh-connection")){
      printf("sshd: malformed SSH_MSG_USERAUTH_REQUEST\n");
      return -1;
    }
    if(string_is(method, method_len, "password")){
      if(off >= n || payload[off++] != 0 ||
         parse_string(payload, n, &off, &password, &password_len) < 0 ||
         off != n)
        return -1;
      if(authenticate(name, name_len, password, password_len, home, shell) == 0){
        payload[0] = SSH_MSG_USERAUTH_SUCCESS;
        if(packet_write(fd, payload, 1, random, transport) < 0)
          return -1;
        char user[32];
        if(copy_string(user, sizeof(user), name, name_len) < 0)
          strcpy(user, "<long-name>");
        printf("sshd: password authentication accepted user=%s\n", user);
        memset(user, 0, sizeof(user));
        return 0;
      }
      printf("sshd: password authentication rejected\n");
    }
    // RFC 4252 requires the "none" probe and unsupported methods to receive
    // the same failure packet listing the methods that may continue.
    if(send_userauth_failure(fd, random, transport) < 0)
      return -1;
  }
  return -1;
}

static int
send_channel_status(int fd, int success, uint32 remote_channel,
                    uint32 *random, struct ssh_transport *transport)
{
  uchar p[5];
  p[0] = success ? SSH_MSG_CHANNEL_SUCCESS : SSH_MSG_CHANNEL_FAILURE;
  put32(p + 1, remote_channel);
  return packet_write(fd, p, sizeof(p), random, transport);
}

static int
send_window_adjust(int fd, uint32 remote_channel, uint32 bytes,
                   uint32 *random, struct ssh_transport *transport)
{
  uchar p[9];
  p[0] = SSH_MSG_CHANNEL_WINDOW_ADJUST;
  put32(p + 1, remote_channel);
  put32(p + 5, bytes);
  return packet_write(fd, p, sizeof(p), random, transport);
}

static int
send_channel_data(int fd, uint32 remote_channel, const uchar *data, int length,
                  uint32 *random, struct ssh_transport *transport)
{
  uchar *p;
  int off = 0;
  if(length < 0 || length > SSH_CHANNEL_PACKET ||
     (p = malloc(9 + length)) == 0)
    return -1;
  p[off++] = SSH_MSG_CHANNEL_DATA;
  if(append_u32(p, 9 + length, &off, remote_channel) < 0 ||
     append_blob(p, 9 + length, &off, data, length) < 0){
    free(p);
    return -1;
  }
  int result = packet_write(fd, p, off, random, transport);
  free(p);
  return result;
}

static int
spawn_shell(int network_fd, int pty_fd[2], char home[64], char shell[64])
{
  int pid = fork();
  if(pid != 0)
    return pid;
  close(pty_fd[0]);
  close(network_fd);
  if(ssh_listener >= 0)
    close(ssh_listener);
  close(0);
  dup(pty_fd[1]);
  close(1);
  dup(pty_fd[1]);
  close(2);
  dup(pty_fd[1]);
  if(pty_fd[1] > 2)
    close(pty_fd[1]);
  if(home[0] && chdir(home) < 0)
    chdir("/");
  char *argv[] = { "-sh", home, 0 };
  exec(shell, argv);
  exec("/bin/sh", argv);
  exit(127);
}

static int
relay_shell(int fd, int master, int child, uint32 local_channel,
            uint32 remote_channel, uint32 remote_window,
            uint32 remote_max_packet, uchar *payload, uint32 *random,
            struct ssh_transport *transport)
{
  struct epoll_event event, events[2];
  uchar *terminal = malloc(SSH_CHANNEL_PACKET);
  if(terminal == 0)
    return -1;
  int epfd = epoll_create();
  if(epfd < 0){
    free(terminal);
    return -1;
  }
  event.events = EPOLLIN;
  event.data.fd = fd;
  if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) < 0)
    goto failed;
  event.data.fd = master;
  if(epoll_ctl(epfd, EPOLL_CTL_ADD, master, &event) < 0)
    goto failed;

  for(;;){
    int ready = epoll_wait(epfd, events, 2, -1);
    if(ready < 0)
      break;
    for(int i = 0; i < ready; i++){
      if(events[i].data.fd == fd){
        int n = packet_read(fd, payload, SSH_PACKET_MAX, transport);
        if(n < 1)
          goto done;
        if(payload[0] == SSH_MSG_CHANNEL_WINDOW_ADJUST && n == 9 &&
           get32(payload + 1) == local_channel){
          uint32 added = get32(payload + 5);
          if(0xffffffffU - remote_window < added)
            remote_window = 0xffffffffU;
          else
            remote_window += added;
        } else if(payload[0] == SSH_MSG_CHANNEL_DATA && n >= 9 &&
                  get32(payload + 1) == local_channel){
          uint32 length = get32(payload + 5);
          if(length > SSH_CHANNEL_PACKET || length > (uint32)(n - 9) ||
             writen(master, payload + 9, length) < 0 ||
             send_window_adjust(fd, remote_channel, length, random,
                                transport) < 0)
            goto done;
        } else if(payload[0] == SSH_MSG_CHANNEL_EOF && n == 5 &&
                  get32(payload + 1) == local_channel){
          uchar eof = 4;
          write(master, &eof, 1);
        } else if(payload[0] == SSH_MSG_CHANNEL_CLOSE && n == 5 &&
                  get32(payload + 1) == local_channel){
          uchar close_packet[5];
          close_packet[0] = SSH_MSG_CHANNEL_CLOSE;
          put32(close_packet + 1, remote_channel);
          packet_write(fd, close_packet, sizeof(close_packet), random,
                       transport);
          goto done;
        }
      } else if(events[i].data.fd == master){
        if(events[i].events & EPOLLERR)
          goto shell_eof;
        uint32 limit = remote_max_packet;
        if(limit > SSH_CHANNEL_PACKET)
          limit = SSH_CHANNEL_PACKET;
        if(limit > remote_window)
          limit = remote_window;
        if(limit == 0)
          continue;
        int n = read(master, terminal, limit);
        if(n <= 0)
          goto shell_eof;
        if(send_channel_data(fd, remote_channel, terminal, n, random,
                             transport) < 0)
          goto done;
        remote_window -= n;
      }
    }
  }
shell_eof: {
    uchar p[5];
    p[0] = SSH_MSG_CHANNEL_EOF;
    put32(p + 1, remote_channel);
    packet_write(fd, p, sizeof(p), random, transport);
    p[0] = SSH_MSG_CHANNEL_CLOSE;
    packet_write(fd, p, sizeof(p), random, transport);
  }
done:
  close(epfd);
  free(terminal);
  kill(child);
  wait(0);
  return 0;
failed:
  close(epfd);
  free(terminal);
  return -1;
}

static __attribute__((noinline)) int
channel_session(int fd, uchar *payload, uint32 *random,
                struct ssh_transport *transport, char home[64], char shell[64])
{
  int n = packet_read(fd, payload, SSH_PACKET_MAX, transport), off = 1;
  const uchar *type;
  uint32 type_len, remote_channel, remote_window, remote_max_packet;
  const uint32 local_channel = 0;
  if(n < 1 || payload[0] != SSH_MSG_CHANNEL_OPEN ||
     parse_string(payload, n, &off, &type, &type_len) < 0 ||
     parse_u32(payload, n, &off, &remote_channel) < 0 ||
     parse_u32(payload, n, &off, &remote_window) < 0 ||
     parse_u32(payload, n, &off, &remote_max_packet) < 0 || off != n ||
     remote_max_packet == 0 ||
     !string_is(type, type_len, "session")){
    printf("sshd: expected session channel open\n");
    return -1;
  }
  uchar confirmation[17];
  confirmation[0] = SSH_MSG_CHANNEL_OPEN_CONFIRMATION;
  put32(confirmation + 1, remote_channel);
  put32(confirmation + 5, local_channel);
  put32(confirmation + 9, SSH_CHANNEL_WINDOW);
  put32(confirmation + 13, SSH_CHANNEL_PACKET);
  if(packet_write(fd, confirmation, sizeof(confirmation), random, transport) < 0)
    return -1;
  printf("sshd: session channel opened local=%d remote=%d\n",
         local_channel, remote_channel);

  for(;;){
    n = packet_read(fd, payload, SSH_PACKET_MAX, transport);
    off = 1;
    uint32 recipient, request_len;
    const uchar *request;
    if(n < 1)
      return -1;
    if(payload[0] == SSH_MSG_CHANNEL_CLOSE)
      return 0;
    if(payload[0] != SSH_MSG_CHANNEL_REQUEST ||
       parse_u32(payload, n, &off, &recipient) < 0 ||
       parse_string(payload, n, &off, &request, &request_len) < 0 ||
       off >= n || recipient != local_channel)
      return -1;
    int want_reply = payload[off++] != 0;
    if(string_is(request, request_len, "pty-req")){
      const uchar *term, *modes;
      uint32 term_len, columns, rows, width, height, modes_len;
      int valid = parse_string(payload, n, &off, &term, &term_len) == 0 &&
                  parse_u32(payload, n, &off, &columns) == 0 &&
                  parse_u32(payload, n, &off, &rows) == 0 &&
                  parse_u32(payload, n, &off, &width) == 0 &&
                  parse_u32(payload, n, &off, &height) == 0 &&
                  parse_string(payload, n, &off, &modes, &modes_len) == 0 &&
                  off == n;
      (void)term; (void)term_len; (void)columns; (void)rows;
      (void)width; (void)height; (void)modes; (void)modes_len;
      if(want_reply && send_channel_status(fd, valid, remote_channel, random,
                                           transport) < 0)
        return -1;
      if(valid)
        printf("sshd: PTY request accepted\n");
    } else if(string_is(request, request_len, "shell") && off == n){
      int pty_fd[2];
      if(pty_open(pty_fd) < 0){
        if(want_reply)
          send_channel_status(fd, 0, remote_channel, random, transport);
        return -1;
      }
      int child = spawn_shell(fd, pty_fd, home, shell);
      if(child < 0){
        close(pty_fd[0]);
        close(pty_fd[1]);
        return -1;
      }
      close(pty_fd[1]);
      if(want_reply && send_channel_status(fd, 1, remote_channel, random,
                                           transport) < 0){
        close(pty_fd[0]);
        kill(child);
        wait(0);
        return -1;
      }
      printf("sshd: PTY shell started pid=%d\n", child);
      int result = relay_shell(fd, pty_fd[0], child, local_channel,
                               remote_channel, remote_window,
                               remote_max_packet, payload, random, transport);
      close(pty_fd[0]);
      return result;
    } else {
      // OpenSSH commonly sends LANG as an env request. xv6 has no process
      // environment yet, so reject it without aborting channel setup.
      if(want_reply && send_channel_status(fd, 0, remote_channel, random,
                                           transport) < 0)
        return -1;
    }
  }
}

// Keep the per-connection protocol frame separate from main(). TweetNaCl's
// Ed25519 call tree has large leaf frames; GCC otherwise inlines this function
// into main and the combined live frame can cross xv6's one-page user stack.
static __attribute__((noinline)) void
serve_connection(int fd)
{
  static char server_ident[] = "SSH-2.0-xv6-aarch64_0.1";
  static char server_ident_line[] = "SSH-2.0-xv6-aarch64_0.1\r\n";
  struct ssh_kex_state *state = 0;
  uchar *payload = 0, *client_kexinit = 0, *server_kexinit = 0;
  uint32 random = (uint32)uptime() ^ ((uint32)getpid() << 16) ^ 0x58d6a73bU;
  int n, server_kexinit_len, client_kexinit_len, host_blob_len, reply_len;
  int offset;

  state = malloc(sizeof(*state));
  if(state == 0)
    return;
  memset(state, 0, sizeof(*state));
  if(writen(fd, server_ident_line, strlen(server_ident_line)) < 0 ||
     read_identification(fd, state->client_ident,
                         sizeof(state->client_ident)) < 0){
    printf("sshd: invalid client identification\n");
    goto out;
  }
  printf("sshd: client %s\n", state->client_ident);

  payload = malloc(SSH_PACKET_MAX);
  client_kexinit = malloc(SSH_PACKET_MAX);
  server_kexinit = malloc(SSH_PACKET_MAX);
  if(payload == 0 || client_kexinit == 0 || server_kexinit == 0)
    goto out;
  server_kexinit_len = build_kexinit(server_kexinit, SSH_PACKET_MAX, &random);
  if(server_kexinit_len < 0 ||
     packet_write(fd, server_kexinit, server_kexinit_len, &random,
                  &state->transport) < 0)
    goto out;
  n = packet_read(fd, payload, SSH_PACKET_MAX, &state->transport);
  if(n < 0 || payload[0] != SSH_MSG_KEXINIT){
    printf("sshd: expected SSH_MSG_KEXINIT\n");
    goto out;
  }
  client_kexinit_len = n;
  memmove(client_kexinit, payload, n);
  offset = 1 + 16;
  if(copy_name_list(state->client_kex, sizeof(state->client_kex),
                    payload, n, &offset) < 0 ||
     copy_name_list(state->client_hostkey, sizeof(state->client_hostkey),
                    payload, n,
                    &offset) < 0){
    printf("sshd: malformed KEXINIT\n");
    goto out;
  }
  printf("sshd: KEXINIT received\n");
  printf("sshd: client kex=%s\n", state->client_kex);
  printf("sshd: client hostkey=%s\n", state->client_hostkey);

  // RFC 8731 encodes the Curve25519 client ephemeral public key as one
  // standard SSH string containing exactly 32 octets.
  n = packet_read(fd, payload, SSH_PACKET_MAX, &state->transport);
  if(n < 0 || parse_curve25519_init(payload, n,
                                    state->client_ephemeral) < 0){
    printf("sshd: malformed SSH_MSG_KEX_ECDH_INIT\n");
    send_disconnect(fd, "invalid Curve25519 public key", &random,
                    &state->transport);
    goto out;
  }
  printf("sshd: Curve25519 client public key received (32 bytes)\n");

  if(load_host_key(state->host_seed, state->host_public,
                   state->host_secret) < 0 ||
     ssh_random(state->server_secret, sizeof(state->server_secret)) < 0 ||
     ssh_x25519_public(state->server_ephemeral, state->server_secret) < 0 ||
     ssh_x25519(state->shared, state->server_secret,
                state->client_ephemeral) < 0){
    printf("sshd: key generation failed\n");
    send_disconnect(fd, "key generation failed", &random, &state->transport);
    goto out;
  }
  host_blob_len = build_host_key_blob(state->host_blob,
                                      sizeof(state->host_blob),
                                      state->host_public);
  if(host_blob_len < 0 ||
     build_exchange_hash(state->hash, state->client_ident, server_ident,
                         client_kexinit, client_kexinit_len,
                         server_kexinit, server_kexinit_len,
                         state->host_blob, host_blob_len,
                         state->client_ephemeral,
                         state->server_ephemeral, state->shared) < 0 ||
     ssh_ed25519_sign(state->signature, state->hash,
                      sizeof(state->hash), state->host_secret) < 0){
    printf("sshd: exchange hash/signature failed\n");
    send_disconnect(fd, "key exchange signing failed", &random,
                    &state->transport);
    goto out;
  }
  reply_len = build_kex_reply(payload, SSH_PACKET_MAX, state->host_blob,
                              host_blob_len, state->server_ephemeral,
                              state->signature);
  if(reply_len < 0 ||
     packet_write(fd, payload, reply_len, &random, &state->transport) < 0)
    goto out;
  printf("sshd: sent SSH_MSG_KEX_ECDH_REPLY type=31\n");

  if(transport_keys(&state->transport, state->shared, state->hash) < 0)
    goto out;
  payload[0] = SSH_MSG_NEWKEYS;
  if(packet_write(fd, payload, 1, &random, &state->transport) < 0)
    goto out;
  state->transport.encrypt_out = 1;
  printf("sshd: sent SSH_MSG_NEWKEYS; outbound encryption active\n");

  n = packet_read(fd, payload, SSH_PACKET_MAX, &state->transport);
  if(n != 1 || payload[0] != SSH_MSG_NEWKEYS){
    printf("sshd: expected SSH_MSG_NEWKEYS\n");
    goto out;
  }
  state->transport.encrypt_in = 1;
  printf("sshd: received SSH_MSG_NEWKEYS; inbound encryption active\n");

  // Decrypt and authenticate the first post-NEWKEYS request. Accepting the
  // ssh-userauth service proves both directions use matching cipher/MAC keys.
  n = packet_read(fd, payload, SSH_PACKET_MAX, &state->transport);
  if(n != 17 || payload[0] != SSH_MSG_SERVICE_REQUEST ||
     get32(payload + 1) != 12 ||
     memcmp(payload + 5, "ssh-userauth", 12) != 0){
    printf("sshd: expected encrypted SSH_MSG_SERVICE_REQUEST\n");
    goto out;
  }
  payload[0] = SSH_MSG_SERVICE_ACCEPT;
  if(packet_write(fd, payload, n, &random, &state->transport) < 0)
    goto out;
  printf("sshd: encrypted transport ready; service accepted\n");

  char home[64], shell[64];
  memset(home, 0, sizeof(home));
  memset(shell, 0, sizeof(shell));
  if(userauth(fd, payload, &random, &state->transport, home, shell) < 0){
    printf("sshd: authentication failed\n");
    goto out;
  }
  if(channel_session(fd, payload, &random, &state->transport,
                     home, shell) < 0)
    printf("sshd: channel ended with protocol/I/O error\n");
out:
  if(state){
    memset(state, 0, sizeof(*state));
    free(state);
  }
  if(client_kexinit)
    free(client_kexinit);
  if(server_kexinit)
    free(server_kexinit);
  free(payload);
}

int
main(int argc, char **argv)
{
  int port = 22;
  if(argc > 1)
    port = atoi(argv[1]);
  ssh_listener = socket_listen(port, 4);
  if(ssh_listener < 0){
    printf("sshd: cannot listen on port %d\n", port);
    exit(1);
  }
  printf("sshd: SSH-2 transport bring-up listening on port %d\n", port);
  for(;;){
    int connection = socket_accept(ssh_listener);
    if(connection < 0)
      continue;
    serve_connection(connection);
    close(connection);
  }
}
