// Format of an ELF executable file

#define ELF_MAGIC 0x464C457FU  // "\x7FELF" in little endian

// File header
struct elfhdr {
  uint magic;  // must equal ELF_MAGIC
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

// Program section header
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

// Values for Proghdr type
#define ELF_PROG_LOAD           1

// Flag bits for Proghdr flags
#define ELF_PROG_FLAG_EXEC      1
#define ELF_PROG_FLAG_WRITE     2
#define ELF_PROG_FLAG_READ      4

// ---------------------------------------------------------------------------
// Relocatable objects (the .ko modules).
//
// A user program is a linked, ET_EXEC image with program headers, so exec()
// only has to copy PT_LOAD segments straight into an address space.  A loadable
// kernel module is the opposite: `ld -r` leaves it ET_REL, still full of
// unresolved relocations, because at compile time nobody knows where the
// module's text will land or which kernel routine a `bl` will reach.  finit_
// module reads these section headers instead of program headers, reserves
// memory for every SHF_ALLOC section, points every relocation at its final
// home, and only then calls the module's init.  This is the same contract the
// Linux module loader implements in kernel/module.c:apply_relocate_add().
// ---------------------------------------------------------------------------

// Section header.  A .ko carries one of these per section (.text, .rodata,
// .data, .bss, .initcall.list, plus the non-alloc .symtab/.strtab/.rela.text).
struct elf64_shdr {
  uint32 name;       // offset into the section-name string table
  uint32 type;       // SHT_* below
  uint64 flags;      // SHF_* below
  uint64 addr;       // 0 for an object file
  uint64 offset;     // file offset of the section contents
  uint64 size;       // section size in bytes
  uint32 link;       // SHT_RELA -> the symtab it indexes; SHT_SYMTAB -> strtab
  uint32 info;
  uint64 addralign;
  uint64 entsize;    // size of one entry for fixed-size sections
};

#define SHT_NULL        0
#define SHT_PROGBITS    1
#define SHT_NOBITS      8   // .bss: occupies memory but has no bytes in the file
#define SHT_REL         9
#define SHT_RELA        4
#define SHT_SYMTAB      2
#define SHT_STRTAB      3

#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4

// Symbol table entry.  Together with st_info this says whether the symbol is
// defined by the module (so st_value is an offset inside shndx) or still
// undefined (so its name must be resolved against the kernel's export table).
struct elf64_sym {
  uint32 name;       // offset into the symbol string table
  uchar  info;       // low 4 bits: STT_*, high bits: binding
  uchar  other;
  uint16 shndx;      // SHN_UNDEF means the symbol is not defined here
  uint64 value;      // offset within section shndx
  uint64 size;
};

#define STT_SECTION     3   // symbol names a whole section, value 0
#define STT_FUNC        2
#define STT_OBJECT      1
#define SHN_UNDEF       0
#define SHN_ABS         0xfff1

#define ELF64_ST_TYPE(i)  ((i) & 0xf)

// One relocation record.  It says: take the value of symtab[rels->sym], add
// rels->addend, combine it with the address of the place being patched, and
// write the result into the target section according to the reloc type.
struct elf64_rela {
  uint64 offset;     // where in the target section to patch
  uint64 info;       // low 32: symbol index, high 32: R_AARCH64_* type
  long   addend;
};

// In an ELF64 RELA the symbol index lives in the high 32 bits of r_info and
// the relocation type in the low 32 -- the reverse of the ELF32 layout.
#define ELF64_R_SYM(i)    ((uint32)((i) >> 32))
#define ELF64_R_TYPE(i)   ((uint32)((i) & 0xffffffffu))

// AArch64 relocation types a -fno-pic, non-literal-pool-heavy kernel module
// actually emits.  Everything the compiler generates for ordinary C that calls
// other functions and touches globals falls out of this small set.
#define R_AARCH64_NONE                0
#define R_AARCH64_ABS64               257
#define R_AARCH64_ABS32               258
#define R_AARCH64_ABS16               259
#define R_AARCH64_PREL64              260
#define R_AARCH64_PREL32              261
#define R_AARCH64_PREL16              262
#define R_AARCH64_ADR_PREL_PG_HI21    275   // adrp: page delta, bits 12..32
#define R_AARCH64_ADR_PREL_LO21       276   // adr:  byte delta within a page
#define R_AARCH64_ADD_ABS_LO12_NC     277   // add:  low 12 bits of S + A
#define R_AARCH64_ADR_GOT_PAGE        311
#define R_AARCH64_LD64_GOT_LO12_NC    312
#define R_AARCH64_JUMP26              282   // b   <label>
#define R_AARCH64_CALL26              283   // bl  <kernel function>
