// Built-in firmware table for request_firmware(). Each blob is linked in by
// the Makefile as fw_<name>_start/end (see ports/x210/Makefile, FIRMWARE).
#include "kernel/types.h"
#include "kernel/aarch64.h"
#include "kernel/defs.h"
#include "kernel/firmware.h"

extern const uchar fw_mt7601u_start[], fw_mt7601u_end[];

static struct firmware builtin[] = {
  { "mt7601u.bin", fw_mt7601u_start, 0 },
};

int
request_firmware(const struct firmware **fw, const char *name)
{
  builtin[0].size = fw_mt7601u_end - fw_mt7601u_start;
  for(uint i = 0; i < sizeof(builtin)/sizeof(builtin[0]); i++)
    if(strncmp(builtin[i].name, name, 32) == 0){
      *fw = &builtin[i];
      return 0;
    }
  printf("firmware: %s not built in\n", name);
  return -1;
}
