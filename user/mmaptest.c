#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <uapi/fb.h>

// mmap(2) regression test: anonymous, file-backed (MAP_PRIVATE/MAP_SHARED,
// msync, writeback on munmap), fork inheritance, partial munmap, fault
// signals, and device mappings (/dev/zero, /dev/mem, /dev/fb0).
//   mmaptest        run everything
//   mmaptest fb     only draw into /dev/fb0

#define PG 4096
#define FILE1 "mmaptest.dat"

static int failures;

static void
check(int ok, char *what)
{
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    failures++;
}

static char
pattern(int i)
{
  return 'A' + i % 23;
}

// Run fn in a child and report whether the child was killed by a fault.
static int
child_faults(void (*fn)(void *), void *arg)
{
  int pid = fork(), status = 0;

  if(pid == 0){
    fn(arg);
    exit(0);
  }
  wait(&status);
  return status != 0;
}

static void
touch_read(void *p)
{
  volatile char c = *(volatile char *)p;
  (void)c;
}

static void
touch_write(void *p)
{
  *(volatile char *)p = 1;
}

static int
make_file(char *name, int size)
{
  char buf[512];
  int fd, i, n;

  unlink(name);
  if((fd = open(name, O_CREATE | O_RDWR)) < 0)
    return -1;
  for(i = 0; i < size; i += n){
    n = size - i < (int)sizeof(buf) ? size - i : (int)sizeof(buf);
    for(int j = 0; j < n; j++)
      buf[j] = pattern(i + j);
    if(write(fd, buf, n) != n){
      close(fd);
      return -1;
    }
  }
  close(fd);
  return 0;
}

