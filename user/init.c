// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = { "login", 0 };

// Bring wlan0 up from /etc/wifi.conf before starting the local login service.
// /bin/wifi performs association, WPA2, DHCP and gateway ARP setup.
#define AUTO_START_WIFI 1

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
      if(target && type && options){
        int flags = strcmp(options, "ro") == 0 ? 1 : 0;
        if(strcmp(options, "ro") == 0 || strcmp(options, "rw") == 0)
          mount(source, target, type, flags);
      }
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

  // The kernel has mounted devtmpfs on /dev with a node for every driver
  // (see kernel/do_mounts.c).  Only if that is missing -- an old kernel or a
  // root without a /dev directory -- create static nodes on the root disk.
  unlink("/console");  // Remove the obsolete root-level compatibility node.
  if(open("/dev/ttyS0", O_RDWR) < 0 &&
     open("/dev/console", O_RDWR) < 0){
    mkdir("dev");
    mkdir("dev/input");
    mknod("dev/console", CONSOLE, 0);
    mknod("dev/tty", TTY, 0);
    mknod("dev/ttyS0", TTYS0, 0);
    mknod("dev/input/event0", INPUT, 0);
    mknod("dev/video0", CAMERA, 0);
    mknod("dev/sdroot", ROOTUPDATE, 0);
    if(open("/dev/ttyS0", O_RDWR) < 0 &&
       open("/dev/console", O_RDWR) < 0)
      exit(1);
  }
  dup(0);
  dup(0);

  // Visible mount points.  VFS path routing overlays procfs and ext2 on
  // these native directories when their corresponding backend is available.
  mkdir("proc");
  mkdir("boot");
  mkdir("mnt");
  mkdir("mnt/ext2");
  mkdir("mnt/net");
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
                "bootfs /boot fat32 rw 0 0\n"
                "ext2 /mnt/ext2 ext2 rw 0 0\n"
                "192.168.0.195:5640 /mnt/net netfs ro 0 0\n");
  create_config("etc/wifi.conf",
                "ssid=TP-Link_B114\n"
                "psk=Minghua123\n");
  mount_fstab();
  // Default recording/download directory on the ext2 volume (rw, large
  // files): tftpd serves from it and camshot -v writes frames into it.
  mkdir("mnt/ext2/video");

#if AUTO_START_WIFI
  // Optional automatic network bring-up. /bin/wifi reads wifi.conf,
  // associates, and obtains an IPv4 address through DHCP.
  char *wifi_argv[] = { "wifi", 0 };
  printf("init: connecting Wi-Fi from /etc/wifi.conf\n");
  pid = fork();
  if(pid == 0){
    exec("/bin/wifi", wifi_argv);
    printf("init: exec /bin/wifi failed\n");
    exit(1);
  }
  if(pid > 0)
    wait(0);
#endif

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
