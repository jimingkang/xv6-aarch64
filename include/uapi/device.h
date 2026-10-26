#ifndef _UAPI_DEVICE_H
#define _UAPI_DEVICE_H

#include "types.h"

#define major(dev)  ((dev) >> 16 & 0xffff)
#define minor(dev)  ((dev) & 0xffff)
#define mkdev(m, n) ((uint)((m) << 16 | (n)))

/* Stable character/block device major numbers visible through mknod(). */
#define CONSOLE    1
#define TTY        2
#define TTYS0      3
#define INPUT      4
#define CAMERA     5
/* 6 was ROOTUPDATE (/dev/sdroot), removed */
#define BLOCKDEV   7
#define MEM        8     /* /dev/mem (minor 1), /dev/zero (minor 5) */
#define FB         9     /* /dev/fb0 */

#endif
