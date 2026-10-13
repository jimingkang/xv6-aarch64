#ifndef _UAPI_ELF_H
#define _UAPI_ELF_H

#include "types.h"

#define ELF_MAGIC 0x464C457FU

struct elfhdr {
  uint magic;
  uchar elf[12];
  ushort type;
  ushort machine;
  uint version;
  uint64 entry;
  uint64 phoff;
  uint64 shoff;
  uint flags;
  ushort ehsize;
  ushort phentsize;
  ushort phnum;
  ushort shentsize;
  ushort shnum;
  ushort shstrndx;
};

struct proghdr {
  uint32 type;
  uint32 flags;
  uint64 off;
  uint64 vaddr;
  uint64 paddr;
  uint64 filesz;
  uint64 memsz;
  uint64 align;
};

#define ELF_PROG_LOAD       1
#define ELF_PROG_FLAG_EXEC  1
#define ELF_PROG_FLAG_WRITE 2
#define ELF_PROG_FLAG_READ  4

#endif
