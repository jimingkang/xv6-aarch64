// reboot [-h]   (from rpi3-lock)
//   reboot      restart the board (Tiny210: S5PV210 software reset; the iROM
//               boots BL1 and the kernel from the SD card again)
//   reboot -h   halt instead: power off by dropping PS_HOLD
#include "kernel/types.h"
#include "kernel/reboot.h"
#include "user/user.h"

static int
streq(const char *a, const char *b)
{
  while(*a && *a == *b){ a++; b++; }
  return *a == *b;
}

static const char *
base(const char *p)
{
  const char *b = p;
  for(; *p; p++)
    if(*p == '/')
      b = p + 1;
  return b;
}

int
main(int argc, char *argv[])
{
  int cmd = REBOOT_CMD_RESTART;

  if(argc > 0 && streq(base(argv[0]), "halt"))
    cmd = REBOOT_CMD_HALT;
  if(argc == 2 && streq(argv[1], "-h"))
    cmd = REBOOT_CMD_HALT;
  else if(argc != 1){
    fprintf(2, "usage: reboot [-h]\n");
    exit(1);
  }

  if(reboot(cmd) < 0){
    fprintf(2, "reboot: failed\n");
    exit(1);
  }
  exit(0);   // not reached
}
