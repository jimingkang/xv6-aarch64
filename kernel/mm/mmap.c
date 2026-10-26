// mmap(2): virtual memory areas, demand paging and the shared page cache.
//
// See include/kernel/mm.h for the data structures.  Locks, outermost first:
//
//   vm->mmap_lock  rw_semaphore (Linux mmap_lock).  Read for page faults and
//                  msync, write for mmap/munmap/exit.  Sleepable, so a fault
//                  may read a file and munmap may write dirty pages back.
//   vm->lock       spinlock: page-table entries and the vm->mmap list.  The
//                  list is changed only with both locks held, so growproc()
//                  can check the heap against the lowest VMA with vm->lock.
//   pcache.lock    spinlock: the shared page cache.
//
// Page ownership:
//   private (anonymous or MAP_PRIVATE file)  one PTE owns a kalloc() page
//   shared file                              a pcache_page, mapcount = PTEs;
//                                            freed with its last mapping
//   shared anonymous (and /dev/zero)         a pcache_page kept until the
//                                            shm_object, counted per VMA,
//                                            dies: the memory has no file
//   VM_PFNMAP (device)                       no owner; PTE_SPECIAL is set
//
// Dirty tracking: Cortex-A53 has no hardware dirty bit, so a MAP_SHARED file
// page is first mapped read-only even in a PROT_WRITE VMA.  The first store
// takes a permission fault and handle_mm_fault() makes the PTE writable;
// "writable PTE in a shared file VMA" therefore means "dirty".  munmap(),
// msync() and exit write exactly those pages back with f_op->write_at.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/defs.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/device.h>
#include <kernel/mman.h>
#include <kernel/mm.h>

#define MAXUVA (1ULL << 38)     // TTBR0 covers [0, 256 GiB)

// ---------------------------------------------------------------------------
// Small object caches.  kalloc() hands out whole pages; a VMA or a page-cache
// entry is ~100 bytes, so carve pages into objects (a minimal kmem_cache).
// Pages are never returned to kalloc, as in a slab with no shrinker.
// ---------------------------------------------------------------------------
struct objcache {
  struct spinlock lock;
  void *free;                   // first word of a free object links the list
  uint size;
};

static void*
obj_alloc(struct objcache *c)
{
  void *o;

  acquire(&c->lock);
  if(c->free == 0){
    char *page = kalloc();
    if(page == 0){
      release(&c->lock);
      return 0;
    }
    for(uint i = 0; i + c->size <= PGSIZE; i += c->size){
      *(void**)(page + i) = c->free;
      c->free = page + i;
    }
  }
  o = c->free;
  c->free = *(void**)o;
  release(&c->lock);
  memset(o, 0, c->size);
  return o;
}

static void
obj_free(struct objcache *c, void *o)
{
  acquire(&c->lock);
  *(void**)o = c->free;
  c->free = o;
  release(&c->lock);
}

static struct objcache vma_cache;

// ---------------------------------------------------------------------------
// Shared page cache.  A page shared by MAP_SHARED mappings is identified by
// (key, index): key is the in-memory inode for a file (all struct files of
// one inode share it) or the shm_object of shared anonymous memory; index is
// the page offset within that object.  A file page lives exactly as long as
// some PTE maps it: dirty pages are written back before their last mapping
// goes away, so dropping the entry loses nothing.  An anonymous page has no
// backing store and lives as long as its shm_object (Linux shmem inode),
// which every VMA sharing the memory references, mapped or not.
// ---------------------------------------------------------------------------
struct pcache_page {
  struct pcache_page *next;     // hash chain (first: objcache link)
  uint64 key;
  uint64 index;
  uint64 pa;
  int mapcount;
};

struct shm_object {
  struct shm_object *link;      // objcache link while free
  int ref;                      // VMAs sharing this memory
};

#define PCACHE_HASH 64

static struct {
  struct spinlock lock;         // also protects shm_object.ref
  struct pcache_page *hash[PCACHE_HASH];
  struct objcache objs;
  struct objcache shm_objs;
} pcache;

static struct pcache_page**
pcache_slot(uint64 key, uint64 index)
{
  return &pcache.hash[((key >> 6) ^ (index * 31)) % PCACHE_HASH];
}

static struct pcache_page*
pcache_lookup(uint64 key, uint64 index)
{
  for(struct pcache_page *p = *pcache_slot(key, index); p; p = p->next)
    if(p->key == key && p->index == index)
      return p;
  return 0;
}

