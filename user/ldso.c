// Minimal ELF64/AArch64 user-space dynamic linker.
//
// The kernel maps the main PIE and this interpreter, then starts here with a
// Linux-shaped auxiliary vector following argv.  No libc routine may be used
// until relocation is complete, so this file uses raw system calls and small
// private string/memory helpers.  It currently supports the relocation set
// emitted by this tree's PIE programs and its single DT_NEEDED dependency,
// /lib/libc.so.

#include <uapi/types.h>
#include <uapi/elf.h>
#include <uapi/syscall.h>

#define PGSIZE 4096UL
#define PGROUNDUP(x) (((x) + PGSIZE - 1) & ~(PGSIZE - 1))

#define ET_DYN       3
#define PT_DYNAMIC   2
#define DT_NULL      0
#define DT_NEEDED    1
#define DT_PLTRELSZ  2
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_RELA      7
#define DT_RELASZ    8
#define DT_RELAENT   9
#define DT_STRSZ     10
#define DT_SYMENT    11
#define DT_PLTREL    20
#define DT_JMPREL    23
#define STB_LOCAL    0
#define STB_GLOBAL   1
#define STB_WEAK     2
#define SHN_UNDEF    0
#define R_AARCH64_NONE       0
#define R_AARCH64_ABS64      257
#define R_AARCH64_GLOB_DAT   1025
#define R_AARCH64_JUMP_SLOT  1026
#define R_AARCH64_RELATIVE   1027

#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_R_SYM(i)   ((uint32)((i) >> 32))
#define ELF64_R_TYPE(i)  ((uint32)(i))

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

struct elf64_dyn { uint64 tag, val; };
struct elf64_sym {
  uint32 name;
  uchar info, other;
  uint16 shndx;
  uint64 value, size;
};
struct elf64_rela { uint64 offset, info; long addend; };
struct auxv { uint64 type, value; };

struct image {
  uint64 base, end, dynamic, dynamicsz;
  uint64 strtab, strsz, symtab, syment, hash;
  uint64 rela, relasz, relaent, jmprel, pltrelsz, pltrel;
  uint64 needed;
  uint nsyms, nneeded;
};

extern long __syscall(long, long, long, long, long, long, long);

static long sc(long n, long a, long b, long c, long d)
{ return __syscall(n, a, b, c, d, 0, 0); }
static int xopen(const char *p) { return sc(SYS_open, (long)p, 0, 0, 0); }
static int xclose(int fd) { return sc(SYS_close, fd, 0, 0, 0); }
static int xpread(int fd, void *p, int n, uint64 off)
{ return sc(SYS_pread, fd, (long)p, n, off); }
static char *xsbrk(int n) { return (char *)sc(SYS_sbrk, n, 0, 0, 0); }
static void xwrite(const char *s, int n)
{ sc(SYS_write, 2, (long)s, n, 0); }
static void xexit(int status)
{
  sc(SYS_exit, status, 0, 0, 0);
  for(;;) { }
}

static void zero(void *dst, uint64 n)
{
  uchar *p = dst;
  while(n--) *p++ = 0;
}

// GCC may lower fixed-size structure copies to these freestanding helpers.
// They are intentionally local to ld.so; pulling in ulib.o would introduce
// the very libc dependency the interpreter has to bootstrap.
void *memcpy(void *dst, const void *src, unsigned long n)
{
  uchar *d = dst;
  const uchar *s = src;
  while(n--) *d++ = *s++;
  return dst;
}

void *memset(void *dst, int value, unsigned long n)
{
  uchar *d = dst;
  while(n--) *d++ = value;
  return dst;
}

static int streq(const char *a, const char *b)
{
  while(*a && *a == *b) a++, b++;
  return *a == *b;
}

static void fail(const char *message)
{
  const char prefix[] = "ld.so: ";
  int n = 0;
  while(message[n]) n++;
  xwrite(prefix, sizeof(prefix) - 1);
  xwrite(message, n);
  xwrite("\n", 1);
  xexit(127);
}

static int contains(struct image *img, uint64 address, uint64 length)
{
  return address >= img->base && address + length >= address &&
         address + length <= img->end;
}

