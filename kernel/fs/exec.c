#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/defs.h>
#include <kernel/elf.h>
#include <kernel/stat.h>
#include <kernel/fcntl.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/vfs.h>

// The executable is a locked inode of any mounted filesystem, read through
// its default file operations, inside a log transaction.
struct execsrc {
  struct inode *ip;
};

#define DYN_NAME_MAX    128

struct elf_image {
  uint64 base;
  uint64 end;
};

static int
srcread(struct execsrc *src, uint64 dst, uint off, uint n)
{
  return file_inode_read_locked(src->ip, 0, dst, off, n);
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
                     uint64, struct elf_image *);

// Return the interpreter pathname named by PT_INTERP.  Dynamic executables
// without an interpreter are rejected: libc loading and relocation belong to
// user-space ld.so, not to exec().
static int
readinterp(struct execsrc *src, struct elfhdr *elf, char *path, uint size)
{
  struct proghdr ph;
  int found = 0;

  if(size < 2)
    return -1;
  path[0] = 0;
  for(uint i = 0; i < elf->phnum; i++){
    uint64 off = elf->phoff + (uint64)i * sizeof(ph);
    if(off > 0xffffffffULL ||
       srcread(src, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return -1;
    if(ph.type != PT_INTERP)
      continue;
    if(found || ph.filesz < 2 || ph.filesz > size ||
       ph.off > 0xffffffffULL ||
       srcread(src, (uint64)path, ph.off, ph.filesz) != (int)ph.filesz ||
       path[ph.filesz - 1] != 0)
      return -1;
    found = 1;
  }
  return found ? 0 : -1;
}

static int
imagecontains(struct elf_image *img, uint64 addr, uint64 len)
{
  return addr >= img->base && addr + len >= addr && addr + len <= img->end;
}

static int
loadimage(pagetable_t pagetable, uint64 *psz, struct execsrc *src,
          struct elfhdr *elf, uint64 base, struct elf_image *img)
{
  struct proghdr ph;
  uint64 maxend = 0, minstart = ~0ULL, end, newsz;
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
    if(ph.type != ELF_PROG_LOAD)
      continue;
    loads++;
    if(ph.vaddr < minstart)
      minstart = ph.vaddr;
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
  if(base + minstart < PGROUNDUP(*psz))
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
    if(loadseg(pagetable, base + ph.vaddr, src, ph.off, ph.filesz) < 0)
      return -1;
  }
  return 0;
}

int
exec(char *path, char **argv)
{
  char *s, *last;
  uint64 argc, sz = 0, sp, ustack[MAXARG + 18], stackbase;
  struct elfhdr elf;
  struct execsrc src = { 0 };
  struct execsrc interpsrc = { 0 };
  struct elfhdr interpelf;
  struct elf_image mainimg;
  struct elf_image interpimg;
  char interp[DYN_NAME_MAX];
  uint64 initial_entry = 0;
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
  interp[0] = 0;
  if(dynamic && readinterp(&src, &elf, interp, sizeof(interp)) < 0){
    printf("exec: dynamic %s has no valid PT_INTERP\n", path);
    goto bad;
  }

  if((pagetable = uvmcreate()) == 0)
    goto bad;

  if(loadimage(pagetable, &sz, &src, &elf, 0, &mainimg) < 0)
    goto bad;
  srcclose(&src);

  // Linux-style division of responsibility: exec maps the PIE and the ELF
  // interpreter named by PT_INTERP, but does not open libc, resolve symbols,
  // or apply dynamic relocations.  The interpreter receives main-image facts
  // in auxv and performs all of that after entering EL0.
  if(dynamic){
    uint64 interpbase = PGROUNDUP(sz);
    if(srcopen(&interpsrc, interp) < 0){
      printf("exec: cannot open interpreter %s\n", interp);
      goto bad;
    }
    if(srcread(&interpsrc, (uint64)&interpelf, 0, sizeof(interpelf)) !=
         sizeof(interpelf) ||
       interpelf.magic != ELF_MAGIC || interpelf.elf[0] != 2 ||
       interpelf.elf[1] != 1 || interpelf.machine != EM_AARCH64 ||
       interpelf.type != ET_DYN ||
       loadimage(pagetable, &sz, &interpsrc, &interpelf, interpbase,
                 &interpimg) < 0){
      printf("exec: invalid interpreter %s\n", interp);
      goto bad;
    }
    initial_entry = interpbase + interpelf.entry;
    srcclose(&interpsrc);
    printf("exec: %s -> interpreter %s\n", path, interp);
  } else {
    initial_entry = mainimg.base + elf.entry;
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
    sp -= sp % 16; // AArch64 user SP must be 16-byte aligned
    if(sp < stackbase)
      goto bad;
    if(copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
      goto bad;
    ustack[argc] = sp;
  }
  ustack[argc] = 0;

  // argv is followed by an empty environment and a Linux-compatible auxv.
  // ld.so discovers the already mapped main executable exclusively through
  // this user ABI; it receives no kernel pointer or private exec structure.
  uint64 words = argc + 1;
  ustack[words++] = 0;                         // envp[0]
  if(dynamic){
    ustack[words++] = 3; ustack[words++] = mainimg.base + elf.phoff; // AT_PHDR
    ustack[words++] = 4; ustack[words++] = elf.phentsize;             // AT_PHENT
    ustack[words++] = 5; ustack[words++] = elf.phnum;                 // AT_PHNUM
    ustack[words++] = 6; ustack[words++] = PGSIZE;                    // AT_PAGESZ
    ustack[words++] = 7; ustack[words++] = interpimg.base;            // AT_BASE
    ustack[words++] = 9; ustack[words++] = mainimg.base + elf.entry;  // AT_ENTRY
  }
  ustack[words++] = 0; ustack[words++] = 0;                           // AT_NULL

  // Push argv[], empty envp, and auxv in one contiguous initial-stack table.
  sp -= words * sizeof(uint64);
  sp -= sp % 16;
  if(sp < stackbase)
    goto bad;
  if(copyout(pagetable, sp, (char *)ustack, words*sizeof(uint64)) < 0)
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
  p->trapframe->elr = initial_entry; // dynamic: ld.so; static: main
  p->trapframe->spsr = 0;     // switch to EL0
  p->trapframe->sp = sp; // initial stack pointer
  // Debug trace (normally disabled): installed entry point and user stack.
  // printf("exec: path=%s entry=%p sz=%p sp=%p\n",
  //        path, elf.entry, sz, sp);
  uvmsync_icache(pagetable, sz);
  switchuvm(p);
#if VM_TRACE
  // A fork child still contains its parent's image.  Delay the process's one
  // creation snapshot until its first successful exec has installed the
  // image it will actually run (for example /bin/ls rather than "sh").
  if(!p->vm_trace_exec_printed){
    p->vm_trace_exec_printed = 1;
    procvmtrace(p, "exec-image:first");
  }
#endif
  uvmfree(oldpagetable, oldsz);

  return argc; // this ends up in x0, the first argument to main(argc, argv)

 bad:
  acquire(&vm->lock);
  vm->execing = 0;
  release(&vm->lock);
  if(pagetable)
    uvmfree(pagetable, sz);
  srcclose(&src);
  srcclose(&interpsrc);
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