// One more PTE maps an existing page (fork).
static void
pcache_dup(uint64 key, uint64 index)
{
  acquire(&pcache.lock);
  struct pcache_page *p = pcache_lookup(key, index);
  if(p == 0)
    panic("pcache_dup");
  p->mapcount++;
  release(&pcache.lock);
}

// A PTE stopped mapping the page.  A file page goes with its last mapping;
// an anonymous page (keep) stays for the shm_object.
static void
pcache_put(uint64 key, uint64 index, int keep)
{
  struct pcache_page **pp, *p;
  uint64 pa = 0;

  acquire(&pcache.lock);
  for(pp = pcache_slot(key, index); (p = *pp) != 0; pp = &p->next)
    if(p->key == key && p->index == index)
      break;
  if(p == 0)
    panic("pcache_put");
  if(--p->mapcount == 0 && !keep){
    *pp = p->next;
    pa = p->pa;
  }
  release(&pcache.lock);
  if(pa){
    kfree(P2V(pa));
    obj_free(&pcache.objs, p);
  }
}

static uint64
shm_create(void)
{
  struct shm_object *o = obj_alloc(&pcache.shm_objs);

  if(o)
    o->ref = 1;
  return (uint64)o;
}

static void
shm_get(uint64 key)
{
  acquire(&pcache.lock);
  ((struct shm_object *)key)->ref++;
  release(&pcache.lock);
}

// Drop a VMA's reference; the last one frees every page of the object.
// All its PTEs are gone by then (zap_range runs before vma_free).
static void
shm_put(uint64 key)
{
  struct shm_object *o = (struct shm_object *)key;

  acquire(&pcache.lock);
  if(--o->ref > 0){
    release(&pcache.lock);
    return;
  }
  for(int h = 0; h < PCACHE_HASH; h++){
    struct pcache_page **pp = &pcache.hash[h], *p;
    while((p = *pp) != 0){
      if(p->key != key){
        pp = &p->next;
        continue;
      }
      if(p->mapcount != 0)
        panic("shm_put: page still mapped");
      *pp = p->next;
      kfree(P2V(p->pa));
      obj_free(&pcache.objs, p);
    }
  }
  release(&pcache.lock);
  obj_free(&pcache.shm_objs, o);
}

// ---------------------------------------------------------------------------
// VMA helpers
// ---------------------------------------------------------------------------
void
mmap_init(void)
{
  initlock(&vma_cache.lock, "vma_cache");
  vma_cache.size = sizeof(struct vm_area_struct);
  initlock(&pcache.lock, "pcache");
  initlock(&pcache.objs.lock, "pcache_obj");
  pcache.objs.size = sizeof(struct pcache_page);
  initlock(&pcache.shm_objs.lock, "shm_obj");
  pcache.shm_objs.size = sizeof(struct shm_object);
}

static int
vma_is_shared_file(struct vm_area_struct *vma)
{
  return (vma->vm_flags & (VM_SHARED | VM_PFNMAP)) == VM_SHARED &&
         vma->vm_file != 0;
}

static uint64
vma_key(struct vm_area_struct *vma)
{
  return vma->vm_file ? (uint64)vma->vm_file->ip : vma->vm_shmkey;
}

static uint64
vma_index(struct vm_area_struct *vma, uint64 va)
{
  return vma->vm_pgoff + (va - vma->vm_start) / PGSIZE;
}

// Leaf attributes for a page of this VMA.  EL1 never executes user pages.
static uint64
vma_pte_flags(struct vm_area_struct *vma, int writable)
{
  uint64 perm = vma->vm_page_prot | PTE_PXN;

  if((vma->vm_flags & VM_EXEC) == 0)
    perm |= PTE_UXN;
  return perm | (writable ? PTE_U : PTE_URO);
}

// First VMA with vm_end > va (Linux find_vma), or 0.
static struct vm_area_struct*
find_vma(struct vmspace *vm, uint64 va)
{
  for(struct vm_area_struct *v = vm->mmap; v; v = v->vm_next)
    if(va < v->vm_end)
      return v;
  return 0;
}

static int
range_is_free(struct vmspace *vm, uint64 start, uint64 end)
{
  struct vm_area_struct *v = find_vma(vm, start);
  return v == 0 || v->vm_start >= end;
}