static int parse_dynamic(struct image *img)
{
  struct elf64_dyn *d;
  uint32 *hash;

  if(!contains(img, img->dynamic, img->dynamicsz))
    return -1;
  img->syment = sizeof(struct elf64_sym);
  img->relaent = sizeof(struct elf64_rela);
  d = (struct elf64_dyn *)img->dynamic;
  for(uint64 i = 0; i < img->dynamicsz / sizeof(*d); i++, d++){
    if(d->tag == DT_NULL)
      break;
    switch(d->tag){
    case DT_NEEDED:
      img->needed = d->val;
      img->nneeded++;
      break;
    case DT_HASH:     img->hash = img->base + d->val; break;
    case DT_STRTAB:   img->strtab = img->base + d->val; break;
    case DT_STRSZ:    img->strsz = d->val; break;
    case DT_SYMTAB:   img->symtab = img->base + d->val; break;
    case DT_SYMENT:   img->syment = d->val; break;
    case DT_RELA:     img->rela = img->base + d->val; break;
    case DT_RELASZ:   img->relasz = d->val; break;
    case DT_RELAENT:  img->relaent = d->val; break;
    case DT_JMPREL:   img->jmprel = img->base + d->val; break;
    case DT_PLTRELSZ: img->pltrelsz = d->val; break;
    case DT_PLTREL:   img->pltrel = d->val; break;
    }
  }
  if(img->hash == 0 || img->strtab == 0 || img->symtab == 0 ||
     img->syment != sizeof(struct elf64_sym) ||
     img->relaent != sizeof(struct elf64_rela) ||
     !contains(img, img->hash, 8) || !contains(img, img->strtab, img->strsz))
    return -1;
  hash = (uint32 *)img->hash;
  img->nsyms = hash[1];
  return img->nsyms && contains(img, img->symtab,
                    (uint64)img->nsyms * sizeof(struct elf64_sym)) ? 0 : -1;
}

static int lookup(struct image *img, const char *name, uint64 *value)
{
  struct elf64_sym *symbols = (struct elf64_sym *)img->symtab;
  for(uint i = 1; i < img->nsyms; i++){
    struct elf64_sym *s = &symbols[i];
    int bind = ELF64_ST_BIND(s->info);
    if(s->shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK) ||
       s->name >= img->strsz)
      continue;
    if(streq((char *)(img->strtab + s->name), name)){
      *value = img->base + s->value;
      return 1;
    }
  }
  return 0;
}

static int resolve(struct image *owner, uint index, struct image *main,
                   struct image *libc, uint64 *value)
{
  struct elf64_sym *s;
  const char *name;
  int found;
  if(index >= owner->nsyms)
    return -1;
  s = &((struct elf64_sym *)owner->symtab)[index];
  if(ELF64_ST_BIND(s->info) == STB_LOCAL && s->shndx != SHN_UNDEF){
    *value = owner->base + s->value;
    return 0;
  }
  if(s->name >= owner->strsz)
    return -1;
  name = (char *)(owner->strtab + s->name);
  found = lookup(main, name, value);
  if(!found)
    found = lookup(libc, name, value);
  if(!found && ELF64_ST_BIND(s->info) == STB_WEAK){
    *value = 0;
    return 0;
  }
  return found ? 0 : -1;
}

static int relocate_table(struct image *img, struct image *main,
                          struct image *libc, uint64 address, uint64 bytes)
{
  if(bytes == 0)
    return 0;
  if(bytes % sizeof(struct elf64_rela) || !contains(img, address, bytes))
    return -1;
  struct elf64_rela *r = (struct elf64_rela *)address;
  for(uint64 i = 0; i < bytes / sizeof(*r); i++){
    uint type = ELF64_R_TYPE(r[i].info);
    uint symbol = ELF64_R_SYM(r[i].info);
    uint64 *where = (uint64 *)(img->base + r[i].offset);
    uint64 value, sym;
    if(!contains(img, (uint64)where, sizeof(*where)))
      return -1;
    switch(type){
    case R_AARCH64_NONE:
      continue;
    case R_AARCH64_RELATIVE:
      if(symbol) return -1;
      value = img->base + r[i].addend;
      break;
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      if(!symbol || resolve(img, symbol, main, libc, &sym) < 0)
        return -1;
      value = sym + r[i].addend;
      break;
    default:
      return -1;
    }
    *where = value;
  }
  return 0;
}

