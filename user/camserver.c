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
// One process serves everyone.  Capture, JPEG conversion, and TCP broadcast
// are a three-stage double-buffered pipeline; each frame is converted once
// and written to every stream viewer, so N viewers still cost one capture.
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
#include "kernel/mman.h"
#include "kernel/epoll.h"
#include "user/user.h"
#include "user/sync.h"
#include "user/camproc.h"

#define BACKLOG         4
#define MAXCLIENTS      4
#define REQ_CAP         1024
#define REQ_TIMEOUT_MS  2000
#define JPEG_CAP        (256 * 1024)
#define PIPE_DEPTH      2

static int port = 8080, quality = 75, half, wb = 1, maxclients = MAXCLIENTS;

static int clients[MAXCLIENTS];       // fds receiving /stream
static int nclients;

static int cam = -1;                  // /dev/video0 while someone is watching
static uchar *frame;                  // raw capture, CAM_READ_MAX bytes
static uchar *jpeg;                   // last encoded frame
static uint jpeg_len;                 // 0 = no valid frame yet

// Streaming pipeline.  The capture thread owns two camera fds so each mmap
// DMA slot remains held until the JPEG thread has consumed it.  The main
// thread alone owns accepted TCP descriptors and broadcasts encoded jobs.
struct raw_job {
  struct cam_frame_hdr hdr;
  uint64 capture_us;
  uint generation;
  int camera_index;
  int error;
};

struct encoded_job {
  uint64 capture_us;
  uint64 jpeg_us;
  uint generation;
  uint len;
  int buffer_index;
  int error;
};

static struct raw_job rawq[PIPE_DEPTH];
static struct encoded_job encodedq[PIPE_DEPTH];
static uchar *encoded_buf[PIPE_DEPTH];
static uchar *camera_dma;
static volatile uint raw_head, raw_tail, encoded_head, encoded_tail;
static volatile int pipeline_run;
static volatile uint pipeline_generation;
static semaphore_t pipeline_start;
static semaphore_t camera_free[PIPE_DEPTH];
static semaphore_t raw_free;
static semaphore_t raw_ready;
static semaphore_t encoded_free;
static semaphore_t encoded_ready;
static mutex_t convert_lock;          // camproc uses shared scratch arrays
static thread_t capture_thread, jpeg_thread;

// Counters for /status and the console.
static uint frames, served_snapshots, total_viewers;
static uint64 window_start_us;
static uint window_frames;
static uint fps10;                    // frames per second * 10, last window
static uint64 window_capture_us, window_jpeg_us, window_send_us;
static uint64 window_jpeg_bytes;
static uint window_send_frames;
static uint capture_ms10, jpeg_ms10, send_ms10; // window averages, 0.1 ms
static uint payload_kbps;             // generated MJPEG payload per viewer

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

// A duration stored as tenths of a millisecond, formatted as "12.3".
static void
cat_ms10(char *buf, int *n, uint v)
{
  cat_uint(buf, n, v / 10);
  cat(buf, n, ".");
  cat_uint(buf, n, v % 10);
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

static void
stats_reset(void)
{
  window_start_us = clock_us();
  window_frames = 0;
  window_capture_us = window_jpeg_us = window_send_us = 0;
  window_jpeg_bytes = 0;
  window_send_frames = 0;
}

// Account for one frame after the final pipeline stage.  Keeping the window
// update in the main thread avoids putting a lock around the status counters.
static void
stats_frame(uint64 capture_us, uint64 jpeg_us, uint64 send_us, uint bytes,
            int sent)
{
  frames++;
  window_frames++;
  window_capture_us += capture_us;
  window_jpeg_us += jpeg_us;
  window_jpeg_bytes += bytes;
  if(sent){
    window_send_us += send_us;
    window_send_frames++;
  }
  uint64 now = clock_us();
  if(now - window_start_us < 3000000ULL)
    return;

  uint64 elapsed = now - window_start_us;
  fps10 = (uint)(window_frames * 10000000ULL / elapsed);
  capture_ms10 = (uint)(window_capture_us * 10 /
                        (window_frames * 1000ULL));
  jpeg_ms10 = (uint)(window_jpeg_us * 10 /
                     (window_frames * 1000ULL));
  send_ms10 = window_send_frames ?
    (uint)(window_send_us * 10 / (window_send_frames * 1000ULL)) : 0;
  payload_kbps = (uint)(window_jpeg_bytes * 8000ULL / elapsed);

  char line[224];
  int k = 0;
  cat(line, &k, "camserver: ");
  cat_fps(line, &k);
  cat(line, &k, " fps, ");
  cat_uint(line, &k, bytes);
  cat(line, &k, " bytes/frame, ");
  cat_uint(line, &k, nclients);
  cat(line, &k, " viewer(s), capture/jpeg/send ");
  cat_ms10(line, &k, capture_ms10);
  cat(line, &k, "/");
  cat_ms10(line, &k, jpeg_ms10);
  cat(line, &k, "/");
  cat_ms10(line, &k, send_ms10);
  cat(line, &k, " ms, payload ");
  cat_uint(line, &k, payload_kbps);
  cat(line, &k, " kbit/s\n");
  write(1, line, k);
  stats_reset();
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
    stats_reset();
  }
  uint64 capture_begin = clock_us();
  int n = read(cam, frame, CAM_READ_MAX);
  uint64 capture_end = clock_us();
  if(n <= 0){
    printf("camserver: capture failed\n");
    camera_close();
    return -1;
  }
  uint len = 0;
  uint64 jpeg_begin = clock_us();
  mutex_lock(&convert_lock);
  int convert_result = camproc_jpeg(jpeg, JPEG_CAP, frame, n, half, wb,
                                    quality, &len);
  mutex_unlock(&convert_lock);
  if(convert_result < 0 || len == 0){
    printf("camserver: JPEG conversion failed\n");
    return -1;
  }
  uint64 jpeg_end = clock_us();
  jpeg_len = len;
  stats_frame(capture_end - capture_begin, jpeg_end - jpeg_begin, 0,
              jpeg_len, 0);
  return 0;
}