// Insert into the sorted list.  Caller holds mmap_lock for writing.
static void
vma_link(struct vmspace *vm, struct vm_area_struct *vma)
{
  struct vm_area_struct **pp = &vm->mmap;

  acquire(&vm->lock);
  while(*pp && (*pp)->vm_start < vma->vm_start)
    pp = &(*pp)->vm_next;
  vma->vm_next = *pp;
  *pp = vma;
  release(&vm->lock);
}

static void
vma_unlink(struct vmspace *vm, struct vm_area_struct *vma)
{
  acquire(&vm->lock);
  for(struct vm_area_struct **pp = &vm->mmap; *pp; pp = &(*pp)->vm_next)
    if(*pp == vma){
      *pp = vma->vm_next;
      break;
    }
  release(&vm->lock);
}

// A new VMA (split, fork) refers to the same file / anonymous object.
static void
vma_get_refs(struct vm_area_struct *vma)
{
  if(vma->vm_file)
    filedup(vma->vm_file);
  if(vma->vm_shmkey)
    shm_get(vma->vm_shmkey);
  if(vma->vm_ops && vma->vm_ops->open)
    vma->vm_ops->open(vma);
}

static void
vma_free(struct vm_area_struct *vma)
{
  if(vma->vm_ops && vma->vm_ops->close)
    vma->vm_ops->close(vma);
  if(vma->vm_file)
    fileclose(vma->vm_file);
  if(vma->vm_shmkey)
    shm_put(vma->vm_shmkey);
  obj_free(&vma_cache, vma);
}

// Top-down search of [max(MMAP_MIN, heap), MMAP_TOP) for len free bytes.
static uint64
get_unmapped_area(struct vmspace *vm, uint64 len)
{
  uint64 prev_end = PGROUNDUP(vm->sz), best = 0;

  if(prev_end < MMAP_MIN)
    prev_end = MMAP_MIN;
  for(struct vm_area_struct *v = vm->mmap; ; v = v->vm_next){
    uint64 gap_end = v && v->vm_start < MMAP_TOP ? v->vm_start : MMAP_TOP;
    if(gap_end > prev_end && gap_end - prev_end >= len)
      best = gap_end - len;
    if(v == 0)
      break;
    if(v->vm_end > prev_end)
      prev_end = v->vm_end;
  }
  return best;
}

// Lowest address the heap must not grow into.  Caller holds vm->lock.
uint64
mmap_lowest(struct vmspace *vm)
{
  return vm->mmap ? vm->mmap->vm_start : MAXUVA;
}

// Physical ranges the kernel direct map covers with Normal memory, so that
// copyin()/copyout() may reach a PFNMAP page through P2V().  Device memory
// is excluded: memmove() would make unaligned accesses to it.
int
pa_kernel_mapped(uint64 pa)
{
  return pa < PHYSTOP ||
         (pa >= RAMDISK_PA && pa < RAMDISK_PA + RAMDISK_SIZE) ||
         (pa >= CAMDMA_PA && pa < CAMDMA_PA + CAMDMA_SIZE) ||
         (pa >= MODAREA_PA && pa < MODAREA_PA + MODAREA_SIZE);
}

// ---------------------------------------------------------------------------
// File I/O for faults and writeback
// ---------------------------------------------------------------------------

// Fill a page from file offset off.  Returns 0, or VM_FAULT_SIGBUS when the
// page lies wholly beyond end of file (Linux raises SIGBUS there too).  The
// tail of the last partial page reads as zeros.
static int
file_fill_page(struct file *f, uint64 off, char *kva)
{
  struct inode *ip = f->ip;
  int r;

  memset(kva, 0, PGSIZE);
  ireadlock(ip);
  if(off >= ip->size){
    ireadunlock(ip);
    return VM_FAULT_SIGBUS;
  }
  uint n = ip->size - off < PGSIZE ? ip->size - off : PGSIZE;
  r = f->f_op->read_at(f, 0, (uint64)kva, off, n);
  ireadunlock(ip);
  return r < 0 ? VM_FAULT_SIGBUS : VM_FAULT_OK;
}

// Write one dirty page back, never extending the file: bytes past EOF in the
// last page are not part of the file (as in Linux).
static void
writeback_page(struct vm_area_struct *vma, uint64 index, uint64 pa)
{
  struct file *f = vma->vm_file;
  uint64 off = index * PGSIZE, size;

  ireadlock(f->ip);
  size = f->ip->size;
  ireadunlock(f->ip);
  if(off >= size)
    return;
  int n = size - off < PGSIZE ? size - off : PGSIZE;
  if(file_write_at(f, 0, (uint64)P2V(pa), off, n) != n)
    printf("mmap: writeback of ino %d offset %p failed\n", f->ip->inum, off);
}

