// Kernel command line (compare Linux init/main.c + __setup("root=")).
//
// cmdline.txt on the boot partition belongs to the Raspberry Pi VideoCore
// firmware (start.elf), which would pass it to a Linux kernel in the device
// tree.  xv6 keeps its own file, cmdline_xv6.txt, next to it: config.txt
// sets disable_commandline_tags=1 and xv6 parses no device tree, so the
// kernel reads the file from the FAT32 boot partition itself, right after
// rootdev_init() has mounted it.  Without the file or a boot partition (QEMU
// bare image) the built-in CONFIG_CMDLINE is used.  Parameters xv6
// understands:
//
//   root=/dev/mmcblk0p2 | mmcblk0p2 | PARTUUID=a1b2c3d4-02 | /dev/mmcblk0
//   rootfstype=xv6fs
//
// Everything else is kept in /proc/cmdline but ignored.

#include "types.h"
#include "aarch64.h"
#include "param.h"
#include "spinlock.h"
#include "defs.h"
#include "fat32.h"

#ifndef CONFIG_CMDLINE
#define CONFIG_CMDLINE ""
#endif

#define CMDLINE_MAX  256
#define CMDLINE_FILE "/cmdline_xv6.txt"

static char cmdline[CMDLINE_MAX];

void
cmdline_init(void)
{
  struct fat32_file f;
  int n = -1;

  if(fat32ready() && fat32openpath(CMDLINE_FILE, &f) == 0){
    n = fat32pread(&f, 0, cmdline, CMDLINE_MAX - 1);
    if(n < 0)
      n = -1;
  }
  if(n < 0){
    safestrcpy(cmdline, CONFIG_CMDLINE, sizeof(cmdline));
    n = strlen(cmdline);
  }
  cmdline[n] = 0;
  // One line: newlines and tabs become blanks, trailing blanks go.
  for(int i = 0; i < n; i++)
    if(cmdline[i] == '\n' || cmdline[i] == '\r' || cmdline[i] == '\t')
      cmdline[i] = ' ';
  while(n > 0 && cmdline[n - 1] == ' ')
    cmdline[--n] = 0;
  printf("Kernel command line: %s\n", cmdline);
}

char*
cmdline_get_all(void)
{
  return cmdline;
}

// Copy the value of key=value into val; returns 1 if the key is present.
// A bare word "key" counts as present with an empty value.
int
cmdline_get(char *key, char *val, int n)
{
  int klen = strlen(key);
  char *p = cmdline;

  while(*p){
    while(*p == ' ')
      p++;
    char *w = p;
    while(*p && *p != ' ')
      p++;
    if(p - w >= klen && strncmp(w, key, klen) == 0 &&
       (w + klen == p || w[klen] == '=')){
      int len = 0;
      if(w + klen < p){
        char *v = w + klen + 1;
        while(v < p && len < n - 1)
          val[len++] = *v++;
      }
      if(n > 0)
        val[len] = 0;
      return 1;
    }
  }
  return 0;
}