// ---------------------------------------------------------------------------
// Three-stage stream pipeline.

static int
pipeline_running(void)
{
  return __atomic_load_n(&pipeline_run, __ATOMIC_ACQUIRE);
}

static uint
pipeline_current_generation(void)
{
  return __atomic_load_n(&pipeline_generation, __ATOMIC_ACQUIRE);
}

static void
raw_push(const struct raw_job *job)
{
  sem_wait(&raw_free);
  uint tail = __atomic_load_n(&raw_tail, __ATOMIC_RELAXED);
  rawq[tail % PIPE_DEPTH] = *job;
  __atomic_store_n(&raw_tail, tail + 1, __ATOMIC_RELEASE);
  sem_post(&raw_ready);
}

static struct raw_job
raw_pop(void)
{
  sem_wait(&raw_ready);
  uint head = __atomic_load_n(&raw_head, __ATOMIC_RELAXED);
  struct raw_job job = rawq[head % PIPE_DEPTH];
  __atomic_store_n(&raw_head, head + 1, __ATOMIC_RELEASE);
  sem_post(&raw_free);
  return job;
}

static void
encoded_push(const struct encoded_job *job)
{
  uint tail = __atomic_load_n(&encoded_tail, __ATOMIC_RELAXED);
  encodedq[tail % PIPE_DEPTH] = *job;
  __atomic_store_n(&encoded_tail, tail + 1, __ATOMIC_RELEASE);
  sem_post(&encoded_ready);
}

static struct encoded_job
encoded_pop(void)
{
  sem_wait(&encoded_ready);
  uint head = __atomic_load_n(&encoded_head, __ATOMIC_RELAXED);
  struct encoded_job job = encodedq[head % PIPE_DEPTH];
  __atomic_store_n(&encoded_head, head + 1, __ATOMIC_RELEASE);
  return job;
}

