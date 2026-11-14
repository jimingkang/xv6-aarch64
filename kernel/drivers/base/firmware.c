// request_firmware(): look for the blob on the SD card's FAT32 partition
// first (the volume init mounts at /boot; a file at its root, e.g.
// /boot/mt7601u.bin), then fall back to the copy linked into the kernel
// (fw_<name>_start/end, see ports/tiny210/Makefile FIRMWARE).
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/param.h"
#include "kernel/spinlock.h"
#include "kernel/defs.h"
#include "kernel/firmware.h"
#ifdef XV6_HAVE_VFS
#include "kernel/fat32.h"
#endif

extern const uchar fw_mt7601u_start[], fw_mt7601u_end[];

static struct firmware builtin[] = {
  { "mt7601u.bin", fw_mt7601u_start, 0 },
};

#ifdef XV6_HAVE_VFS
#define FW_MAX (256U << 10)
static uchar fw_buf[FW_MAX] __attribute__((aligned(64)));
static struct firmware from_sd;
static char from_sd_name[32];
static struct spinlock fw_lock;

static int
load_from_boot(const struct firmware **fw, const char *name)
{
  char path[34];
  struct fat32_file f;
  if(!fat32ready() || strlen(name) > 31)
    return -1;
  path[0] = '/';
  safestrcpy(path + 1, (char*)name, sizeof(path) - 1);
  if(fat32openpath(path, &f) < 0)
    return -1;
  if(f.size == 0 || f.size > FW_MAX){
    printf("firmware: /boot%s: size %d unusable\n", path, f.size);
    return -1;
  }
  static int lock_ready;
  if(!lock_ready){ initlock(&fw_lock, "firmware"); lock_ready = 1; }
  acquire(&fw_lock);         // one SD-loaded blob at a time (single buffer)
  int n = fat32pread(&f, 0, fw_buf, f.size);
  release(&fw_lock);
  if(n != (int)f.size){
    printf("firmware: /boot%s: read %d of %d bytes\n", path, n, f.size);
    return -1;
  }
  safestrcpy(from_sd_name, (char*)name, sizeof(from_sd_name));
  from_sd.name = from_sd_name;
  from_sd.data = fw_buf;
  from_sd.size = f.size;
  *fw = &from_sd;
  printf("firmware: %s loaded from /boot (%d bytes)\n", name, f.size);
  return 0;
}
#endif

int
request_firmware(const struct firmware **fw, const char *name)
{
#ifdef XV6_HAVE_VFS
  if(load_from_boot(fw, name) == 0)
    return 0;
#endif
  builtin[0].size = fw_mt7601u_end - fw_mt7601u_start;
  for(uint i = 0; i < sizeof(builtin)/sizeof(builtin[0]); i++)
    if(strncmp(builtin[i].name, name, 32) == 0){
      *fw = &builtin[i];
      printf("firmware: %s from the kernel image (%d bytes)\n", name, builtin[i].size);
      return 0;
    }
  printf("firmware: %s not found\n", name);
  return -1;
}
