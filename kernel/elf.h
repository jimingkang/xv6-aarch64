// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian

struct elfhdr {
  uint magic; uchar elf[12]; ushort type, machine; uint version;
  uint entry, phoff, shoff, flags;
  ushort ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct proghdr {
  uint type, off, vaddr, paddr, filesz, memsz, flags, align;
};

// Values for Proghdr type
#define ELF_PROG_LOAD           1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4