// Sync the d-cache to the point of unification and invalidate the I-cache,
// so instructions read from a file into a new page are fetched correctly.
static void
sync_icache_page(uint64 pa)
{
  uint64 kva = (uint64)P2V(pa);

  for(uint64 p = kva; p < kva + PGSIZE; p += 64)
    asm volatile("dc cvau, %0" :: "r"(p) : "memory");
  asm volatile("dsb ish\n\tic ialluis\n\tdsb ish\n\tisb" ::: "memory");
}

// ---------------------------------------------------------------------------
// Page fault
// ---------------------------------------------------------------------------

// Find or create the shared page (key, index) and take one mapping on it.
static int
shared_page_get(struct vm_area_struct *vma, uint64 index, uint64 *pa)
{
  uint64 key = vma_key(vma);
  struct pcache_page *p;
  char *mem;
  int r;

  acquire(&pcache.lock);
  if((p = pcache_lookup(key, index)) != 0){
    p->mapcount++;
    *pa = p->pa;
    release(&pcache.lock);
    return VM_FAULT_OK;
  }
  release(&pcache.lock);

  // Not cached: fill a new page without the spinlock (reading may sleep),
  // then publish it unless another fault won the race meanwhile.
  if((mem = kalloc()) == 0)
    return VM_FAULT_OOM;
  if(vma->vm_file){
    if((r = file_fill_page(vma->vm_file, index * PGSIZE, mem)) != 0){
      kfree(mem);
      return r;
    }
  } else
    memset(mem, 0, PGSIZE);

  struct pcache_page *np = obj_alloc(&pcache.objs);
  acquire(&pcache.lock);
  if((p = pcache_lookup(key, index)) != 0){
    p->mapcount++;
    *pa = p->pa;
    release(&pcache.lock);
    kfree(mem);
    if(np)
      obj_free(&pcache.objs, np);
    return VM_FAULT_OK;
  }
  if(np == 0){
    release(&pcache.lock);
    kfree(mem);
    return VM_FAULT_OOM;
  }
  np->key = key;
  np->index = index;
  np->pa = V2P(mem);
  np->mapcount = 1;
  np->next = *pcache_slot(key, index);
  *pcache_slot(key, index) = np;
  release(&pcache.lock);
  *pa = np->pa;
  return VM_FAULT_OK;
}

static int
__handle_mm_fault(struct vmspace *vm, uint64 va, int flags)
{
  struct vm_area_struct *vma = find_vma(vm, va);
  int write = (flags & FAULT_FLAG_WRITE) != 0;
  int special = 0, r;
  uint64 index, pa;
  pte_t *pte;

  if(vma == 0 || va < vma->vm_start)
    return VM_FAULT_SIGSEGV;
  if((vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)) == 0 ||
     (write && (vma->vm_flags & VM_WRITE) == 0) ||
     ((flags & FAULT_FLAG_INSTR) && (vma->vm_flags & VM_EXEC) == 0))
    return VM_FAULT_SIGSEGV;
  va = PGROUNDDOWN(va);
  index = vma_index(vma, va);

  // Present already: only a store to a clean shared page has work to do
  // (mark it dirty by making it writable).  Anything else lost a race with
  // another thread that installed the page first.
  acquire(&vm->lock);
  pte = walk(vm->pagetable, va, 0);
  if(pte && (*pte & PTE_AF)){
    if(write && (*pte & PTE_RO)){
      *pte = (*pte & ~PTE_AP(3)) | PTE_U;
      flush_tlb();
    }
    release(&vm->lock);
    return VM_FAULT_OK;
  }
  release(&vm->lock);

  if(vma->vm_flags & VM_PFNMAP){
    // A driver either installed every PTE in f_op->mmap (remap_pfn_range)
    // or supplies frames on demand here.
    struct vm_fault vmf = { va, index, flags, 0 };
    if(vma->vm_ops == 0 || vma->vm_ops->fault == 0)
      return VM_FAULT_SIGBUS;
    if((r = vma->vm_ops->fault(vma, &vmf)) != VM_FAULT_OK)
      return r;
    pa = vmf.pfn << PGSHIFT;
    special = 1;
  } else if(vma->vm_flags & VM_SHARED){
    if((r = shared_page_get(vma, index, &pa)) != VM_FAULT_OK)
      return r;
  } else {
    char *mem = kalloc();
    if(mem == 0)
      return VM_FAULT_OOM;
    if(vma->vm_file){
      if((r = file_fill_page(vma->vm_file, index * PGSIZE, mem)) != 0){
        kfree(mem);
        return r;
      }
    } else
      memset(mem, 0, PGSIZE);
    pa = V2P(mem);
  }

  // A shared file page starts clean (read-only) unless this is the store
  // that dirties it.  Private and anonymous pages get the VMA's permission.
  int writable = (vma->vm_flags & VM_WRITE) &&
                 (!vma_is_shared_file(vma) || write);
  if((vma->vm_flags & VM_EXEC) && !special)
    sync_icache_page(pa);

  acquire(&vm->lock);
  pte = walk(vm->pagetable, va, 1);
  if(pte == 0 || (*pte & PTE_AF)){
    release(&vm->lock);
    if(special)
      ;
    else if(vma->vm_flags & VM_SHARED)
      pcache_put(vma_key(vma), index, vma->vm_file == 0);
    else
      kfree(P2V(pa));
    return pte ? VM_FAULT_OK : VM_FAULT_OOM;
  }
  *pte = PA2PTE(pa) | vma_pte_flags(vma, writable) | PTE_AF | PTE_V |
         (special ? PTE_SPECIAL : 0);
  flush_tlb();
  release(&vm->lock);
  return VM_FAULT_OK;
}

