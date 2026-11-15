// reboot: reset the board (Tiny210: S5PV210 software reset; the iROM boots
// BL1 and the kernel from the SD card again).
#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  printf("reboot: restarting\n");
  reboot();
  printf("reboot: not supported on this board\n");
  exit(1);
}