static void
test_anon(void)
{
  char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int ok = p != MAP_FAILED;

  check(ok, "anon: mmap 3 pages");
  if(!ok)
    return;
  ok = p[0] == 0 && p[PG] == 0 && p[3 * PG - 1] == 0;
  for(int i = 0; i < 3 * PG; i++)
    p[i] = pattern(i);
  for(int i = 0; i < 3 * PG; i++)
    if(p[i] != pattern(i))
      ok = 0;
  check(ok, "anon: zero-filled on first touch, then read back");

  // Partial munmap splits the VMA: the middle page goes, both ends stay.
  check(munmap(p + PG, PG) == 0, "anon: munmap middle page");
  check(p[0] == pattern(0) && p[2 * PG] == pattern(2 * PG),
        "anon: pages around the hole survive");
  check(child_faults(touch_read, p + PG), "anon: access to the hole faults");
  check(munmap(p, 3 * PG) == 0, "anon: munmap the rest");
  check(child_faults(touch_read, p), "anon: access after munmap faults");

  // A read-only mapping rejects stores.
  char *r = mmap(0, PG, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  check(r != MAP_FAILED && child_faults(touch_write, r),
        "anon: store to PROT_READ faults");
  munmap(r, PG);

  // MAP_FIXED at a chosen address.
  char *want = (char *)0x2000000000UL;
  char *f = mmap(want, PG, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  check(f == want, "anon: MAP_FIXED honours the address");
  if(f == want){
    f[10] = 7;
    check(f[10] == 7, "anon: MAP_FIXED page usable");
    munmap(f, PG);
  }
}

static void
test_fork(void)
{
  char *sh = mmap(0, PG, PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  char *pr = mmap(0, PG, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  int status;

  if(sh == MAP_FAILED || pr == MAP_FAILED){
    check(0, "fork: mmap");
    return;
  }
  strcpy(sh, "parent");
  strcpy(pr, "parent");
  if(fork() == 0){
    strcpy(sh, "child");
    strcpy(pr, "child");
    exit(0);
  }
  wait(&status);
  check(strcmp(sh, "child") == 0, "fork: MAP_SHARED|MAP_ANONYMOUS is shared");
  check(strcmp(pr, "parent") == 0, "fork: MAP_PRIVATE copy is private");

  // A page the child touches first must reach the parent as well.
  char *late = mmap(0, 2 * PG, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if(fork() == 0){
    late[PG + 5] = 42;
    exit(0);
  }
  wait(&status);
  check(late[PG + 5] == 42, "fork: shared page first faulted by the child");
  munmap(sh, PG);
  munmap(pr, PG);
  munmap(late, 2 * PG);
}

static void
test_file_private(void)
{
  int size = 2 * PG + 100, fd, ok = 1;
  char buf[16];

  if(make_file(FILE1, size) < 0 || (fd = open(FILE1, O_RDONLY)) < 0){
    check(0, "file private: create test file");
    return;
  }
  char *p = mmap(0, 3 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  close(fd);                          // the mapping keeps the file
  check(p != MAP_FAILED, "file private: mmap O_RDONLY file, PROT_WRITE");
  if(p == MAP_FAILED)
    return;
  for(int i = 0; i < size; i++)
    if(p[i] != pattern(i))
      ok = 0;
  check(ok, "file private: contents match the file (after close(fd))");
  check(p[size] == 0 && p[3 * PG - 1] == 0,
        "file private: tail of last page is zero");
  p[0] = 'z';
  munmap(p, 3 * PG);
  fd = open(FILE1, O_RDONLY);
  check(read(fd, buf, 1) == 1 && buf[0] == pattern(0),
        "file private: stores never reach the file");
  close(fd);

  // Offset mapping: page 1 of the file.
  fd = open(FILE1, O_RDONLY);
  p = mmap(0, PG, PROT_READ, MAP_PRIVATE, fd, PG);
  check(p != MAP_FAILED && p[0] == pattern(PG) && p[99] == pattern(PG + 99),
        "file private: offset = 1 page");
  munmap(p, PG);
  check(mmap(0, PG, PROT_READ, MAP_PRIVATE, fd, 100) == MAP_FAILED,
        "file private: unaligned offset rejected");
  check(mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED,
        "file shared: PROT_WRITE on O_RDONLY fd rejected");
  close(fd);
}

static void
sigbus_child(void *arg)
{
  int fd = open(FILE1, O_RDONLY);
  char *p = mmap(0, 4 * PG, PROT_READ, MAP_PRIVATE, fd, 0);
  touch_read(p + 3 * PG);             // wholly beyond EOF
}

static void
test_file_shared(void)
{
  int size = 2 * PG + 100, fd, ok;
  char buf[32];

  if(make_file(FILE1, size) < 0 || (fd = open(FILE1, O_RDWR)) < 0){
    check(0, "file shared: create test file");
    return;
  }
  char *p = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  check(p != MAP_FAILED, "file shared: mmap");
  if(p == MAP_FAILED){
    close(fd);
    return;
  }
  check(p[PG + 1] == pattern(PG + 1), "file shared: read through mapping");

  // msync makes the store visible to read() without unmapping.
  memmove(p + 10, "msync-ok", 8);
  check(msync(p, PG, MS_SYNC) == 0, "file shared: msync");
  check(pread(fd, buf, 8, 10) == 8 && memcmp(buf, "msync-ok", 8) == 0,
        "file shared: msync wrote the page back");

  // munmap writes the remaining dirty page back, and never extends the file.
  memmove(p + PG + 20, "munmap-ok", 9);
  p[size - 1] = '!';
  check(munmap(p, size) == 0, "file shared: munmap");
  check(pread(fd, buf, 9, PG + 20) == 9 && memcmp(buf, "munmap-ok", 9) == 0,
        "file shared: munmap wrote the dirty page back");
  check(pread(fd, buf, 1, size - 1) == 1 && buf[0] == '!',
        "file shared: last partial page written back");
  struct stat st;
  check(fstat(fd, &st) == 0 && st.size == size,
        "file shared: file size unchanged");

  // Two processes share one page-cache page.
  p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(fork() == 0){
    int cfd = open(FILE1, O_RDWR);
    char *q = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
    memmove(q + 100, "from-child", 10);
    exit(0);
  }
  wait(0);
  check(memcmp(p + 100, "from-child", 10) == 0,
        "file shared: independent mmaps of one file share pages");

  // write() from a mapped buffer, read() into a never-touched mapping.
  char *dst = mmap(0, PG, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ok = pread(fd, dst, 64, 0) == 64 && memcmp(dst, p, 64) == 0;
  check(ok, "file shared: read() into an untouched mapping");
  int out = open("mmaptest.out", O_CREATE | O_RDWR | O_TRUNC);
  ok = write(out, p + 100, 10) == 10 && pread(out, buf, 10, 0) == 10 &&
       memcmp(buf, "from-child", 10) == 0;
  check(ok, "file shared: write() from a mapped buffer");
  close(out);
  unlink("mmaptest.out");
  munmap(dst, PG);
  munmap(p, PG);
  close(fd);

  check(child_faults(sigbus_child, 0), "file: access beyond EOF faults (SIGBUS)");

  int pfd[2];
  pipe(pfd);
  check(mmap(0, PG, PROT_READ, MAP_SHARED, pfd[0], 0) == MAP_FAILED,
        "file: mmap of a pipe rejected");
  close(pfd[0]);
  close(pfd[1]);
  unlink(FILE1);
}

static void
test_devices(void)
{
  int fd, status;

  // /dev/zero: MAP_SHARED is shared anonymous memory.
  if((fd = open("/dev/zero", O_RDWR)) < 0){
    check(0, "dev/zero: open");
  } else {
    char *z = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    check(z != MAP_FAILED && z[0] == 0 && z[PG - 1] == 0, "dev/zero: mmap");
    if(fork() == 0){
      z[1] = 9;
      exit(0);
    }
    wait(&status);
    check(z[1] == 9, "dev/zero: MAP_SHARED visible across fork");
    munmap(z, PG);
  }

  // /dev/mem: the BCM2835 system timer (free-running 1 MHz counter).
  if((fd = open("/dev/mem", O_RDWR)) < 0){
    check(0, "dev/mem: open");
  } else {
    volatile uint32 *t = mmap(0, PG, PROT_READ, MAP_SHARED, fd, 0x3f003000);
    check(t != MAP_FAILED, "dev/mem: mmap system timer registers");
    if(t != MAP_FAILED){
      uint32 a = t[1];                // CLO
      sleep(1);
      uint32 b = t[1];
      printf("     timer CLO %d -> %d\n", a, b);
      check(b > a, "dev/mem: timer counter advances");
      munmap((void *)t, PG);
    }
    check(mmap(0, PG, PROT_READ, MAP_SHARED, fd, 0x100000) == MAP_FAILED,
          "dev/mem: kernel RAM refused");
    close(fd);
  }
}

static int
test_fb(void)
{
  struct fb_var_screeninfo var;
  struct fb_fix_screeninfo fix;
  int fd = open("/dev/fb0", O_RDWR), ok;

  if(fd < 0){
    check(0, "fb0: open");
    return -1;
  }
  ok = ioctl(fd, FBIOGET_VSCREENINFO, &var) == 0 &&
       ioctl(fd, FBIOGET_FSCREENINFO, &fix) == 0;
  check(ok, "fb0: FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO");
  if(!ok){
    close(fd);
    return -1;
  }
  printf("     fb0 %dx%d %d bpp, line %d, %d bytes\n", var.xres, var.yres,
         var.bits_per_pixel, fix.line_length, fix.smem_len);
  uint32 *px = mmap(0, fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, 0);
  close(fd);
  check(px != MAP_FAILED, "fb0: mmap frame buffer");
  if(px == MAP_FAILED)
    return -1;
  // Colour bars.
  uint32 bars[] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
                    0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
  int stride = fix.line_length / 4;
  for(int y = 0; y < (int)var.yres; y++)
    for(int x = 0; x < (int)var.xres; x++)
      px[y * stride + x] = bars[x * 8 / var.xres];
  check(px[0] == 0xffffff && px[(var.yres - 1) * stride + var.xres - 1] == 0,
        "fb0: pixels written and read back");
  munmap(px, fix.smem_len);
  return 0;
}

int
main(int argc, char **argv)
{
  if(argc > 1 && strcmp(argv[1], "fb") == 0)
    exit(test_fb() < 0);
  test_anon();
  test_fork();
  test_file_private();
  test_file_shared();
  test_devices();
  test_fb();
  printf("mmaptest: %s (%d failure%s)\n", failures ? "FAILED" : "OK",
         failures, failures == 1 ? "" : "s");
  exit(failures != 0);
}