// Resolve a fault at user address va.  Called from usertrap() for EL0
// aborts, and from copyin()/copyout() with FAULT_FLAG_KERNEL: there the
// kernel may already hold an inode lock, so only try the mmap_lock and let
// the copy fail rather than risk an ABBA deadlock with munmap writeback.
int
handle_mm_fault(struct vmspace *vm, uint64 va, int flags)
{
  int r;

  if(va >= MAXUVA)
    return VM_FAULT_SIGSEGV;
  if(flags & FAULT_FLAG_KERNEL){
    if(!down_read_trylock(&vm->mmap_lock))
      return VM_FAULT_SIGSEGV;
  } else
    down_read(&vm->mmap_lock);
  r = __handle_mm_fault(vm, va, flags);
  up_read(&vm->mmap_lock);
  return r;
}

// Populate the mapped pages of a user buffer before a system call starts
// taking filesystem locks (Linux fault_in_pages_*): read(fd, map, n) then
// finds its destination present and writable.  Pages outside any VMA are
// left to copyin()/copyout() to reject.
int
fault_in_user_range(uint64 va, uint64 len, int write)
{
  struct vmspace *vm = myproc()->vm;
  uint64 a, end;

  if(len == 0 || vm->mmap == 0 || va + len < va)
    return 0;
  end = PGROUNDUP(va + len);
  for(a = PGROUNDDOWN(va); a < end; a += PGSIZE){
    if(a < mmap_lowest(vm))
      continue;
    if(handle_mm_fault(vm, a, write ? FAULT_FLAG_WRITE : 0) != VM_FAULT_OK)
      return -1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Device mappings
// ---------------------------------------------------------------------------

// Map [addr, addr+size) of a VMA being set up by f_op->mmap to physical
// frames starting at pfn, with memory type prot (PTE_DEVICE, PTE_NORMAL_NC,
// PTE_NORMAL).  As in Linux the PTEs are installed immediately and the VMA
// becomes VM_IO | VM_PFNMAP: no struct page, nothing to free on unmap.
int
remap_pfn_range(struct vm_area_struct *vma, uint64 addr, uint64 pfn,
                uint64 size, uint64 prot)
{
  struct vmspace *vm = vma->vm_mm;
  uint64 a, perm;
  pte_t *pte;

  if(addr % PGSIZE || size % PGSIZE || addr < vma->vm_start ||
     addr + size > vma->vm_end)
    return -1;
  vma->vm_flags |= VM_IO | VM_PFNMAP;
  vma->vm_page_prot = prot;
  perm = vma_pte_flags(vma, (vma->vm_flags & VM_WRITE) != 0) | PTE_SPECIAL;

  acquire(&vm->lock);
  for(a = addr; a < addr + size; a += PGSIZE, pfn++){
    if((pte = walk(vm->pagetable, a, 1)) == 0 || (*pte & PTE_AF))
      goto bad;
    *pte = PA2PTE(pfn << PGSHIFT) | perm | PTE_AF | PTE_V;
  }
  flush_tlb();
  release(&vm->lock);
  return 0;

 bad:
  // Remove the prefix installed so far; the frames belong to the device.
  for(uint64 b = addr; b < a; b += PGSIZE)
    *walk(vm->pagetable, b, 0) = 0;
  flush_tlb();
  release(&vm->lock);
  return -1;
}

// ---------------------------------------------------------------------------
// Unmapping
// ---------------------------------------------------------------------------

// Remove the PTEs of [start, end) inside vma.  With writeback, dirty shared
// file pages are first written to the file (this may sleep, so it happens
// before taking vm->lock).  PTEs are cleared in batches; the pages are
// released only after the broadcast TLB invalidation, because the page table
// can be live on another core (threads).
static void
zap_range(struct vm_area_struct *vma, uint64 start, uint64 end, int writeback)
{
  struct vmspace *vm = vma->vm_mm;
  struct { uint64 pa, index; } batch[32];
  uint64 a;
  pte_t *pte;

  if(writeback && vma_is_shared_file(vma)){
    for(a = start; a < end; a += PGSIZE){
      uint64 pa = 0;
      acquire(&vm->lock);
      pte = walk(vm->pagetable, a, 0);
      if(pte && (*pte & PTE_AF) && (*pte & PTE_RO) == 0)
        pa = PTE2PA(*pte);
      release(&vm->lock);
      if(pa)
        writeback_page(vma, vma_index(vma, a), pa);
    }
  }

  a = start;
  while(a < end){
    int n = 0;
    acquire(&vm->lock);
    for(; a < end && n < (int)NELEM(batch); a += PGSIZE){
      pte = walk(vm->pagetable, a, 0);
      if(pte == 0 || (*pte & PTE_AF) == 0)
        continue;
      batch[n].pa = PTE2PA(*pte);
      batch[n].index = vma_index(vma, a);
      n++;
      *pte = 0;
    }
    flush_tlb();
    release(&vm->lock);
    for(int i = 0; i < n; i++){
      if(vma->vm_flags & VM_PFNMAP)
        continue;
      if(vma->vm_flags & VM_SHARED)
        pcache_put(vma_key(vma), batch[i].index, vma->vm_file == 0);
      else
        kfree(P2V(batch[i].pa));
    }
  }
}

// Unmap [start, end).  Caller holds mmap_lock for writing.  A VMA cut in the
// middle is split in two, like Linux __split_vma().
static int
__do_munmap(struct vmspace *vm, uint64 start, uint64 end)
{
  struct vm_area_struct *vma, *next;

  for(vma = find_vma(vm, start); vma && vma->vm_start < end; vma = next){
    next = vma->vm_next;
    if(vma->vm_start < start && vma->vm_end > end){
      struct vm_area_struct *tail = obj_alloc(&vma_cache);
      if(tail == 0)
        return -1;
      *tail = *vma;
      tail->vm_start = end;
      tail->vm_pgoff += (end - vma->vm_start) / PGSIZE;
      vma_get_refs(tail);
      acquire(&vm->lock);
      vma->vm_end = end;
      tail->vm_next = vma->vm_next;
      vma->vm_next = tail;
      release(&vm->lock);
      next = tail;
    }
    uint64 s = vma->vm_start > start ? vma->vm_start : start;
    uint64 e = vma->vm_end < end ? vma->vm_end : end;
    zap_range(vma, s, e, 1);
    if(s == vma->vm_start && e == vma->vm_end){
      vma_unlink(vm, vma);
      vma_free(vma);
    } else {
      acquire(&vm->lock);
      if(s == vma->vm_start){
        vma->vm_pgoff += (e - vma->vm_start) / PGSIZE;
        vma->vm_start = e;
      } else
        vma->vm_end = s;
      release(&vm->lock);
    }
  }
  return 0;
}

int
do_munmap(struct vmspace *vm, uint64 addr, uint64 len)
{
  int r;

  if(addr % PGSIZE || len == 0 || addr + len < addr || addr + len > MAXUVA)
    return -1;
  down_write(&vm->mmap_lock);
  r = __do_munmap(vm, addr, addr + PGROUNDUP(len));
  up_write(&vm->mmap_lock);
  return r;
}

// ---------------------------------------------------------------------------
// mmap
// ---------------------------------------------------------------------------
uint64
do_mmap(struct file *f, uint64 addr, uint64 len, int prot, int flags,
        uint64 off)
{
  struct vmspace *vm = myproc()->vm;
  struct vm_area_struct *vma;
  uint64 vm_flags = 0;
  int type = flags & (MAP_SHARED | MAP_PRIVATE);

  if(len == 0 || PGROUNDUP(len) < len || off % PGSIZE ||
     (type != MAP_SHARED && type != MAP_PRIVATE) ||
     (flags & ~(MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS |
                MAP_POPULATE)) ||
     (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
    return (uint64)-1;
  len = PGROUNDUP(len);
  if(flags & MAP_ANONYMOUS){
    f = 0;
    off = 0;
  }

  // AArch64 cannot express write-only, so PROT_WRITE implies read.
  if(prot & (PROT_READ | PROT_WRITE))
    vm_flags |= VM_READ;
  if(prot & PROT_WRITE)
    vm_flags |= VM_WRITE;
  if(prot & PROT_EXEC)
    vm_flags |= VM_EXEC;
  if(type == MAP_SHARED)
    vm_flags |= VM_SHARED;

  if(f){
    if(!f->readable || f->f_op == 0)
      return (uint64)-1;                  // EACCES / ENODEV
    if(f->f_op->mmap == 0){
      // generic_file_mmap: any filesystem with an offset read backend.
      if(f->type != FD_INODE || f->f_op->read_at == 0 ||
         (type == MAP_SHARED && (prot & PROT_WRITE) &&
          (!f->writable || f->f_op->write_at == 0)))
        return (uint64)-1;
    }
    if(f->writable)
      vm_flags |= VM_MAYWRITE;
  }

  down_write(&vm->mmap_lock);
  if(flags & MAP_FIXED){
    if(addr % PGSIZE || addr < PGROUNDUP(vm->sz) || addr + len < addr ||
       addr + len > MAXUVA || __do_munmap(vm, addr, addr + len) < 0)
      goto bad;
  } else if(addr == 0 || addr % PGSIZE || addr < MMAP_MIN ||
            addr + len < addr || addr + len > MMAP_TOP ||
            !range_is_free(vm, addr, addr + len)){
    // The address is only a hint, as in Linux.
    if((addr = get_unmapped_area(vm, len)) == 0)
      goto bad;
  }

  if((vma = obj_alloc(&vma_cache)) == 0)
    goto bad;
  vma->vm_start = addr;
  vma->vm_end = addr + len;
  vma->vm_flags = vm_flags;
  vma->vm_page_prot = PTE_NORMAL;
  vma->vm_pgoff = off / PGSIZE;
  vma->vm_mm = vm;
  if(f)
    vma->vm_file = filedup(f);
  if(f && f->f_op->mmap && f->f_op->mmap(f, vma) < 0){
    fileclose(vma->vm_file);
    obj_free(&vma_cache, vma);
    goto bad;
  }
  if((vma->vm_flags & VM_NOFILEREF) && vma->vm_file){
    fileclose(vma->vm_file);
    vma->vm_file = 0;
  }
  // Shared anonymous memory (MAP_SHARED|MAP_ANONYMOUS, /dev/zero) needs an
  // object that outlives any one mapping of it.
  if((vma->vm_flags & (VM_SHARED | VM_PFNMAP)) == VM_SHARED &&
     vma->vm_file == 0 && (vma->vm_shmkey = shm_create()) == 0){
    vma_free(vma);
    goto bad;
  }
  vma_link(vm, vma);
  up_write(&vm->mmap_lock);

  if(flags & MAP_POPULATE)
    for(uint64 a = addr; a < addr + len; a += PGSIZE)
      handle_mm_fault(vm, a, 0);
  return addr;

 bad:
  up_write(&vm->mmap_lock);
  return (uint64)-1;
}

// ---------------------------------------------------------------------------
// msync: write dirty shared file pages back and mark them clean again.  The
// PTE is made read-only *before* the write, so a store racing with msync
// faults and re-dirties the page instead of being lost.
// ---------------------------------------------------------------------------
int
do_msync(struct vmspace *vm, uint64 addr, uint64 len, int flags)
{
  uint64 end = addr + PGROUNDUP(len);
  struct vm_area_struct *vma;
  int r = 0;

  if(addr % PGSIZE || end < addr ||
     (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)))
    return -1;
  down_read(&vm->mmap_lock);
  uint64 a = addr;
  for(vma = find_vma(vm, addr); vma && vma->vm_start < end;
      vma = vma->vm_next){
    if(vma->vm_start > a)
      r = -1;                             // ENOMEM: hole in the range
    uint64 s = vma->vm_start > addr ? vma->vm_start : addr;
    uint64 e = vma->vm_end < end ? vma->vm_end : end;
    a = e;
    if(!vma_is_shared_file(vma))
      continue;
    for(uint64 p = s; p < e; p += PGSIZE){
      uint64 pa = 0;
      acquire(&vm->lock);
      pte_t *pte = walk(vm->pagetable, p, 0);
      if(pte && (*pte & PTE_AF) && (*pte & PTE_RO) == 0){
        pa = PTE2PA(*pte);
        *pte |= PTE_URO;
        flush_tlb();
      }
      release(&vm->lock);
      if(pa)
        writeback_page(vma, vma_index(vma, p), pa);
    }
  }
  if(a < end)
    r = -1;
  up_read(&vm->mmap_lock);
  return r;
}

// ---------------------------------------------------------------------------
// fork and exit
// ---------------------------------------------------------------------------

// Copy the parent's VMAs and the pages they have so far into the child.
// Caller holds old->mmap_lock for reading and old->lock; the child is not
// running yet.  Private pages are copied (this kernel has no copy-on-write),
// shared and device pages are mapped again.  On failure the caller releases
// the partial copy with mmap_abort().
int
dup_mmap(struct vmspace *old, struct vmspace *new)
{
  struct vm_area_struct **tail = &new->mmap;

  for(struct vm_area_struct *v = old->mmap; v; v = v->vm_next){
    if(v->vm_flags & VM_DONTCOPY)
      continue;
    struct vm_area_struct *n = obj_alloc(&vma_cache);
    if(n == 0)
      return -1;
    *n = *v;
    n->vm_mm = new;
    n->vm_next = 0;
    vma_get_refs(n);
    *tail = n;
    tail = &n->vm_next;

    for(uint64 a = v->vm_start; a < v->vm_end; a += PGSIZE){
      pte_t *pte = walk(old->pagetable, a, 0), *npte;
      if(pte == 0 || (*pte & PTE_AF) == 0)
        continue;
      if((npte = walk(new->pagetable, a, 1)) == 0)
        return -1;
      if(v->vm_flags & VM_PFNMAP){
        *npte = *pte;
      } else if(v->vm_flags & VM_SHARED){
        pcache_dup(vma_key(v), vma_index(v, a));
        *npte = *pte;
      } else {
        char *mem = kalloc();
        if(mem == 0)
          return -1;
        memmove(mem, P2V(PTE2PA(*pte)), PGSIZE);
        *npte = (*pte & ~PA2PTE(~0ULL)) | PA2PTE(V2P(mem));
      }
    }
  }
  return 0;
}

static void
__exit_mmap(struct vmspace *vm, int writeback)
{
  struct vm_area_struct *v;

  while((v = vm->mmap) != 0){
    zap_range(v, v->vm_start, v->vm_end, writeback);
    vma_unlink(vm, v);
    vma_free(v);
  }
}

// Tear down every mapping of a process that exits or execs, writing dirty
// shared file pages back first.
void
exit_mmap(struct vmspace *vm)
{
  if(vm->mmap == 0)
    return;
  down_write(&vm->mmap_lock);
  __exit_mmap(vm, 1);
  up_write(&vm->mmap_lock);
}

// Release a fork child's partial copy without sleeping (no writeback: the
// parent still maps every dirty page; file references are not the last).
void
mmap_abort(struct vmspace *vm)
{
  __exit_mmap(vm, 0);
}

// vmmap: list the VMAs next to the page-table dump.  Caller may hold
// p->lock, so take only vm->lock.
void
mmap_dump(struct vmspace *vm)
{
  acquire(&vm->lock);
  if(vm->mmap)
    printf("  VMAs (mmap):\n");
  for(struct vm_area_struct *v = vm->mmap; v; v = v->vm_next){
    char perm[5] = {
      (v->vm_flags & VM_READ) ? 'r' : '-',
      (v->vm_flags & VM_WRITE) ? 'w' : '-',
      (v->vm_flags & VM_EXEC) ? 'x' : '-',
      (v->vm_flags & VM_SHARED) ? 's' : 'p', 0 };
    printf("   %p-%p %s pgoff=%p ", v->vm_start, v->vm_end, perm,
           v->vm_pgoff);
    if(v->vm_flags & VM_PFNMAP)
      printf("device%s\n", (v->vm_flags & VM_IO) ? " io" : "");
    else if(v->vm_file)
      printf("file dev=%d ino=%d\n", v->vm_file->ip->dev, v->vm_file->ip->inum);
    else
      printf("anon\n");
  }
  release(&vm->lock);
}
