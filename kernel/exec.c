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
#include "vfs.h"

// An executable comes either from the native xv6 root (a locked inode inside
// a log transaction) or from a VFS mount such as /mnt/ext2 (an open vnode).
struct execsrc {
  struct inode *ip;
  struct vnode *vn;
};

static int
srcread(struct execsrc *src, uint64 dst, uint off, uint n)
{
  if(src->vn)
    return vfsread(src->vn, 0, dst, off, n);
  return readi(src->ip, 0, dst, off, n);
}

static void
srcclose(struct execsrc *src)
{
  if(src->ip){
    iunlockput(src->ip);
    end_op();
    src->ip = 0;
  }
  if(src->vn){
    vfsclose(src->vn);
    src->vn = 0;
  }
}

static int loadseg(pde_t *pgdir, uint64 addr, struct execsrc *src, uint offset, uint sz);

int
exec(char *path, char **argv)
{
  char *s, *last;
  int i, off;
  uint64 argc, sz = 0, sp, ustack[MAXARG], stackbase;
  struct elfhdr elf;
  struct execsrc src = { 0, 0 };
  struct stat st;
  struct proghdr ph;
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

  int vr = vfsopen(path, O_RDONLY, &src.vn);
  if(vr > 0 && (vfsstat(src.vn, &st) < 0 || st.type != T_FILE))
    vr = -1;
  if(vr == 0){
    begin_op();
    if((src.ip = namei(path)) == 0){
      end_op();
      vr = -1;
    } else {
      ilock(src.ip);
    }
  }
  if(vr < 0){
    srcclose(&src);
    acquire(&vm->lock);
    vm->execing = 0;
    release(&vm->lock);
    return -1;
  }

  // Check ELF header
  if(srcread(&src, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
    goto bad;
  if(elf.magic != ELF_MAGIC)
    goto bad;

  if((pagetable = uvmcreate()) == 0)
    goto bad;

  // Load program into memory.
  for(i=0, off=elf.phoff; i<elf.phnum; i++, off+=sizeof(ph)){
    if(srcread(&src, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if(ph.type != ELF_PROG_LOAD)
      continue;
    // Debug trace (normally disabled): ELF segment selected for loading.
    // printf("exec: path=%s load va=%p filesz=%p memsz=%p off=%p\n",
    //        path, ph.vaddr, ph.filesz, ph.memsz, ph.off);
    if(ph.memsz < ph.filesz)
      goto bad;
    if(ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    uint64 sz1;
    if((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz)) == 0)
      goto bad;
    sz = sz1;
    if((ph.vaddr % PGSIZE) != 0)
      goto bad;
    if(loadseg(pagetable, ph.vaddr, &src, ph.off, ph.filesz) < 0)
      goto bad;
  }
  srcclose(&src);

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
  vm->execing = 0;
  release(&vm->lock);
  p->trapframe->elr = elf.entry;  // initial program counter = main
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
  return -1;
}

// Load a program segment into pagetable at virtual address va.
// va must be page-aligned
// and the pages from va to va+sz must already be mapped.
// Returns 0 on success, -1 on failure.
static int
loadseg(pagetable_t pagetable, uint64 va, struct execsrc *src, uint offset, uint sz)
{
  uint i, n;
  uint64 pa;

  for(i = 0; i < sz; i += PGSIZE){
    pa = uva2ka(pagetable, va + i);
    if(pa == 0)
      panic("loadseg: address should exist");
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(srcread(src, (uint64)pa, offset+i, n) != n)
      return -1;
  }
  
  return 0;
}
