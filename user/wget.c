// wget: download a file over HTTP/1.0.
//
//   wget [-q] [-O file] http://host[:port]/path
//
//   -O file   write the body to file ("-O -" writes to stdout).
//             Default: last path component, or index.html.
//   -q        quiet: no progress/status lines.
//
// Steps: parse URL -> resolve host (dotted IP, or a DNS A query over UDP to
// the DHCP-supplied server) -> socket()+connect() -> send "GET path
// HTTP/1.0" -> parse the status line and headers -> stream the body to the
// output file.  3xx redirects (Location: http://...) are followed up to 5
// times.  HTTPS is not supported (no TLS in xv6).
#include <uapi/types.h>
#include <uapi/socket.h>
#include <fcntl.h>
#include "user/user.h"

#define DNS_PORT_LOCAL 53053
#define DNS_TIMEOUT    20       // ticks (100 ms each)
#define MAXREDIR       5

static int quiet;

struct url {
  char host[128];
  int port;
  char path[512];
};

static uint16
htons16(uint16 v)
{
  return (v << 8) | (v >> 8);
}

static uint32
htonl32(uint32 v)
{
  return ((v & 0xff) << 24) | ((v & 0xff00) << 8) |
         ((v >> 8) & 0xff00) | (v >> 24);
}

static void
putip(int fd, uint32 ip)
{
  fprintf(fd, "%d.%d.%d.%d", (ip >> 24) & 255, (ip >> 16) & 255,
          (ip >> 8) & 255, ip & 255);
}

