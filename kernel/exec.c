#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "stat.h"
#include "fcntl.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "vfs.h"

// The executable is a locked inode of any mounted filesystem, read through
// its filesystem's read operation, inside a log transaction.
struct execsrc {
  struct inode *ip;
};

#define DYN_LIBC_PATH   "/lib/libc.so"
#define DYN_MAX_NEEDED  4
#define DYN_NAME_MAX    128

// Shared libc.so text.
//
// libc.so is position independent and has no DT_TEXTREL: every dynamic
// relocation lands in its RW segment (GOT/.data).  Its read-only pages are
// therefore byte-identical in every process whatever base exec() picks, so the
// first exec loads them from the file once and later execs map the same
// physical pages read-only.  The RW page is still loaded and relocated per
// process.  The cache is keyed by the file's (dev, inum, size); if libc.so is
// replaced, a new set of pages is cached and the old set is left allocated
// because processes started earlier may still map it.
#define SHLIB_MAXPAGES  128
#define SHLIB_PTE       (PTE_NORMAL | PTE_URO | PTE_PXN | PTE_SHARED)

struct shlib_cache {
  struct spinlock lock;
  int valid;
  uint dev, inum;
  uint64 size;                  // file size, part of the identity check
  uint64 textsz;                // bytes of shared text, a page multiple
  uint64 pa[SHLIB_MAXPAGES];
};
static struct shlib_cache libc_cache = { .lock = { .name = "libc_text" } };

struct elf_image {
  uint64 base;
  uint64 end;
  uint64 sharedend;     // [base, sharedend) is shared read-only text, or 0
  uint64 dynamic;
  uint64 dynamicsz;
  uint64 strtab;
  uint64 strsz;
  uint64 symtab;
  uint64 syment;
  uint64 hash;
  uint64 rela;
  uint64 relasz;
  uint64 relaent;
  uint64 jmprel;
  uint64 pltrelsz;
  uint64 pltrel;
  uint64 needed[DYN_MAX_NEEDED];
  uint nneeded;
  uint nsyms;
};

static int
srcread(struct execsrc *src, uint64 dst, uint off, uint n)
{
  return src->ip->iop->read(src->ip, 0, dst, off, n);
}

static void
srcclose(struct execsrc *src)
{
  if(src->ip){
    iunlockput(src->ip);
    end_op();
    src->ip = 0;
  }
}

