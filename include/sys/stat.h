#ifndef _SYS_STAT_H
#define _SYS_STAT_H

#include <sys/types.h>
#include "kernel/types.h"
#include "kernel/stat.h"

#define st_dev   dev
#define st_ino   ino
#define st_mode  type
#define st_nlink nlink
#define st_size  size

#define S_IFMT   7
#define S_IFDIR  T_DIR
#define S_IFREG  T_FILE
#define S_IFCHR  T_DEVICE
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)

int stat(const char *, struct stat *);
int fstat(int, struct stat *);
int mkdir(const char *, mode_t);
int mknod(const char *, mode_t, unsigned long);

#endif
