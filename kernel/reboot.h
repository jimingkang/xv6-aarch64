#ifndef XV6_REBOOT_H
#define XV6_REBOOT_H

// Commands for the reboot(2) system call (same values as rpi3-lock's
// include/uapi/reboot.h).
#define REBOOT_CMD_RESTART  0x01234567
#define REBOOT_CMD_HALT     0xcdef0123

#endif
