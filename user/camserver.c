// camserver: HTTP camera server (live MJPEG stream + snapshots).
//
//   camserver [-p PORT] [-q QUALITY] [-s] [-w] [-n CLIENTS]
//
//   -p  listen port (default 8080)
//   -q  JPEG quality 1..100 (default 75)
//   -s  half size, 320x240 (smaller frames, higher frame rate over Wi-Fi)
//   -w  skip gray-world white balance
//   -n  maximum simultaneous stream viewers (default 4, at most MAXCLIENTS)
//
// Endpoints (open http://<wlan0-ip>:8080/ in a browser):
//
//   /               HTML page that shows the live stream
//   /stream         multipart/x-mixed-replace MJPEG stream
//   /snapshot.jpg   one JPEG frame
//   /status         plain-text counters
//
// One process serves everyone.  Each captured frame is converted to JPEG
// once and written to every stream viewer, so N viewers cost one capture
// (/dev/video0 has a single sensor stream; separate opens would restart it).
// The sensor is powered only while somebody is watching: when the last viewer
// leaves, /dev/video0 is closed and the kernel stops the stream.
//
// xv6 sockets belong to the process that accepted them, so the server does
// not fork.  New connections are picked up between frames with a zero-timeout
// epoll_wait(); a request whose headers do not arrive within REQ_TIMEOUT_MS
// (for example a browser's speculative pre-connection) is dropped instead of
// stalling the stream.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/camera.h"
#include "kernel/epoll.h"
#include "user/user.h"
#include "user/camproc.h"

#define BACKLOG         4
#define MAXCLIENTS      4
#define REQ_CAP         1024
#define REQ_TIMEOUT_MS  2000
#define JPEG_CAP        (256 * 1024)

static int port = 8080, quality = 75, half, wb = 1, maxclients = MAXCLIENTS;

static int clients[MAXCLIENTS];       // fds receiving /stream
static int nclients;

static int cam = -1;                  // /dev/video0 while someone is watching
static uchar *frame;                  // raw capture, CAM_READ_MAX bytes
static uchar *jpeg;                   // last encoded frame
static uint jpeg_len;                 // 0 = no valid frame yet

// Counters for /status and the console.
static uint frames, served_snapshots, total_viewers;
static uint64 window_start_us;
static uint window_frames;
static uint fps10;                    // frames per second * 10, last window

static int
write_all(int fd, const void *buf, int n)
{
  int done = 0;
  while(done < n){
    int m = write(fd, (const char *)buf + done, n - done);
    if(m <= 0)
      return -1;
    done += m;
  }
  return 0;
}

static int
put_uint(char *dst, uint v)
{
  char tmp[16];
  int n = 0;
  do {
    tmp[n++] = '0' + v % 10;
    v /= 10;
  } while(v);
  for(int i = 0; i < n; i++)
    dst[i] = tmp[n - 1 - i];
  dst[n] = 0;
  return n;
}

// Append string s to buf at *n.
static void
cat(char *buf, int *n, const char *s)
{
  int len = strlen(s);
  memmove(buf + *n, s, len);
  *n += len;
  buf[*n] = 0;
}

static void
cat_uint(char *buf, int *n, uint v)
{
  *n += put_uint(buf + *n, v);
}

// fps10 as "12.3".
static void
cat_fps(char *buf, int *n)
{
  cat_uint(buf, n, fps10 / 10);
  cat(buf, n, ".");
  cat_uint(buf, n, fps10 % 10);
}