static int relocate(struct image *img, struct image *main, struct image *libc)
{
  if(img->rela && relocate_table(img, main, libc, img->rela, img->relasz) < 0)
    return -1;
  if(img->jmprel && (img->pltrel != DT_RELA ||
     relocate_table(img, main, libc, img->jmprel, img->pltrelsz) < 0))
    return -1;
  return 0;
}

static int main_image(struct image *img, struct proghdr *ph, uint phnum)
{
  zero(img, sizeof(*img));
  for(uint i = 0; i < phnum; i++){
    uint64 end = ph[i].vaddr + ph[i].memsz;
    if(ph[i].type == PT_DYNAMIC){
      img->dynamic = ph[i].vaddr;
      img->dynamicsz = ph[i].memsz;
    }
    if(ph[i].type == ELF_PROG_LOAD && end > img->end)
      img->end = end;
  }
  return img->dynamic && parse_dynamic(img) == 0 ? 0 : -1;
}

static int load_libc(struct image *img)
{
  struct elfhdr eh;
  struct proghdr ph[16];
  uint64 maxend = 0;
  int fd = xopen("/lib/libc.so");
  if(fd < 0 || xpread(fd, &eh, sizeof(eh), 0) != sizeof(eh) ||
     eh.magic != ELF_MAGIC || eh.type != ET_DYN ||
     eh.phentsize != sizeof(struct proghdr) || eh.phnum > 16 ||
     xpread(fd, ph, eh.phnum * sizeof(ph[0]), eh.phoff) !=
       eh.phnum * sizeof(ph[0]))
    return -1;
  for(uint i = 0; i < eh.phnum; i++)
    if(ph[i].type == ELF_PROG_LOAD && ph[i].vaddr + ph[i].memsz > maxend)
      maxend = ph[i].vaddr + ph[i].memsz;
  char *base = xsbrk(PGROUNDUP(maxend));
  if(base == (char *)-1)
    return -1;
  zero(img, sizeof(*img));
  img->base = (uint64)base;
  img->end = img->base + maxend;
  for(uint i = 0; i < eh.phnum; i++){
    if(ph[i].type == PT_DYNAMIC){
      img->dynamic = img->base + ph[i].vaddr;
      img->dynamicsz = ph[i].memsz;
    }
    if(ph[i].type == ELF_PROG_LOAD && ph[i].filesz &&
       xpread(fd, (void *)(img->base + ph[i].vaddr), ph[i].filesz,
              ph[i].off) != (int)ph[i].filesz)
      return -1;
  }
  xclose(fd);
  return img->dynamic && parse_dynamic(img) == 0 ? 0 : -1;
}

int
ldso_start(int argc, char **argv)
{
  struct auxv *aux = (struct auxv *)(argv + argc + 2); // argv NULL, envp NULL
  uint64 phdr = 0, phent = 0, phnum = 0, entry = 0;
  struct image main, libc;

  for(; aux->type != AT_NULL; aux++){
    if(aux->type == AT_PHDR) phdr = aux->value;
    else if(aux->type == AT_PHENT) phent = aux->value;
    else if(aux->type == AT_PHNUM) phnum = aux->value;
    else if(aux->type == AT_ENTRY) entry = aux->value;
  }
  if(phdr == 0 || phent != sizeof(struct proghdr) || phnum == 0 || entry == 0)
    fail("invalid auxiliary vector");
  if(main_image(&main, (struct proghdr *)phdr, phnum) < 0)
    fail("invalid main dynamic image");
  if(main.nneeded != 1 || main.needed >= main.strsz ||
     !streq((char *)(main.strtab + main.needed), "libc.so") ||
     load_libc(&libc) < 0)
    fail("cannot load /lib/libc.so");
  if(libc.nneeded != 0 || relocate(&libc, &main, &libc) < 0 ||
     relocate(&main, &main, &libc) < 0)
    fail("relocation failed");

  const char ready[] = "ld.so: relocated libc.so in user space\n";
  xwrite(ready, sizeof(ready) - 1);
  int status = ((int (*)(int, char **))entry)(argc, argv);
  xexit(status);
  return 0;
}