// Two open camera files pin the two DMA slots independently.  A camera_free
// token is returned only after JPEG conversion, so the kernel cannot recycle
// a slot while userspace is reading RAW10 from its mmap mapping.
static void
capture_worker(void *arg)
{
  (void)arg;
  for(;;){
    sem_wait(&pipeline_start);
    if(!pipeline_running())
      continue;
    uint generation = pipeline_current_generation();
    int fd[PIPE_DEPTH] = {-1, -1};
    struct raw_job failure;
    memset(&failure, 0, sizeof(failure));
    failure.generation = generation;
    failure.camera_index = -1;
    failure.error = 1;

    fd[0] = open("/dev/video0", O_RDWR);
    fd[1] = open("/dev/video0", O_RDWR);
    if(fd[0] < 0 || fd[1] < 0){
      printf("camserver: pipeline cannot open two camera files\n");
      raw_push(&failure);
      if(fd[0] >= 0) close(fd[0]);
      if(fd[1] >= 0) close(fd[1]);
      continue;
    }
    if(camera_dma == 0){
      void *p = mmap(0, CAM_MMAP_BYTES, PROT_READ, MAP_SHARED, fd[0], 0);
      if(p == MAP_FAILED){
        printf("camserver: camera DMA mmap failed\n");
        raw_push(&failure);
        close(fd[0]);
        close(fd[1]);
        continue;
      }
      camera_dma = p;
    }
    printf("camserver: capture/JPEG/send pipeline started\n");

    uint sequence = 0;
    while(pipeline_running() &&
          generation == pipeline_current_generation()){
      int ci = sequence++ % PIPE_DEPTH;
      sem_wait(&camera_free[ci]);
      if(!pipeline_running() || generation != pipeline_current_generation()){
        sem_post(&camera_free[ci]);
        break;
      }
      struct raw_job job;
      memset(&job, 0, sizeof(job));
      job.generation = generation;
      job.camera_index = ci;
      uint64 begin = clock_us();
      int n = read(fd[ci], &job.hdr, sizeof(job.hdr));
      job.capture_us = clock_us() - begin;
      if(n != sizeof(job.hdr) || job.hdr.magic != CAM_MAGIC ||
         job.hdr.reserved >= CAM_BUFFER_COUNT ||
         job.hdr.data_bytes > CAM_SLOT_BYTES){
        printf("camserver: pipeline capture failed\n");
        job.error = 1;
      }
      raw_push(&job);
      if(job.error)
        break;
    }

    // Closing a camera file releases its kernel-side held slot.  Wait until
    // the JPEG worker has stopped touching both user mappings first.
    sem_wait(&camera_free[0]);
    sem_wait(&camera_free[1]);
    close(fd[0]);
    close(fd[1]);
    sem_post(&camera_free[0]);
    sem_post(&camera_free[1]);
  }
}

static void
jpeg_worker(void *arg)
{
  (void)arg;
  for(;;){
    struct raw_job raw = raw_pop();
    sem_wait(&encoded_free);
    uint tail = __atomic_load_n(&encoded_tail, __ATOMIC_RELAXED);
    int bi = tail % PIPE_DEPTH;
    struct encoded_job out;
    memset(&out, 0, sizeof(out));
    out.capture_us = raw.capture_us;
    out.generation = raw.generation;
    out.buffer_index = bi;
    out.error = raw.error;
    if(!out.error){
      const uchar *pixels = camera_dma + raw.hdr.reserved * CAM_SLOT_BYTES;
      uint64 begin = clock_us();
      mutex_lock(&convert_lock);
      int result = camproc_jpeg_raw(encoded_buf[bi], JPEG_CAP, &raw.hdr,
                                    pixels, half, wb, quality, &out.len);
      mutex_unlock(&convert_lock);
      if(result < 0 || out.len == 0)
        out.error = 1;
      out.jpeg_us = clock_us() - begin;
    }
    if(raw.camera_index >= 0)
      sem_post(&camera_free[raw.camera_index]);
    encoded_push(&out);
  }
}

static void
pipeline_start_stream(void)
{
  // Only the HTTP/main thread changes streaming generations, so an acquire
  // load followed by release stores is sufficient (and avoids an LSE helper
  // dependency in the freestanding userspace runtime).
  if(!pipeline_running()){
    uint next = pipeline_current_generation() + 1;
    __atomic_store_n(&pipeline_generation, next, __ATOMIC_RELEASE);
    __atomic_store_n(&pipeline_run, 1, __ATOMIC_RELEASE);
    jpeg_len = 0;
    stats_reset();
    sem_post(&pipeline_start);
  }
}

static void
pipeline_stop_stream(void)
{
  __atomic_store_n(&pipeline_run, 0, __ATOMIC_RELEASE);
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
  char body[640];
  int n = 0;
  body[0] = 0;
  cat(body, &n, "camera: ");
  cat(body, &n, (pipeline_running() || cam >= 0) ? "streaming\n" : "idle\n");
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
  cat(body, &n, "\navg capture ms: ");
  cat_ms10(body, &n, capture_ms10);
  cat(body, &n, "\navg jpeg ms: ");
  cat_ms10(body, &n, jpeg_ms10);
  cat(body, &n, "\navg send ms: ");
  cat_ms10(body, &n, send_ms10);
  cat(body, &n, "\npayload kbit/s per viewer: ");
  cat_uint(body, &n, payload_kbps);
  cat(body, &n, "\nsnapshots: ");
  cat_uint(body, &n, served_snapshots);
  cat(body, &n, "\n");
  send_simple(fd, "200 OK", "text/plain; charset=utf-8", body);
}