static void
send_simple(int fd, const char *status, const char *type, const char *body)
{
  char head[256];
  int n = 0;
  cat(head, &n, "HTTP/1.1 ");
  cat(head, &n, status);
  cat(head, &n, "\r\nContent-Type: ");
  cat(head, &n, type);
  cat(head, &n, "\r\nContent-Length: ");
  cat_uint(head, &n, strlen(body));
  cat(head, &n, "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
  if(write_all(fd, head, n) == 0)
    write_all(fd, body, strlen(body));
}

// ---------------------------------------------------------------------------
// Camera.

static void
camera_close(void)
{
  if(cam >= 0){
    close(cam);                       // last reference: kernel stops sensor
    cam = -1;
    printf("camserver: no viewers, camera stopped\n");
  }
}

// Capture one frame and convert it to JPEG.  Returns 0 on success.
static int
capture(void)
{
  if(cam < 0){
    cam = open("/dev/video0", O_RDWR);
    if(cam < 0){
      printf("camserver: cannot open /dev/video0\n");
      return -1;
    }
    printf("camserver: camera opened\n");
    window_start_us = clock_us();
    window_frames = 0;
  }
  int n = read(cam, frame, CAM_READ_MAX);
  if(n <= 0){
    printf("camserver: capture failed\n");
    camera_close();
    return -1;
  }
  uint len = 0;
  if(camproc_jpeg(jpeg, JPEG_CAP, frame, n, half, wb, quality, &len) < 0 ||
     len == 0){
    printf("camserver: JPEG conversion failed\n");
    return -1;
  }
  jpeg_len = len;
  frames++;
  window_frames++;
  uint64 now = clock_us();
  if(now - window_start_us >= 3000000ULL){
    fps10 = (uint)(window_frames * 10000000ULL / (now - window_start_us));
    char line[160];
    int k = 0;
    cat(line, &k, "camserver: ");
    cat_fps(line, &k);
    cat(line, &k, " fps, ");
    cat_uint(line, &k, jpeg_len);
    cat(line, &k, " bytes/frame, ");
    cat_uint(line, &k, nclients);
    cat(line, &k, " viewer(s)\n");
    write(1, line, k);
    window_start_us = now;
    window_frames = 0;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// HTTP requests.

// Wait until fd is readable, up to timeout_ms.  Returns 1 if readable.
static int
wait_readable(int epfd, int fd, int timeout_ms)
{
  struct epoll_event ev, out;
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  if(epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
    return 0;
  int r = epoll_wait(epfd, &out, 1, timeout_ms);
  epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev);
  return r > 0;
}

static int
has_header_end(const char *s)
{
  for(const char *p = s; *p; p++)
    if(p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
      return 1;
  return 0;
}

// Read the request headers.  Every read is preceded by a bounded wait so a
// silent client cannot block the server.
static int
read_request(int epfd, int fd, char *buf, int cap)
{
  int n = 0;
  buf[0] = 0;
  while(n < cap - 1){
    if(!wait_readable(epfd, fd, REQ_TIMEOUT_MS))
      return -1;
    int got = read(fd, buf + n, cap - 1 - n);
    if(got <= 0)
      return -1;
    n += got;
    buf[n] = 0;
    if(has_header_end(buf))
      return n;
  }
  return n;                           // over-long headers: use what we have
}

// Copy the request path ("GET /path HTTP/1.1") without any query string.
static int
request_path(const char *req, char *path, int cap)
{
  if(strncmp(req, "GET ", 4) != 0)
    return -1;
  const char *p = req + 4;
  int n = 0;
  while(*p && *p != ' ' && *p != '?' && n < cap - 1)
    path[n++] = *p++;
  path[n] = 0;
  return n > 0 ? 0 : -1;
}

static void
send_index(int fd)
{
  static const char page[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>xv6 camera</title><style>"
    "body{margin:0;background:#111;color:#ddd;font:14px sans-serif;"
    "text-align:center}"
    "img{max-width:100%;margin-top:12px;background:#000}"
    "a{color:#8cf;margin:0 8px}"
    "</style></head><body>"
    "<img src=\"/stream\" alt=\"live stream\">"
    "<p><a href=\"/snapshot.jpg\">snapshot</a>"
    "<a href=\"/status\">status</a></p>"
    "<p>xv6-aarch64 &middot; OV5647 &middot; MJPEG over HTTP</p>"
    "</body></html>";
  send_simple(fd, "200 OK", "text/html; charset=utf-8", page);
}

static void
send_status(int fd)
{
  char body[384];
  int n = 0;
  body[0] = 0;
  cat(body, &n, "camera: ");
  cat(body, &n, cam >= 0 ? "streaming\n" : "idle\n");
  cat(body, &n, "size: ");
  cat(body, &n, half ? "320x240\n" : "640x480\n");
  cat(body, &n, "quality: ");
  cat_uint(body, &n, quality);
  cat(body, &n, "\nviewers: ");
  cat_uint(body, &n, nclients);
  cat(body, &n, "/");
  cat_uint(body, &n, maxclients);
  cat(body, &n, "\nviewers served: ");
  cat_uint(body, &n, total_viewers);
  cat(body, &n, "\nframes: ");
  cat_uint(body, &n, frames);
  cat(body, &n, "\nfps: ");
  cat_fps(body, &n);
  cat(body, &n, "\nlast frame bytes: ");
  cat_uint(body, &n, jpeg_len);
  cat(body, &n, "\nsnapshots: ");
  cat_uint(body, &n, served_snapshots);
  cat(body, &n, "\n");
  send_simple(fd, "200 OK", "text/plain; charset=utf-8", body);
}

static int
send_jpeg_part(int fd)
{
  char head[128];
  int n = 0;
  cat(head, &n, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ");
  cat_uint(head, &n, jpeg_len);
  cat(head, &n, "\r\n\r\n");
  if(write_all(fd, head, n) < 0 || write_all(fd, jpeg, jpeg_len) < 0 ||
     write_all(fd, "\r\n", 2) < 0)
    return -1;
  return 0;
}

static void
send_snapshot(int fd)
{
  // While streaming, the newest frame is at most one frame old; otherwise
  // power the camera up for a single capture.
  int fresh = cam < 0;
  if(cam < 0 || jpeg_len == 0){
    if(capture() < 0){
      send_simple(fd, "503 Service Unavailable", "text/plain", "no camera\n");
      return;
    }
  }
  char head[256];
  int n = 0;
  cat(head, &n, "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                "Content-Length: ");
  cat_uint(head, &n, jpeg_len);
  cat(head, &n, "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n");
  if(write_all(fd, head, n) == 0)
    write_all(fd, jpeg, jpeg_len);
  served_snapshots++;
  if(fresh && nclients == 0)
    camera_close();
}

// Returns 1 if fd joined the stream (do not close it), 0 otherwise.
static int
start_stream(int fd)
{
  if(nclients >= maxclients){
    send_simple(fd, "503 Service Unavailable", "text/plain",
                "too many viewers\n");
    return 0;
  }
  static const char head[] =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Connection: close\r\n"
      "\r\n";
  if(write_all(fd, head, sizeof(head) - 1) < 0)
    return 0;
  clients[nclients++] = fd;
  total_viewers++;
  printf("camserver: viewer fd=%d joined (%d watching)\n", fd, nclients);
  return 1;
}

// Accept one pending connection and answer it.
static void
handle_connection(int listener, int epfd)
{
  char req[REQ_CAP], path[128];
  int fd = socket_accept(listener);
  if(fd < 0)
    return;
  if(read_request(epfd, fd, req, sizeof(req)) < 0 ||
     request_path(req, path, sizeof(path)) < 0){
    close(fd);                        // silent pre-connection or not a GET
    return;
  }
  if(strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)
    send_index(fd);
  else if(strcmp(path, "/stream") == 0 || strcmp(path, "/stream.mjpg") == 0){
    if(start_stream(fd))
      return;
  } else if(strcmp(path, "/snapshot.jpg") == 0)
    send_snapshot(fd);
  else if(strcmp(path, "/status") == 0)
    send_status(fd);
  else
    send_simple(fd, "404 Not Found", "text/plain", "not found\n");
  close(fd);
}

// Send the current frame to every viewer; drop the ones that went away.
static void
broadcast(void)
{
  for(int i = 0; i < nclients; ){
    if(send_jpeg_part(clients[i]) < 0){
      printf("camserver: viewer fd=%d left\n", clients[i]);
      close(clients[i]);
      clients[i] = clients[--nclients];
      continue;
    }
    i++;
  }
}

static void
drop_all_viewers(void)
{
  while(nclients > 0)
    close(clients[--nclients]);
}

int
main(int argc, char **argv)
{
  for(int i = 1; i < argc; i++){
    if(strcmp(argv[i], "-p") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if(strcmp(argv[i], "-q") == 0 && i + 1 < argc) quality = atoi(argv[++i]);
    else if(strcmp(argv[i], "-n") == 0 && i + 1 < argc) maxclients = atoi(argv[++i]);
    else if(strcmp(argv[i], "-s") == 0) half = 1;
    else if(strcmp(argv[i], "-w") == 0) wb = 0;
    else {
      fprintf(2, "usage: camserver [-p PORT] [-q QUALITY] [-s] [-w] "
                 "[-n CLIENTS]\n");
      exit(1);
    }
  }
  if(port < 1 || port > 65535 || quality < 1 || quality > 100 ||
     maxclients < 1 || maxclients > MAXCLIENTS){
    fprintf(2, "camserver: bad port, quality or client count\n");
    exit(1);
  }

  frame = malloc(CAM_READ_MAX);
  jpeg = malloc(JPEG_CAP);
  int listener = socket_listen(port, BACKLOG);
  int accept_ep = epoll_create();     // watches the listener
  int req_ep = epoll_create();        // bounded waits for request headers
  if(frame == 0 || jpeg == 0 || listener < 0 || accept_ep < 0 || req_ep < 0){
    fprintf(2, "camserver: setup failed (memory, port %d or epoll)\n", port);
    exit(1);
  }
  struct epoll_event ev, ready[BACKLOG];
  ev.events = EPOLLIN;
  ev.data.fd = listener;
  if(epoll_ctl(accept_ep, EPOLL_CTL_ADD, listener, &ev) < 0){
    fprintf(2, "camserver: epoll_ctl failed\n");
    exit(1);
  }
  printf("camserver: listening on port %d (%s, quality %d, max %d viewers)\n",
         port, half ? "320x240" : "640x480", quality, maxclients);
  printf("camserver: open http://<ip>:%d/  (stream: /stream, "
         "snapshot: /snapshot.jpg, status: /status)\n", port);

  for(;;){
    // Nobody watching: power the sensor down before sleeping in epoll_wait.
    if(nclients == 0)
      camera_close();
    // Idle: sleep until a connection arrives.  Streaming: just poll.
    int n = epoll_wait(accept_ep, ready, BACKLOG, nclients ? 0 : -1);
    for(int i = 0; i < n; i++)
      handle_connection(listener, req_ep);
    if(nclients == 0)
      continue;
    if(capture() < 0){
      // Camera failed: tell nobody more frames are coming and go idle.
      drop_all_viewers();
      sleep(10);
      continue;
    }
    broadcast();
  }
}
