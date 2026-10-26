// Exercise fork(), the thread library and the Linux-style clone() flags:
// CLONE_FILES, CLONE_FS, CLONE_VM, CLONE_THREAD, CLONE_VFORK and the TID
// stores.  Prints "clonetest: OK" when every check passes.

#include <uapi/types.h>
#include <uapi/stat.h>
#include <uapi/fcntl.h>
#include <uapi/sched.h>
#include "user/user.h"

#define STACK 8192

static int failures;
static volatile int shared_word;
static volatile int vfork_done_flag;
static int ptid, ctid;
static char cwd[128];

static void
check(int ok, char *what)
{
  printf("clonetest: %s %s\n", ok ? "ok  " : "FAIL", what);
  if(!ok)
    failures++;
}

static void *
stack_top(void)
{
  char *s = malloc(STACK);
  if(s == 0){
    printf("clonetest: malloc failed\n");
    exit(1);
  }
  return s + STACK;
}

static int
fd_valid(int fd)
{
  struct stat st;
  return fstat(fd, &st) == 0;
}

static int
child_open(void *arg)
{
  int fd = open("/clonetest.tmp", O_CREATE | O_RDWR);
  return fd < 0 ? 100 : fd;
}

static int
child_close(void *arg)
{
  return close((int)(uint64)arg) == 0 ? 0 : 1;
}

static int
child_chdir(void *arg)
{
  return chdir((char *)arg) == 0 ? 0 : 1;
}

static int
child_store(void *arg)
{
  shared_word = (int)(uint64)arg;
  return 0;
}

static int
child_slow(void *arg)
{
  // A vfork parent must not run before this returns.
  sleep_ticks(5);
  vfork_done_flag = 1;
  return 7;
}

static int
child_tid(void *arg)
{
  return 0;
}

static void
worker_open(void *arg)
{
  *(int *)arg = open("/clonetest.tmp", O_CREATE | O_RDWR);
  texit(0);
}

static int
waitchild(int pid)
{
  int status = -1;
  if(waitpid(pid, &status, 0) != pid)
    return -1;
  return status;
}

static void
test_fork(void)
{
  int pid = fork();
  if(pid == 0)
    exit(42);
  check(pid > 0 && waitchild(pid) == 42, "fork() via SYS_fork");
}

static void
test_thread_library(void)
{
  thread_t t;
  int fd = -1, status = -1;

  check(thread_create(&t, worker_open, &fd) == 0 &&
        thread_join(&t, &status) > 0 && status == 0,
        "thread_create/join via SYS_clone");
  check(fd >= 0 && fd_valid(fd), "thread's open() visible to the process");
  if(fd >= 0)
    close(fd);
}

static void
test_files(void)
{
  int pid, fd, status;

  pid = clone(child_open, stack_top(), CLONE_FILES | SIGCHLD, 0);
  fd = waitchild(pid);
  check(pid > 0 && fd >= 0 && fd < 100 && fd_valid(fd),
        "CLONE_FILES: child's open() appears in parent");

  pid = clone(child_close, stack_top(), CLONE_FILES | SIGCHLD,
              (void *)(uint64)fd);
  status = waitchild(pid);
  check(pid > 0 && status == 0 && !fd_valid(fd),
        "CLONE_FILES: child's close() closes parent's fd");

  pid = clone(child_open, stack_top(), SIGCHLD, 0);
  fd = waitchild(pid);
  check(pid > 0 && fd >= 0 && fd < 100 && !fd_valid(fd),
        "no CLONE_FILES: child's open() stays private");

  fd = open("/clonetest.tmp", O_RDWR);
  pid = clone(child_close, stack_top(), SIGCHLD, (void *)(uint64)fd);
  status = waitchild(pid);
  check(fd >= 0 && status == 0 && fd_valid(fd),
        "no CLONE_FILES: child's close() leaves parent's fd");
  close(fd);
}

