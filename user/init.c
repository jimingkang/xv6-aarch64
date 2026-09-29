// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = { "login", 0 };

static void
create_config(char *path, char *contents)
{
  int fd = open(path, O_RDONLY);
  if(fd >= 0){
    close(fd);
    return;
  }
  fd = open(path, O_CREATE | O_WRONLY);
  if(fd >= 0){
    write(fd, contents, strlen(contents));
    close(fd);
  }
}

static char*
field(char **cursor)
{
  char *p = *cursor, *start;
  while(*p == ' ' || *p == '\t')
    p++;
  if(*p == 0 || *p == '\n' || *p == '#'){
    *cursor = p;
    return 0;
  }
  start = p;
  while(*p && *p != ' ' && *p != '\t' && *p != '\n')
    p++;
  if(*p)
    *p++ = 0;
  *cursor = p;
  return start;
}

static void
mount_fstab(void)
{
  char buf[512], *p, *source, *target, *type, *options;
  int fd = open("/etc/fstab", O_RDONLY);
  int n;
  if(fd < 0)
    return;
  n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if(n <= 0)
    return;
  buf[n] = 0;
  p = buf;
  while(*p){
    source = field(&p);
    if(source){
      target = field(&p);
      type = field(&p);
      options = field(&p);
      if(target && type && options)
        mount(source, target, type, 1); // all current VFS backends are ro
    }
    while(*p && *p != '\n')
      p++;
    if(*p == '\n')
      p++;
  }
}

int
main(void)
{
  int pid, wpid;

  // Bootstrap from the historical root device node, then install the usual
  // Unix device namespace and use the concrete ttyS0 for local login.
  if(open("console", O_RDWR) < 0){
    mknod("console", CONSOLE, 0);
    open("console", O_RDWR);
  }
  dup(0);  // stdout
  dup(0);  // stderr

  mkdir("dev");
  mknod("dev/console", CONSOLE, 0);
  mknod("dev/tty", TTY, 0);
  mknod("dev/ttyS0", TTYS0, 0);
  close(0);
  close(1);
  close(2);
  if(open("/dev/ttyS0", O_RDWR) < 0 &&
     open("/dev/console", O_RDWR) < 0)
    open("/console", O_RDWR);
  dup(0);
  dup(0);

  // Visible mount points.  VFS path routing overlays procfs and ext2 on
  // these native directories when their corresponding backend is available.
  mkdir("proc");
  mkdir("mnt");
  mkdir("mnt/ext2");
  mkdir("etc");
  mkdir("root");
  mkdir("bin");
  mkdir("usr");
  mkdir("usr/bin");
  create_config("etc/hostname", "xv6-rpi3\n");
  create_config("etc/passwd", "root:xv6:0:0:root:/root:/bin/sh\n");
  create_config("etc/rc", "export PATH=/bin:/usr/bin:/:.\n");
  create_config("etc/fstab",
                "proc /proc procfs ro 0 0\n"
                "ext2 /mnt/ext2 ext2 ro 0 0\n");
  create_config("etc/wifi.conf",
                "ssid=TP-Link_B114_5G\n"
                "psk=Minghua123\n");
  mount_fstab();

  for(;;){
    printf("init: starting local login on /dev/ttyS0\n");
    pid = fork();
    if(pid < 0){
      printf("init: fork failed\n");
      exit(1);
    }
    if(pid == 0){
      exec("/bin/login", argv);
      printf("init: exec login failed\n");
      exit(1);
    }

    for(;;){
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *) 0);
      if(wpid == pid){
        // the shell exited; restart it.
        break;
      } else if(wpid < 0){
        printf("init: wait returned an error\n");
        exit(1);
      } else {
        // it was a parentless process; do nothing.
      }
    }
  }
}
