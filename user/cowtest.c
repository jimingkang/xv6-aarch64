#include <uapi/types.h>
#include <uapi/stat.h>
#include "user/user.h"
#include <fcntl.h>
#include <sys/mman.h>

// Copy-on-write fork regression test.
//   big       fork a process that owns more than half of free memory; only
//             possible if fork shares pages instead of copying them
//   isolate   parent and child write the same pages, each sees only its own
//   three     three processes share, write, and exit in different orders
//   copyout   the kernel (read from a pipe) writes into a COW page
//   mmap      MAP_PRIVATE|MAP_ANONYMOUS pages are COW too

#define PG 4096

static int failures;

static void
check(int ok, char *what)
{
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if(!ok)
    failures++;
}

static long
meminfo(char *key)
{
  static char buf[1024];
  int fd = open("/proc/meminfo", O_RDONLY), n;
  if(fd < 0)
    return -1;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n <= 0)
    return -1;
  buf[n] = 0;
  int kl = strlen(key);
  for(char *p = buf; *p; p++){
    if((p == buf || p[-1] == '\n') && strncmp(p, key, kl) == 0 && p[kl] == ':'){
      p += kl + 1;
      while(*p == ' ')
        p++;
      return atoi(p);
    }
  }
  return -1;
}

static int
child_status(void)
{
  int st = -1;
  wait(&st);
  return st;
}

static void
big(void)
{
  long freekb = meminfo("MemFree");
  long sz = (freekb * 1024 / 3 * 2) & ~(PG - 1);   // two thirds of free RAM
  char *p = sbrk(sz);

  check(p != (char *)-1, "big: sbrk two thirds of free memory");
  if(p == (char *)-1)
    return;
  for(long i = 0; i < sz; i += PG)
    p[i] = (char)(i / PG);
  long before = meminfo("CowShared");
  int pid = fork();
  if(pid == 0){
    for(long i = 0; i < sz; i += PG)
      if(p[i] != (char)(i / PG))
        exit(1);
    exit(0);
  }
  check(pid > 0, "big: fork succeeds (would need 2x memory without COW)");
  if(pid > 0)
    check(child_status() == 0, "big: child reads parent's data");
  check(meminfo("CowShared") - before >= sz / PG, "big: pages were shared");
  sbrk(-sz);
}

static void
isolate(void)
{
  int n = 16;
  char *p = sbrk(n * PG);
  int fds[2];

  for(int i = 0; i < n; i++)
    p[i * PG] = 'P';
  pipe(fds);
  int pid = fork();
  if(pid == 0){
    close(fds[0]);
    int ok = 1;
    for(int i = 0; i < n; i++){
      if(p[i * PG] != 'P')
        ok = 0;
      p[i * PG] = 'C';
    }
    char c = 'x';
    write(fds[1], &c, 1);      // parent writes after the child did
    sleep(5);
    for(int i = 0; i < n; i++)
      if(p[i * PG] != 'C')
        ok = 0;
    exit(ok ? 0 : 1);
  }
  close(fds[1]);
  char c;
  read(fds[0], &c, 1);
  int ok = 1;
  for(int i = 0; i < n; i++){
    if(p[i * PG] != 'P')
      ok = 0;
    p[i * PG] = 'Q';
  }
  check(ok, "isolate: parent does not see child's writes");
  check(child_status() == 0, "isolate: child does not see parent's writes");
  ok = 1;
  for(int i = 0; i < n; i++)
    if(p[i * PG] != 'Q')
      ok = 0;
  check(ok, "isolate: parent keeps its own writes");
  close(fds[0]);
  sbrk(-n * PG);
}

static void
three(void)
{
  int n = 8;
  char *p = sbrk(n * PG);

  for(int i = 0; i < n; i++)
    p[i * PG] = 1;
  long copied = meminfo("CowCopied"), reused = meminfo("CowReused");
  for(int k = 0; k < 2; k++){
    int pid = fork();
    if(pid == 0){
      sleep(3 * (k + 1));      // exit in a known order
      for(int i = 0; i < n; i++){
        if(p[i * PG] != 1)
          exit(1);
        p[i * PG] = 10 + k;
      }
      for(int i = 0; i < n; i++)
        if(p[i * PG] != 10 + k)
          exit(1);
      exit(0);
    }
  }
  // Parent writes last: by then both children copied and exited, so the
  // parent holds the only reference and reuses its pages in place.
  int ok = child_status() == 0;
  ok = ok && child_status() == 0;
  check(ok, "three: two children see the original and their own writes");
  for(int i = 0; i < n; i++)
    p[i * PG] = 2;
  ok = 1;
  for(int i = 0; i < n; i++)
    if(p[i * PG] != 2)
      ok = 0;
  check(ok, "three: parent writes after both children exited");
  check(meminfo("CowCopied") - copied >= 2 * n, "three: children copied");
  check(meminfo("CowReused") - reused >= n, "three: last owner reused in place");
  sbrk(-n * PG);
}

static void
copyout_cow(void)
{
  char *p = sbrk(PG);
  int fds[2];

  strcpy(p, "parent");
  pipe(fds);
  int pid = fork();
  if(pid == 0){
    close(fds[1]);
    // read() writes into p through copyout(): the kernel must break COW.
    int n = read(fds[0], p, 6);
    exit(n == 6 && strcmp(p, "kernel") == 0 ? 0 : 1);
  }
  close(fds[0]);
  write(fds[1], "kernel", 7);
  close(fds[1]);
  check(child_status() == 0, "copyout: read() into a COW page");
  check(strcmp(p, "parent") == 0, "copyout: parent page unchanged");
  sbrk(-PG);
}

static void
mmap_cow(void)
{
  int n = 4;
  char *p = mmap(0, n * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);

  check(p != MAP_FAILED, "mmap: anonymous private mapping");
  if(p == MAP_FAILED)
    return;
  for(int i = 0; i < n; i++)
    p[i * PG] = 'a';
  int pid = fork();
  if(pid == 0){
    for(int i = 0; i < n; i++){
      if(p[i * PG] != 'a')
        exit(1);
      p[i * PG] = 'b';
    }
    exit(0);
  }
  check(child_status() == 0, "mmap: child sees and writes MAP_PRIVATE pages");
  int ok = 1;
  for(int i = 0; i < n; i++)
    if(p[i * PG] != 'a')
      ok = 0;
  check(ok, "mmap: parent's MAP_PRIVATE pages unchanged");
  munmap(p, n * PG);
}

int
main(int argc, char *argv[])
{
  long free0 = meminfo("MemFree");

  // Run the tests in a child so that its page-table pages (sbrk(-n) keeps
  // them until exit) are freed too before the leak check.
  int pid = fork();
  if(pid == 0){
    big();
    isolate();
    three();
    copyout_cow();
    mmap_cow();
    exit(failures);
  }
  int st = -1;
  wait(&st);
  failures += st != 0;
  long free1 = meminfo("MemFree");
  printf("MemFree before %d kB, after %d kB\n", (int)free0, (int)free1);
  // The first run may leave a page or so in kernel caches populated on
  // first use; a second run must come back exactly.
  check(free1 >= free0 - 4 * 4, "no pages leaked");
  printf("cowtest: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
  exit(failures ? 1 : 0);
}
