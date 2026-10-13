#ifndef _UAPI_STAT_H
#define _UAPI_STAT_H

#include "types.h"

#define T_DIR     1
#define T_FILE    2
#define T_DEVICE  3

/* Binary layout returned by SYS_fstat. */
struct stat {
  int dev;
  uint ino;
  short type;
  short nlink;
  uint64 size;
};

#endif