static int
srcopen(struct execsrc *src, char *path)
{
  begin_op();
  if((src->ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(src->ip);
  if(src->ip->type != T_FILE){
    srcclose(src);
    return -1;
  }
  return 0;
}

static int loadseg(pagetable_t, uint64, struct execsrc *, uint64, uint64);
static int loadimage(pagetable_t, uint64 *, struct execsrc *, struct elfhdr *,
                     uint64, struct elf_image *, uint64);
static int parsedynamic(pagetable_t, struct elf_image *);
static int relocateimage(pagetable_t, struct elf_image *,
                         struct elf_image *, struct elf_image *);

static int
imagecontains(struct elf_image *img, uint64 addr, uint64 len)
{
  return addr >= img->base && addr + len >= addr && addr + len <= img->end;
}

static int
imageread(pagetable_t pagetable, struct elf_image *img, uint64 addr,
          void *dst, uint64 len)
{
  if(!imagecontains(img, addr, len))
    return -1;
  return copyin(pagetable, dst, addr, len);
}

static int
imagewrite(pagetable_t pagetable, struct elf_image *img, uint64 addr,
           void *src, uint64 len)
{
  if(!imagecontains(img, addr, len))
    return -1;
  return copyout(pagetable, addr, src, len);
}

static int
dynstring(pagetable_t pagetable, struct elf_image *img, uint64 off,
          char *buf, uint max)
{
  uint64 remain;

  if(img->strtab == 0 || off >= img->strsz || max == 0)
    return -1;
  remain = img->strsz - off;
  if(remain > max)
    remain = max;
  if(!imagecontains(img, img->strtab + off, remain))
    return -1;
  return copyinstr(pagetable, buf, img->strtab + off, remain);
}

static int
readsym(pagetable_t pagetable, struct elf_image *img, uint idx,
        struct elf64_sym *sym)
{
  uint64 addr;

  if(img->syment != sizeof(*sym) || idx >= img->nsyms)
    return -1;
  addr = img->symtab + (uint64)idx * img->syment;
  return imageread(pagetable, img, addr, sym, sizeof(*sym));
}

static int
lookupsym(pagetable_t pagetable, struct elf_image *img, char *name,
          uint64 *value)
{
  struct elf64_sym sym;
  char candidate[DYN_NAME_MAX];

  for(uint i = 1; i < img->nsyms; i++){
    if(readsym(pagetable, img, i, &sym) < 0)
      return -1;
    if(sym.shndx == SHN_UNDEF ||
       (ELF64_ST_BIND(sym.info) != STB_GLOBAL &&
        ELF64_ST_BIND(sym.info) != STB_WEAK))
      continue;
    if(dynstring(pagetable, img, sym.name, candidate, sizeof(candidate)) < 0)
      return -1;
    if(strncmp(candidate, name, DYN_NAME_MAX) == 0){
      *value = img->base + sym.value;
      return 1;
    }
  }
  return 0;
}

static int
resolvesym(pagetable_t pagetable, struct elf_image *owner, uint symidx,
           struct elf_image *mainimg, struct elf_image *libcimg,
           uint64 *value)
{
  struct elf64_sym sym;
  char name[DYN_NAME_MAX];
  int found;

  if(readsym(pagetable, owner, symidx, &sym) < 0)
    return -1;
  if(ELF64_ST_BIND(sym.info) == STB_LOCAL && sym.shndx != SHN_UNDEF){
    *value = owner->base + sym.value;
    return 0;
  }
  if(dynstring(pagetable, owner, sym.name, name, sizeof(name)) < 0)
    return -1;
  found = lookupsym(pagetable, mainimg, name, value);
  if(found < 0)
    return -1;
  if(found == 0 && libcimg){
    found = lookupsym(pagetable, libcimg, name, value);
    if(found < 0)
      return -1;
  }
  if(found == 0){
    if(ELF64_ST_BIND(sym.info) == STB_WEAK){
      *value = 0;
      return 0;
    }
    printf("exec: unresolved dynamic symbol %s\n", name);
    return -1;
  }
  return 0;
}

static int
relocatetable(pagetable_t pagetable, struct elf_image *img,
              struct elf_image *mainimg, struct elf_image *libcimg,
              uint64 table, uint64 tablesz)
{
  struct elf64_rela rela;
  uint64 value, symvalue, where;
  uint type, symidx;

  if(tablesz == 0)
    return 0;
  if(img->relaent != sizeof(rela) || tablesz % sizeof(rela) != 0 ||
     !imagecontains(img, table, tablesz))
    return -1;
  for(uint64 off = 0; off < tablesz; off += sizeof(rela)){
    if(imageread(pagetable, img, table + off, &rela, sizeof(rela)) < 0)
      return -1;
    type = ELF64_R_TYPE(rela.info);
    symidx = ELF64_R_SYM(rela.info);
    where = img->base + rela.offset;
    if(!imagecontains(img, where, sizeof(value)))
      return -1;
    if(where < img->sharedend){
      // A text relocation would make the "shared" pages base-specific.
      printf("exec: relocation into shared text at %p\n", where);
      return -1;
    }
    switch(type){
    case R_AARCH64_NONE:
      continue;
    case R_AARCH64_RELATIVE:
      if(symidx != 0)
        return -1;
      value = img->base + rela.addend;
      break;
    case R_AARCH64_ABS64:
    case R_AARCH64_GLOB_DAT:
    case R_AARCH64_JUMP_SLOT:
      if(symidx == 0 ||
         resolvesym(pagetable, img, symidx, mainimg, libcimg, &symvalue) < 0)
        return -1;
      value = symvalue + rela.addend;
      break;
    default:
      printf("exec: unsupported AArch64 dynamic relocation %d\n", type);
      return -1;
    }
    if(imagewrite(pagetable, img, where, &value, sizeof(value)) < 0)
      return -1;
  }
  return 0;
}

static int
relocateimage(pagetable_t pagetable, struct elf_image *img,
              struct elf_image *mainimg, struct elf_image *libcimg)
{
  if(img->rela && relocatetable(pagetable, img, mainimg, libcimg,
                                img->rela, img->relasz) < 0)
    return -1;
  if(img->jmprel){
    if(img->pltrel != DT_RELA)
      return -1;
    if(relocatetable(pagetable, img, mainimg, libcimg,
                     img->jmprel, img->pltrelsz) < 0)
      return -1;
  }
  return 0;
}

static int
parsedynamic(pagetable_t pagetable, struct elf_image *img)
{
  struct elf64_dyn dyn;
  uint32 hashhdr[2];
  uint64 count;
  int terminated = 0;

  if(img->dynamic == 0 || img->dynamicsz < sizeof(dyn) ||
     !imagecontains(img, img->dynamic, img->dynamicsz))
    return -1;
  img->syment = sizeof(struct elf64_sym);
  img->relaent = sizeof(struct elf64_rela);
  count = img->dynamicsz / sizeof(dyn);
  for(uint64 i = 0; i < count; i++){
    if(imageread(pagetable, img, img->dynamic + i*sizeof(dyn),
                 &dyn, sizeof(dyn)) < 0)
      return -1;
    if(dyn.tag == DT_NULL){
      terminated = 1;
      break;
    }
    switch(dyn.tag){
    case DT_NEEDED:
      if(img->nneeded >= DYN_MAX_NEEDED)
        return -1;
      img->needed[img->nneeded++] = dyn.val;
      break;
    case DT_HASH:     img->hash = img->base + dyn.val; break;
    case DT_STRTAB:   img->strtab = img->base + dyn.val; break;
    case DT_STRSZ:    img->strsz = dyn.val; break;
    case DT_SYMTAB:   img->symtab = img->base + dyn.val; break;
    case DT_SYMENT:   img->syment = dyn.val; break;
    case DT_RELA:     img->rela = img->base + dyn.val; break;
    case DT_RELASZ:   img->relasz = dyn.val; break;
    case DT_RELAENT:  img->relaent = dyn.val; break;
    case DT_JMPREL:   img->jmprel = img->base + dyn.val; break;
    case DT_PLTRELSZ: img->pltrelsz = dyn.val; break;
    case DT_PLTREL:   img->pltrel = dyn.val; break;
    }
  }
  if(!terminated || img->hash == 0 || img->strtab == 0 || img->strsz == 0 ||
     img->symtab == 0 || img->syment != sizeof(struct elf64_sym) ||
     img->relaent != sizeof(struct elf64_rela) ||
     imageread(pagetable, img, img->hash, hashhdr, sizeof(hashhdr)) < 0)
    return -1;
  img->nsyms = hashhdr[1];
  if(img->nsyms == 0 ||
     !imagecontains(img, img->symtab,
                    (uint64)img->nsyms * sizeof(struct elf64_sym)) ||
     !imagecontains(img, img->strtab, img->strsz))
    return -1;
  return 0;
}

// premapped != 0: the caller already mapped [base, base+premapped) with the
// shared libc.so text, so only the remaining pages are allocated and only the
// segments above that range are read from the file.
static int
loadimage(pagetable_t pagetable, uint64 *psz, struct execsrc *src,
          struct elfhdr *elf, uint64 base, struct elf_image *img,
          uint64 premapped)
{
  struct proghdr ph;
  uint64 maxend = 0, end, newsz;
  int loads = 0;

  if(elf->phentsize != sizeof(ph) || elf->phnum == 0 ||
     elf->phoff + (uint64)elf->phnum * sizeof(ph) < elf->phoff)
    return -1;
  memset(img, 0, sizeof(*img));
  img->base = base;

  // GNU ld can place an RW PT_LOAD part way into a page.  Validate the ELF
  // alignment relationship, but do not impose xv6's old page-aligned-vaddr
  // restriction on PIE/shared images.
  for(uint i = 0; i < elf->phnum; i++){
    uint64 off = elf->phoff + (uint64)i * sizeof(ph);
    if(off > 0xffffffffULL ||
       srcread(src, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return -1;
    if(ph.type == PT_DYNAMIC){
      if(ph.vaddr + ph.memsz < ph.vaddr || base + ph.vaddr < base)
        return -1;
      img->dynamic = base + ph.vaddr;
      img->dynamicsz = ph.memsz;
    }
    if(ph.type != ELF_PROG_LOAD)
      continue;
    loads++;
    if(ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr ||
       base + ph.vaddr < base || ph.off + ph.filesz < ph.off ||
       ph.off + ph.filesz > 0xffffffffULL)
      return -1;
    if(ph.align > 1 && ((ph.align & (ph.align - 1)) != 0 ||
                        ((ph.vaddr - ph.off) & (ph.align - 1)) != 0))
      return -1;
    end = ph.vaddr + ph.memsz;
    if(end > maxend)
      maxend = end;
  }
  if(loads == 0 || base + maxend < base)
    return -1;
  if(premapped){
    if(PGROUNDUP(*psz) != base + premapped || maxend < premapped)
      return -1;
  } else if(base < PGROUNDUP(*psz))
    return -1;
  end = base + maxend;
  if((newsz = uvmalloc(pagetable, *psz, end)) == 0)
    return -1;
  *psz = newsz;
  img->end = end;

  // uvmalloc supplied zero-filled BSS.  Copy each file-backed prefix at its
  // exact virtual address; loadseg handles a non-page-aligned first byte.
  for(uint i = 0; i < elf->phnum; i++){
    uint64 off = elf->phoff + (uint64)i * sizeof(ph);
    if(srcread(src, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return -1;
    if(ph.type != ELF_PROG_LOAD || ph.filesz == 0)
      continue;
    if(premapped && ph.vaddr < premapped){
      // Already present in the shared pages, which must hold all of it.
      if(ph.vaddr + ph.memsz > premapped)
        return -1;
      continue;
    }
    if(loadseg(pagetable, base + ph.vaddr, src, ph.off, ph.filesz) < 0)
      return -1;
  }
  return 0;
}

// Bytes at the start of a shared object that can be shared between processes:
// whole pages holding only read-only PT_LOAD contents, ending below the first
// page that contains writable data.  0 if the layout does not allow it.
static uint64
shlib_textsize(struct execsrc *src, struct elfhdr *elf)
{
  struct proghdr ph;
  uint64 wmin = ~0ULL, romax = 0, lowest = ~0ULL, end;

  for(uint i = 0; i < elf->phnum; i++){
    uint64 off = elf->phoff + (uint64)i * sizeof(ph);
    if(off > 0xffffffffULL ||
       srcread(src, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return 0;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    if(ph.vaddr < lowest)
      lowest = ph.vaddr;
    if(ph.flags & ELF_PROG_FLAG_WRITE){
      if(ph.vaddr < wmin)
        wmin = ph.vaddr;
    } else if(ph.vaddr + ph.memsz > romax){
      romax = ph.vaddr + ph.memsz;
    }
  }
  if(lowest != 0 || romax == 0)
    return 0;
  end = wmin == ~0ULL ? PGROUNDUP(romax) : PGROUNDDOWN(wmin);
  if(romax > end || end / PGSIZE > SHLIB_MAXPAGES)
    return 0;
  return end;
}

static int
shlib_match(uint dev, uint inum, uint64 size, uint64 textsz)
{
  return libc_cache.valid && libc_cache.dev == dev &&
         libc_cache.inum == inum && libc_cache.size == size &&
         libc_cache.textsz == textsz;
}

// Map the cached text pages at base.  Returns 1 if mapped, 0 on a cache miss,
// -1 if a page-table page could not be allocated.  *psz advances page by page
// so a failed exec unmaps exactly what was mapped.
static int
shlib_map(pagetable_t pagetable, uint64 *psz, uint64 base,
          uint dev, uint inum, uint64 size, uint64 textsz)
{
  int r = 0;

  acquire(&libc_cache.lock);
  if(shlib_match(dev, inum, size, textsz)){
    r = 1;
    for(uint64 off = 0; off < textsz; off += PGSIZE){
      if(mappages(pagetable, base + off, PGSIZE,
                  libc_cache.pa[off / PGSIZE], SHLIB_PTE) != 0){
        r = -1;
        break;
      }
      *psz = base + off + PGSIZE;
    }
  }
  release(&libc_cache.lock);
  return r;
}

// Cache miss: this exec loaded and relocated a private copy.  Its text pages
// are now known to be base-independent, so hand them to the cache and remap
// them read-only.  The new page table is not live yet, so no TLB flush.
static void
shlib_promote(pagetable_t pagetable, uint64 base,
              uint dev, uint inum, uint64 size, uint64 textsz)
{
  pte_t *pte;

  acquire(&libc_cache.lock);
  if(shlib_match(dev, inum, size, textsz)){
    // Another exec filled the cache meanwhile; keep this private copy.
    release(&libc_cache.lock);
    return;
  }
  for(uint64 off = 0; off < textsz; off += PGSIZE){
    pte = walk(pagetable, base + off, 0);
    if(pte == 0 || (*pte & PTE_V) != PTE_V){
      release(&libc_cache.lock);
      return;
    }
  }
  if(libc_cache.valid)
    printf("exec: %s changed, caching new text\n", DYN_LIBC_PATH);
  for(uint64 off = 0; off < textsz; off += PGSIZE){
    pte = walk(pagetable, base + off, 0);
    uint64 pa = PTE2PA(*pte);
    *pte = PA2PTE(pa) | SHLIB_PTE | PTE_AF | PTE_V;
    libc_cache.pa[off / PGSIZE] = pa;
  }
  libc_cache.dev = dev;
  libc_cache.inum = inum;
  libc_cache.size = size;
  libc_cache.textsz = textsz;
  libc_cache.valid = 1;
  release(&libc_cache.lock);
}

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct execsrc src = { 0 };
  struct execsrc libsrc = { 0 };
  struct elfhdr libelf;
  struct elf_image mainimg, libcimg;
  int dynamic = 0;
  pagetable_t pagetable = 0, oldpagetable;
  struct proc *p = myproc();
  struct vmspace *vm = p->vm;

  // Replacing a shared address space underneath another thread is unsafe.
  // This first implementation requires callers to join all other threads
  // before exec. execing closes the race with a concurrent clone().
  acquire(&vm->lock);
  if(vm->refcount != 1 || vm->execing){
    release(&vm->lock);
    return -1;
  }
  vm->execing = 1;
  release(&vm->lock);

  if(srcopen(&src, path) < 0){
    acquire(&vm->lock);
    vm->execing = 0;
    release(&vm->lock);
    return -1;
  }

  // Check ELF header
  if(srcread(&src, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC || elf.elf[0] != 2 || elf.elf[1] != 1 ||
     elf.machine != EM_AARCH64 ||
     (elf.type != ET_EXEC && elf.type != ET_DYN))
    goto bad;
  dynamic = elf.type == ET_DYN;

  if((pagetable = uvmcreate()) == 0)
    goto bad;

  if(loadimage(pagetable, &sz, &src, &elf, 0, &mainimg, 0) < 0)
    goto bad;
  srcclose(&src);

  if(dynamic){
    char needed[DYN_NAME_MAX];
    uint64 libbase = PGROUNDUP(sz);

    if(parsedynamic(pagetable, &mainimg) < 0 || mainimg.nneeded != 1)
      goto bad;
    // This first ABI intentionally supports one trusted library and one
    // deterministic pathname.  Arbitrary DT_NEEDED search is left to a future
    // user-space ld.so rather than being hidden inside the kernel.
    for(i = 0; i < mainimg.nneeded; i++){
      if(dynstring(pagetable, &mainimg, mainimg.needed[i], needed,
                   sizeof(needed)) < 0 ||
         strncmp(needed, "libc.so", DYN_NAME_MAX) != 0){
        printf("exec: unsupported shared-library dependency\n");
        goto bad;
      }
    }
    if(srcopen(&libsrc, DYN_LIBC_PATH) < 0){
      printf("exec: cannot open %s\n", DYN_LIBC_PATH);
      goto bad;
    }
    if(srcread(&libsrc, (uint64)&libelf, 0, sizeof(libelf)) != sizeof(libelf) ||
       libelf.magic != ELF_MAGIC || libelf.elf[0] != 2 || libelf.elf[1] != 1 ||
       libelf.machine != EM_AARCH64 || libelf.type != ET_DYN)
      goto bad;
    uint libdev = libsrc.ip->dev, libinum = libsrc.ip->inum;
    uint64 libfsz = libsrc.ip->size;
    uint64 textsz = shlib_textsize(&libsrc, &libelf);
    int shared = 0;
    if(textsz &&
       (shared = shlib_map(pagetable, &sz, libbase,
                           libdev, libinum, libfsz, textsz)) < 0)
      goto bad;
    if(loadimage(pagetable, &sz, &libsrc, &libelf, libbase, &libcimg,
                 shared ? textsz : 0) < 0)
      goto bad;
    libcimg.sharedend = textsz ? libbase + textsz : 0;
    srcclose(&libsrc);
    if(parsedynamic(pagetable, &libcimg) < 0 || libcimg.nneeded != 0 ||
       relocateimage(pagetable, &libcimg, &mainimg, &libcimg) < 0 ||
       relocateimage(pagetable, &mainimg, &mainimg, &libcimg) < 0)
      goto bad;
    if(textsz && !shared)
      shlib_promote(pagetable, libbase, libdev, libinum, libfsz, textsz);
    printf("exec: dynamic %s, libc.so base=%p, text %s\n", path, libbase,
           shared ? "shared" : textsz ? "loaded" : "private");
  }

  if(!imagecontains(&mainimg, mainimg.base + elf.entry, 1))
    goto bad;

  p = myproc();
  uint64 oldsz;

  // Allocate one inaccessible guard page followed by a multi-page user
  // stack.  One 4 KiB page was enough for the original tiny xv6 commands,
  // but is too small for networking/crypto applications such as tftp and
  // sshd, whose local packet buffers and printf/write call chains overlap.
  sz = PGROUNDUP(sz);
  uint64 sz1;
  if((sz1 = uvmalloc(pagetable, sz,
                     sz + (USTACKPAGES + 1)*PGSIZE)) == 0)
    goto bad;
  sz = sz1;
  uvmclear(pagetable, sz-(USTACKPAGES + 1)*PGSIZE);
  sp = sz;
  stackbase = sp - USTACKPAGES*PGSIZE;

  // Push argument strings, prepare rest of stack in ustack.
  for(argc = 0; argv[argc]; argc++) {
    if(argc >= MAXARG)
      goto bad;
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 16; // riscv sp must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // push the array of argv[] pointers.
  sp -= (argc+1) * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, (argc+1)*sizeof(uint64)) < 0)
    goto bad;

  // arguments to user main(argc, argv)
  // argc is returned via the system call return
  // value, which goes in x0.
  p->trapframe->x1 = sp;

  // Save program name for debugging.
  for(last=s=path; *s; s++)
    if(*s == '/')
      last = s+1;
  safestrcpy(p->name, last, sizeof(p->name));
    
  // Commit to the user image.
  acquire(&vm->lock);
  oldpagetable = vm->pagetable;
  oldsz = vm->sz;
  vm->pagetable = pagetable;
  vm->sz = sz;
  vm->camera_mapped = 0;       // mmap belongs to the replaced address space
  vm->execing = 0;
  release(&vm->lock);
  p->trapframe->elr = mainimg.base + elf.entry; // initial PC = main
  p->trapframe->spsr = 0;     // switch to EL0
  p->trapframe->sp = sp; // initial stack pointer
  // Debug trace (normally disabled): installed entry point and user stack.
  // printf("exec: path=%s entry=%p sz=%p sp=%p\n",
  //        path, elf.entry, sz, sp);
  uvmsync_icache(pagetable, sz);
  switchuvm(p);
  // uvmdump(p->vm->pagetable, p->pid, p->name, "exec-image");
  uvmfree(oldpagetable, oldsz);

  return argc; // this ends up in x0, the first argument to main(argc, argv)

 bad:
  acquire(&vm->lock);
  vm->execing = 0;
  release(&vm->lock);
  if(pagetable)
    uvmfree(pagetable, sz);
  srcclose(&src);
  srcclose(&libsrc);
  return -1;
}

// Load a program segment into already allocated pages.  va may be unaligned,
// as required by ordinary GNU ld PIE/shared-object PT_LOAD layouts.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct execsrc *src,
        uint64 offset, uint64 sz)
{
  uint64 i, n, pageva, pageoff, kva;

  for(i = 0; i < sz; i += n){
    pageva = PGROUNDDOWN(va + i);
    pageoff = va + i - pageva;
    kva = uva2ka(pagetable, pageva);
    if(kva == 0)
      panic("loadseg: address should exist");
    n = PGSIZE - pageoff;
    if(n > sz - i)
      n = sz - i;
    if(offset + i > 0xffffffffULL || n > 0xffffffffULL ||
       srcread(src, kva + pageoff, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