static int
lower(int c)
{
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int
strncaseeq(const char *a, const char *b, int n)
{
  for(int i = 0; i < n; i++){
    if(lower(a[i]) != lower(b[i]))
      return 0;
    if(a[i] == 0)
      return 1;
  }
  return 1;
}

// "http://host[:port][/path]"  (scheme optional)
static int
parse_url(const char *s, struct url *u)
{
  int i;

  if(strncaseeq(s, "https://", 8)){
    fprintf(2, "wget: https is not supported (no TLS); use http://\n");
    return -1;
  }
  if(strncaseeq(s, "http://", 7))
    s += 7;
  for(i = 0; *s && *s != ':' && *s != '/' && i < (int)sizeof(u->host) - 1; i++)
    u->host[i] = *s++;
  u->host[i] = 0;
  if(i == 0)
    return -1;
  u->port = 80;
  if(*s == ':'){
    s++;
    u->port = atoi(s);
    while(*s >= '0' && *s <= '9')
      s++;
    if(u->port <= 0 || u->port > 65535)
      return -1;
  }
  if(*s == 0)
    strcpy(u->path, "/");
  else {
    if(*s != '/' || strlen(s) >= sizeof(u->path))
      return -1;
    strcpy(u->path, s);
  }
  return 0;
}

// Dotted-quad literal -> host-order IP.
static int
parse_ip(const char *s, uint32 *ip)
{
  uint32 v = 0;
  for(int part = 0; part < 4; part++){
    int n = 0, digits = 0;
    while(*s >= '0' && *s <= '9'){
      n = n * 10 + (*s++ - '0');
      digits++;
    }
    if(digits == 0 || n > 255)
      return -1;
    v = (v << 8) | n;
    if(part < 3 && *s++ != '.')
      return -1;
  }
  if(*s)
    return -1;
  *ip = v;
  return 0;
}

// Minimal DNS A-record lookup (RFC 1035).
static int
resolve(const char *host, uint32 *ip)
{
  uchar q[300], r[512];
  uint32 server, src;
  uint16 sport, id;
  int qlen, n, attempt;

  if(parse_ip(host, ip) == 0)
    return 0;
  if((server = net_dns()) == 0){
    fprintf(2, "wget: no DNS server (run dhcp first?)\n");
    return -1;
  }

  id = (uint16)(clock_us() & 0xffff);
  memset(q, 0, 12);
  q[0] = id >> 8; q[1] = id;
  q[2] = 0x01;                 // RD: recursion desired
  q[5] = 1;                    // QDCOUNT = 1
  qlen = 12;
  for(const char *p = host; *p; ){
    const char *dot = strchr(p, '.');
    int len = dot ? dot - p : (int)strlen(p);
    if(len == 0 || len > 63 || qlen + len + 6 > (int)sizeof(q))
      return -1;
    q[qlen++] = len;
    memmove(q + qlen, p, len);
    qlen += len;
    p += len;
    if(*p == '.')
      p++;
  }
  q[qlen++] = 0;
  q[qlen++] = 0; q[qlen++] = 1;   // QTYPE  A
  q[qlen++] = 0; q[qlen++] = 1;   // QCLASS IN

  if(udp_bind(DNS_PORT_LOCAL) < 0){
    fprintf(2, "wget: cannot bind UDP %d\n", DNS_PORT_LOCAL);
    return -1;
  }
  n = -1;
  for(attempt = 0; attempt < 3; attempt++){
    if(udp_send(server, DNS_PORT_LOCAL, 53, q, qlen) < 0)
      continue;
    n = udp_recv_timeout(DNS_PORT_LOCAL, &src, &sport, r, sizeof(r),
                         DNS_TIMEOUT);
    if(n >= 12 && r[0] == q[0] && r[1] == q[1])
      break;
    n = -1;
  }
  udp_unbind(DNS_PORT_LOCAL);
  if(n < 12 || (r[3] & 0x0f) != 0){
    fprintf(2, "wget: cannot resolve %s\n", host);
    return -1;
  }

  int qd = (r[4] << 8) | r[5];
  int an = (r[6] << 8) | r[7];
  int pos = 12;
  // Skip the question section(s).
  for(int i = 0; i < qd; i++){
    while(pos < n && r[pos] != 0){
      if((r[pos] & 0xc0) == 0xc0){ pos++; break; }
      pos += r[pos] + 1;
    }
    pos += 5;
  }
  // Walk answers; CNAMEs come first, return the first A record.
  for(int i = 0; i < an && pos < n; i++){
    while(pos < n){
      if((r[pos] & 0xc0) == 0xc0){ pos += 2; break; }
      if(r[pos] == 0){ pos++; break; }
      pos += r[pos] + 1;
    }
    if(pos + 10 > n)
      break;
    int type = (r[pos] << 8) | r[pos + 1];
    int rdlen = (r[pos + 8] << 8) | r[pos + 9];
    pos += 10;
    if(type == 1 && rdlen == 4 && pos + 4 <= n){
      *ip = ((uint32)r[pos] << 24) | ((uint32)r[pos + 1] << 16) |
            ((uint32)r[pos + 2] << 8) | r[pos + 3];
      return 0;
    }
    pos += rdlen;
  }
  fprintf(2, "wget: no A record for %s\n", host);
  return -1;
}

static int
writeall(int fd, const char *buf, int n)
{
  int done = 0;
  while(done < n){
    int w = write(fd, buf + done, n - done);
    if(w <= 0)
      return -1;
    done += w;
  }
  return 0;
}

static const char *
default_name(const char *path)
{
  const char *b = path;
  for(const char *p = path; *p && *p != '?'; p++)
    if(*p == '/')
      b = p + 1;
  if(*b == 0 || *b == '?')
    return "index.html";
  return b;
}

// Read the response head (up to and including the blank line) into hdr.
// Any body bytes that arrived in the same reads are left in hdr after it;
// *bodyoff / *have describe them.
static int
read_head(int s, char *hdr, int cap, int *bodyoff, int *have)
{
  int len = 0;
  while(len < cap - 1){
    int n = read(s, hdr + len, cap - 1 - len);
    if(n <= 0)
      return -1;
    len += n;
    hdr[len] = 0;
    for(int i = 3; i < len; i++){
      if(hdr[i-3] == '\r' && hdr[i-2] == '\n' &&
         hdr[i-1] == '\r' && hdr[i] == '\n'){
        *bodyoff = i + 1;
        *have = len;
        return 0;
      }
    }
  }
  return -1;
}

// Find header "name:" in the head; copy its value into out.
static int
header(const char *hdr, const char *name, char *out, int cap)
{
  int nl = strlen(name);
  const char *p = strchr(hdr, '\n');
  while(p && p[1]){
    p++;
    if(strncaseeq(p, name, nl) && p[nl] == ':'){
      p += nl + 1;
      while(*p == ' ' || *p == '\t')
        p++;
      int i = 0;
      while(*p && *p != '\r' && *p != '\n' && i < cap - 1)
        out[i++] = *p++;
      out[i] = 0;
      return 0;
    }
    p = strchr(p, '\n');
  }
  return -1;
}

// One HTTP transaction.  Returns 0 on success, 1 when *u was replaced by a
// redirect target, -1 on error.
static int
fetch(struct url *u, const char *outname)
{
  static char hdr[4096];
  static char buf[2048];
  char req[800], loc[512], val[32];
  struct sockaddr_in sa;
  uint32 ip;
  int s, bodyoff, have, status, out;
  long total = 0, clen = -1;

  if(resolve(u->host, &ip) < 0)
    return -1;
  if(!quiet){
    fprintf(2, "Connecting to %s (", u->host);
    putip(2, ip);
    fprintf(2, "):%d... ", u->port);
  }
  if((s = socket(AF_INET, SOCK_STREAM, 0)) < 0){
    fprintf(2, "wget: socket failed\n");
    return -1;
  }
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons16(u->port);
  sa.sin_addr.s_addr = htonl32(ip);
  if(connect(s, &sa, sizeof(sa)) < 0){
    fprintf(2, "failed: connection refused or timed out\n");
    close(s);
    return -1;
  }
  if(!quiet)
    fprintf(2, "connected.\n");

  // HTTP/1.0 + Connection: close -> no chunked encoding, EOF ends the body.
  int rl = 0;
  const char *parts[] = {
    "GET ", u->path, " HTTP/1.0\r\nHost: ", u->host, 0
  };
  req[0] = 0;
  for(int i = 0; parts[i]; i++)
    strcat(req, parts[i]);
  if(u->port != 80){
    char pbuf[8]; int k = 0, p = u->port; char t[8]; int tn = 0;
    while(p){ t[tn++] = '0' + p % 10; p /= 10; }
    pbuf[k++] = ':';
    while(tn) pbuf[k++] = t[--tn];
    pbuf[k] = 0;
    strcat(req, pbuf);
  }
  strcat(req, "\r\nUser-Agent: xv6-wget/1.0\r\nAccept: */*\r\n"
              "Connection: close\r\n\r\n");
  rl = strlen(req);
  if(writeall(s, req, rl) < 0){
    fprintf(2, "wget: send failed\n");
    close(s);
    return -1;
  }
  if(!quiet)
    fprintf(2, "HTTP request sent, awaiting response... ");

  if(read_head(s, hdr, sizeof(hdr), &bodyoff, &have) < 0){
    fprintf(2, "wget: bad or truncated response header\n");
    close(s);
    return -1;
  }
  // "HTTP/1.x NNN reason"
  char *sp = strchr(hdr, ' ');
  status = sp ? atoi(sp + 1) : 0;
  if(!quiet){
    char *eol = strchr(hdr, '\r');
    if(sp && eol){ *eol = 0; fprintf(2, "%s\n", sp + 1); *eol = '\r'; }
  }

  if(status >= 300 && status < 400 && header(hdr, "Location", loc, sizeof(loc)) == 0){
    close(s);
    if(!quiet)
      fprintf(2, "Location: %s [following]\n", loc);
    if(loc[0] == '/'){
      if(strlen(loc) >= sizeof(u->path))
        return -1;
      strcpy(u->path, loc);
      return 1;
    }
    struct url nu;
    if(parse_url(loc, &nu) < 0)
      return -1;
    *u = nu;
    return 1;
  }
  if(status != 200){
    fprintf(2, "wget: server returned %d\n", status);
    close(s);
    return -1;
  }
  if(header(hdr, "Content-Length", val, sizeof(val)) == 0)
    clen = atoi(val);
  if(!quiet){
    fprintf(2, "Length: ");
    if(clen >= 0) fprintf(2, "%d", (int)clen); else fprintf(2, "unspecified");
    fprintf(2, "\nSaving to: '%s'\n", outname);
  }

  if(strcmp(outname, "-") == 0)
    out = 1;
  else if((out = open(outname, O_CREATE | O_WRONLY | O_TRUNC)) < 0){
    fprintf(2, "wget: cannot create %s\n", outname);
    close(s);
    return -1;
  }

  uint64 t0 = clock_us();
  if(have > bodyoff){
    if(writeall(out, hdr + bodyoff, have - bodyoff) < 0)
      goto werr;
    total += have - bodyoff;
  }
  long next_report = 64 * 1024;
  while(clen < 0 || total < clen){
    int n = read(s, buf, sizeof(buf));
    if(n < 0){
      fprintf(2, "\nwget: read error after %d bytes\n", (int)total);
      break;
    }
    if(n == 0)
      break;                    // server closed: end of body
    if(writeall(out, buf, n) < 0)
      goto werr;
    total += n;
    if(!quiet && total >= next_report){
      fprintf(2, "  %d KB\n", (int)(total / 1024));
      next_report += 64 * 1024;
    }
  }
  close(s);
  if(out != 1)
    close(out);
  if(!quiet){
    uint64 us = clock_us() - t0;
    int kbs = us ? (int)(total * 1000000 / 1024 / us) : 0;
    fprintf(2, "'%s' saved [%d", outname, (int)total);
    if(clen >= 0) fprintf(2, "/%d", (int)clen);
    fprintf(2, "] %d KB/s\n", kbs);
  }
  return clen >= 0 && total != clen ? -1 : 0;

werr:
  fprintf(2, "wget: write to %s failed\n", outname);
  close(s);
  if(out != 1)
    close(out);
  return -1;
}

static void
usage(void)
{
  fprintf(2, "usage: wget [-q] [-O file] http://host[:port]/path\n");
  exit(1);
}

int
main(int argc, char *argv[])
{
  struct url u;
  const char *outname = 0, *src = 0;
  char namebuf[128];
  int r;

  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "-q") == 0)
      quiet = 1;
    else if(strcmp(argv[i], "-O") == 0 && i + 1 < argc)
      outname = argv[++i];
    else if(argv[i][0] == '-' && argv[i][1])
      usage();
    else if(src == 0)
      src = argv[i];
    else
      usage();
  }
  if(src == 0 || parse_url(src, &u) < 0)
    usage();

  for(int hops = 0; hops <= MAXREDIR; hops++){
    if(outname == 0){
      strncpy(namebuf, default_name(u.path), sizeof(namebuf) - 1);
      namebuf[sizeof(namebuf) - 1] = 0;
    }
    r = fetch(&u, outname ? outname : namebuf);
    if(r == 0)
      exit(0);
    if(r < 0)
      exit(1);
  }
  fprintf(2, "wget: too many redirects\n");
  exit(1);
}