static int
send_jpeg_part(int fd, const uchar *data, uint len)
{
  char head[128];
  int n = 0;
  cat(head, &n, "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ");
  cat_uint(head, &n, len);
  cat(head, &n, "\r\n\r\n");
  if(write_all(fd, head, n) < 0 || write_all(fd, data, len) < 0 ||
     write_all(fd, "\r\n", 2) < 0)
    return -1;
  return 0;
}

static void
send_snapshot(int fd)
{
  // While streaming, the newest frame is at most one frame old; otherwise
  // power the camera up for a single capture.
  int fresh = !pipeline_running() && cam < 0;
  if(pipeline_running() && jpeg_len == 0){
    send_simple(fd, "503 Service Unavailable", "text/plain",
                "camera warming up\n");
    return;
  }
  if(!pipeline_running() && (cam < 0 || jpeg_len == 0)){
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
static uint64
broadcast(const uchar *data, uint len)
{
  uint64 begin = clock_us();
  for(int i = 0; i < nclients; ){
    if(send_jpeg_part(clients[i], data, len) < 0){
      printf("camserver: viewer fd=%d left\n", clients[i]);
      close(clients[i]);
      clients[i] = clients[--nclients];
      continue;
    }
    i++;
  }
  // This is time spent enqueueing one frame to all current TCP sockets.  It
  // can include blocking behind a slow viewer, so it is deliberately shown
  // separately from capture and JPEG conversion.
  return clock_us() - begin;
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
  encoded_buf[0] = malloc(JPEG_CAP);
  encoded_buf[1] = malloc(JPEG_CAP);
  int listener = socket_listen(port, BACKLOG);
  int accept_ep = epoll_create();     // watches the listener
  int req_ep = epoll_create();        // bounded waits for request headers
  if(frame == 0 || jpeg == 0 || encoded_buf[0] == 0 || encoded_buf[1] == 0 ||
     listener < 0 || accept_ep < 0 || req_ep < 0){
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
  if(sem_init(&pipeline_start, 0) < 0 ||
     sem_init(&camera_free[0], 1) < 0 ||
     sem_init(&camera_free[1], 1) < 0 ||
     sem_init(&raw_free, PIPE_DEPTH) < 0 ||
     sem_init(&raw_ready, 0) < 0 ||
     sem_init(&encoded_free, PIPE_DEPTH) < 0 ||
     sem_init(&encoded_ready, 0) < 0 ||
     mutex_init(&convert_lock) < 0 ||
     thread_create(&capture_thread, capture_worker, 0) < 0 ||
     thread_create(&jpeg_thread, jpeg_worker, 0) < 0){
    fprintf(2, "camserver: cannot create pipeline workers\n");
    exit(1);
  }
  printf("camserver: listening on port %d (%s, quality %d, max %d viewers)\n",
         port, half ? "320x240" : "640x480", quality, maxclients);
  printf("camserver: pipeline double-buffer-v1, JPEG dctsym-v2\n");
  printf("camserver: open http://<ip>:%d/  (stream: /stream, "
         "snapshot: /snapshot.jpg, status: /status)\n", port);

  for(;;){
    // Nobody watching: power the sensor down before sleeping in epoll_wait.
    if(nclients == 0){
      pipeline_stop_stream();
      camera_close();
    }
    // Idle: sleep until a connection arrives.  Streaming: just poll.
    int n = epoll_wait(accept_ep, ready, BACKLOG, nclients ? 0 : -1);
    for(int i = 0; i < n; i++)
      handle_connection(listener, req_ep);
    if(nclients == 0)
      continue;

    pipeline_start_stream();
    struct encoded_job job = encoded_pop();
    if(job.generation != pipeline_current_generation()){
      // A completed frame from a stream generation whose last viewer left.
      sem_post(&encoded_free);
      continue;
    }
    if(job.error || job.len == 0 || job.len > JPEG_CAP){
      printf("camserver: stream pipeline failed\n");
      sem_post(&encoded_free);
      pipeline_stop_stream();
      drop_all_viewers();
      sleep(10);
      continue;
    }
    // Preserve the most recent JPEG for /snapshot.jpg.  This is only a small
    // encoded-frame copy; the large RAW10 frame stays in the mmap DMA slot.
    memmove(jpeg, encoded_buf[job.buffer_index], job.len);
    jpeg_len = job.len;
    uint64 send_us = broadcast(encoded_buf[job.buffer_index], job.len);
    stats_frame(job.capture_us, job.jpeg_us, send_us, job.len, 1);
    sem_post(&encoded_free);
    if(nclients == 0)
      pipeline_stop_stream();
  }
}