static void
test_fs(void)
{
  int pid;

  mkdir("/clonetest.d");
  chdir("/");
  pid = clone(child_chdir, stack_top(), SIGCHLD, "/clonetest.d");
  check(waitchild(pid) == 0 && getcwd(cwd, sizeof(cwd)) &&
        strcmp(cwd, "/") == 0, "no CLONE_FS: child's chdir() stays private");

  pid = clone(child_chdir, stack_top(), CLONE_FS | SIGCHLD, "/clonetest.d");
  check(waitchild(pid) == 0 && getcwd(cwd, sizeof(cwd)) &&
        strcmp(cwd, "/clonetest.d") == 0,
        "CLONE_FS: child's chdir() moves parent");
  check(open("clonetest.rel", O_CREATE | O_RDWR) >= 0 &&
        unlink("/clonetest.d/clonetest.rel") == 0,
        "CLONE_FS: relative path resolves in shared cwd");
  chdir("/");
}

static void
test_vm(void)
{
  int pid;

  shared_word = 0;
  pid = clone(child_store, stack_top(), SIGCHLD, (void *)11);
  check(waitchild(pid) == 0 && shared_word == 0,
        "no CLONE_VM: child's store stays private");
  pid = clone(child_store, stack_top(), CLONE_VM | SIGCHLD, (void *)22);
  check(waitchild(pid) == 0 && shared_word == 22,
        "CLONE_VM: child's store is shared");

  pid = clone(child_store, stack_top(),
              CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
              CLONE_THREAD, (void *)33);
  check(pid > 0 && tjoin(pid, 0) == pid && shared_word == 33,
        "CLONE_THREAD: thread joined with tjoin()");

  pid = clone(child_tid, stack_top(),
              CLONE_VM | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | SIGCHLD,
              0, &ptid, 0, &ctid);
  check(pid > 0 && waitchild(pid) == 0 && ptid == pid && ctid == pid,
        "CLONE_PARENT_SETTID/CLONE_CHILD_SETTID");
}

static void
test_vfork(void)
{
  int pid, status;

  vfork_done_flag = 0;
  pid = clone(child_slow, stack_top(), CLONE_VM | CLONE_VFORK | SIGCHLD, 0);
  check(pid > 0 && vfork_done_flag == 1,
        "CLONE_VFORK: parent waits for child exit");
  check(waitchild(pid) == 7, "CLONE_VFORK: exit status reaped");

  vfork_done_flag = 0;
  pid = vfork();
  if(pid == 0){
    char *argv[] = { "echo", "clonetest: vfork child exec'd echo", 0 };
    vfork_done_flag = 1;          // shared memory: parent sees this
    exec("/echo", argv);
    exec("/bin/echo", argv);
    exit(99);
  }
  check(pid > 0 && vfork_done_flag == 1, "vfork(): parent resumes after exec");
  status = waitchild(pid);
  check(status == 0, "vfork(): exec'd child exits normally");

  pid = vfork();
  if(pid == 0)
    exit(5);
  check(pid > 0 && waitchild(pid) == 5, "vfork(): child exit(5)");
}

static void
test_invalid(void)
{
  check(clone(child_tid, stack_top(), CLONE_THREAD | CLONE_VM, 0) < 0,
        "reject CLONE_THREAD without CLONE_SIGHAND");
  check(clone(child_tid, stack_top(), CLONE_SIGHAND, 0) < 0,
        "reject CLONE_SIGHAND without CLONE_VM");
  check(clone(child_tid, 0, CLONE_VM | SIGCHLD, 0) < 0,
        "reject CLONE_VM without a stack");
  check(clone(child_tid, stack_top(), 0x80000000, 0) < 0,
        "reject unknown flags");
}

int
main(void)
{
  test_fork();
  test_thread_library();
  test_files();
  test_fs();
  test_vm();
  test_vfork();
  test_invalid();
  unlink("/clonetest.tmp");
  unlink("/clonetest.d");
  if(failures == 0)
    printf("clonetest: OK\n");
  else
    printf("clonetest: %d FAILED\n", failures);
  exit(failures != 0);
}
