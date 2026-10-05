#include "kernel/types.h"
#include "kernel/param.h"
#include "kernel/memlayout.h"
#include "kernel/aarch64.h"
#include "kernel/spinlock.h"
#include "kernel/proc.h"
#include "kernel/defs.h"
/* Short-descriptor L1 roots must be 16 KiB aligned. A bounded root pool is
   separate from the 4 KiB allocator; each root is reclaimed on exit/exec. */
#define ROOTS 256
static uint32 roots[ROOTS][4096] __attribute__((aligned(16384)));
static uchar used[ROOTS];
static struct spinlock rootlock;
pagetable_t kernel_pagetable;
static void install(pagetable_t p) {
  uint32 base = (uintptr)p, zero = 0, domain = 1;
  dsb();
  asm volatile("mcr p15, 0, %0, c2, c0, 2"::"r"(zero):"memory");
  asm volatile("mcr p15, 0, %0, c2, c0, 0"::"r"(base):"memory");
  asm volatile("mcr p15, 0, %0, c3, c0, 0"::"r"(domain):"memory");
  flush_tlb();
}
static pagetable_t rootalloc(void) {
  acquire(&rootlock);
  for(int i=0;i<ROOTS;i++) if(!used[i]) {
    used[i]=1; memset(roots[i],0,sizeof(roots[i]));
    release(&rootlock); return roots[i];
  }
  release(&rootlock); return 0;
}
void kvminit(void) {
  initlock(&rootlock,"arm-l1");
  kernel_pagetable=rootalloc();
  /* Privileged-only identity sections. Caches stay off for initial bringup. */
  for(uint a=EXTMEM; a<PHYSTOP; a+=0x100000)
    kernel_pagetable[a>>20]=a | (1<<12) | (1<<10) | 2;
#ifdef T113_QEMU
  kernel_pagetable[0x08000000>>20]=0x08000000 | (1<<10) | (1<<4) | 2;
  kernel_pagetable[0x09000000>>20]=0x09000000 | (1<<10) | (1<<4) | 2;
#else
  for(uint a=0x02000000; a<0x04000000; a+=0x100000)
    kernel_pagetable[a>>20]=a | (1<<10) | (1<<4) | 2;
#endif
}
void kvminithart(void) {
  install(kernel_pagetable);
  uint v; asm volatile("mrc p15, 0, %0, c1, c0, 0":"=r"(v));
  v |= 1; asm volatile("mcr p15, 0, %0, c1, c0, 0"::"r"(v):"memory"); isb();
}
pte_t *walk(pagetable_t p, uint64 va, int alloc) {
  if(va>=MAXVA) return 0;
  uint i=(uint)va>>20;
  if((p[i]&3)==0) {
    if(!alloc) return 0;
    void *tab=kalloc(); if(!tab) return 0;
    memset(tab,0,PGSIZE); p[i]=(uintptr)tab | 1;
  }
  if((p[i]&3)!=1) return 0;
  return &((uint32*)(uintptr)(p[i]&~1023U))[((uint)va>>12)&255];
}
int mappages(pagetable_t p,uint64 va,uint64 size,uint64 pa,uint64 perm) {
  if(!size || va>=MAXVA || size>MAXVA-va || (pa&4095)) return -1;
  uint64 last=PGROUNDDOWN(va+size-1);
  for(va=PGROUNDDOWN(va);;va+=PGSIZE,pa+=PGSIZE) {
    pte_t *e=walk(p,va,1); if(!e) return -1;
    if(*e&PTE_V) panic("arm remap");
    *e=(uint32)pa | (uint32)perm | PTE_V;
    if(va==last) break;
  }
  return 0;
}
void kvmmap(pagetable_t p,uint64 va,uint64 pa,uint64 sz,uint64 flags) {
  if(mappages(p,va,sz,pa,flags)<0) panic("kvmmap");
}
pagetable_t uvmcreate(void) {
  pagetable_t p=rootalloc(); if(!p) return 0;
  /* Also inherit device mappings, which lie below MAXVA. User allocation
     is bounded below MMIO (32 MiB); device entries remain privileged. */
  memmove(p,kernel_pagetable,16384); return p;
}
uint64 walkaddr(pagetable_t p,uint64 va) {
  pte_t *e=walk(p,va,0);
  if(!e || !(*e&PTE_V) || !(*e&PTE_U)) return 0;
  return PTE2PA(*e);
}
uint64 uva2ka(pagetable_t p,uint64 va) { return walkaddr(p,va); }
void uvmunmap(pagetable_t p,uint64 va,uint64 n,int freepages) {
  for(uint64 a=va;a<va+n*PGSIZE;a+=PGSIZE) {
    pte_t *e=walk(p,a,0); if(!e || !(*e&PTE_V)) continue;
    if(freepages) kfree((void*)(uintptr)PTE2PA(*e)); *e=0;
  }
  flush_tlb();
}
uint64 uvmdealloc(pagetable_t p,uint64 old,uint64 n) {
  if(n>=old) return old;
  uint64 a=PGROUNDUP(n), b=PGROUNDUP(old);
  if(a<b) uvmunmap(p,a,(b-a)/PGSIZE,1);
  return n;
}
uint64 uvmalloc(pagetable_t p,uint64 old,uint64 n) {
  if(n<old) return old;
  if(n>=0x02000000) return 0;
  uint64 start=PGROUNDUP(old), a;
  for(a=start;a<n;a+=PGSIZE) {
    void *mem=kalloc(); if(!mem) goto fail;
    memset(mem,0,PGSIZE);
    if(mappages(p,a,PGSIZE,(uintptr)mem,PTE_NORMAL|PTE_U)<0) {
      kfree(mem); goto fail;
    }
  }
  return n;
fail:
  uvmunmap(p,start,(a-start)/PGSIZE,1); return 0;
}
void uvminit(pagetable_t p,uchar *code,uint sz) {
  if(sz>=PGSIZE || !uvmalloc(p,0,PGSIZE)) panic("uvminit");
  memmove((void*)(uintptr)walkaddr(p,0),code,sz);
}
void uvmfree(pagetable_t p,uint64 sz) {
  if(sz) uvmunmap(p,0,PGROUNDUP(sz)/PGSIZE,1);
  for(uint i=0;i<32;i++) if((p[i]&3)==1) {
    kfree((void*)(uintptr)(p[i]&~1023U)); p[i]=0;
  }
  acquire(&rootlock);
  for(int i=0;i<ROOTS;i++) if(p==roots[i]) { used[i]=0; release(&rootlock); return; }
  panic("arm root free");
}
void uvmclear(pagetable_t p,uint64 va) { pte_t *e=walk(p,va,0); if(!e) panic("uvmclear"); *e &= ~PTE_U; }
int uvmcopy(pagetable_t old,pagetable_t n,uint64 sz) {
  uint64 a;
  for(a=0;a<sz;a+=PGSIZE) {
    pte_t *e=walk(old,a,0); if(!e || !(*e&PTE_V)) panic("uvmcopy");
    void *m=kalloc(); if(!m) goto fail;
    memmove(m,(void*)(uintptr)PTE2PA(*e),PGSIZE);
    if(mappages(n,a,PGSIZE,(uintptr)m,PTE_FLAGS(*e)&~PTE_V)<0) { kfree(m); goto fail; }
  }
  return 0;
fail: uvmunmap(n,0,a/PGSIZE,1); return -1;
}
int copyout(pagetable_t p,uint64 dst,char *src,uint64 len) {
  while(len) {
    uint64 a=PGROUNDDOWN(dst), pa=walkaddr(p,a), n=PGSIZE-(dst-a);
    if(!pa) return -1; if(n>len) n=len;
    memmove((void*)(uintptr)(pa+dst-a),src,n); len-=n; src+=n; dst+=n;
  } return 0;
}
int copyin(pagetable_t p,char *dst,uint64 src,uint64 len) {
  while(len) {
    uint64 a=PGROUNDDOWN(src), pa=walkaddr(p,a), n=PGSIZE-(src-a);
    if(!pa) return -1; if(n>len) n=len;
    memmove(dst,(void*)(uintptr)(pa+src-a),n); len-=n; dst+=n; src+=n;
  } return 0;
}
int copyinstr(pagetable_t p,char *dst,uint64 src,uint64 max) {
  while(max--) { if(copyin(p,dst,src++,1)<0) return -1; if(!*dst++) return 0; }
  return -1;
}
void switchuvm(struct proc *p) { install(p->vm->pagetable); }
void switchkvm(void) { install(kernel_pagetable); }
void uvmsync_icache(pagetable_t p,uint64 sz) {
  (void)p; (void)sz; uint zero=0; dsb();
  asm volatile("mcr p15, 0, %0, c7, c5, 0"::"r"(zero):"memory"); isb();
}
void kvmdump(void) { printf("ARMv7 short descriptor MMU; kernel identity mapped\n"); }
void uvmdump(pagetable_t p,int pid,char *name,char *why) {
  printf("ARM32 vm pid=%d %s %s root=%x\n",pid,name,why,(uintptr)p);
}
