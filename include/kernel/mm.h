#ifndef XV6_MM_H
#define XV6_MM_H

// Virtual memory areas: the Linux mm_struct/vm_area_struct model, reduced to
// what xv6 needs for mmap(2).
//
//   struct vmspace (mm_struct)
//     pagetable, sz            [0, sz): text/data/heap/stack, managed by
//                              exec/sbrk exactly as before
//     mmap -> vma -> vma ...   one vm_area_struct per mmap() region, sorted
//                              by address, all above MMAP_MIN
//
// A VMA describes what a range *should* contain; the page table only holds
// what has been touched so far.  The first access to a page takes a data or
// instruction abort, usertrap() calls handle_mm_fault(), and the VMA decides
// how the page is produced:
//
//   anonymous private      zero-filled private page
//   file      private      private page filled from the file (MAP_PRIVATE)
//   anonymous shared       page in the shared page cache, zero-filled
//   file      shared       page in the shared page cache keyed by inode,
//                          written back by munmap()/msync()/exit
//   device (VM_PFNMAP)     driver's f_op->mmap() installs PTEs for physical
//                          frames with remap_pfn_range(), or vm_ops->fault
//                          returns a frame on demand

#include <kernel/types.h>

struct file;
struct vmspace;
struct vm_area_struct;

// User virtual address window handed out by mmap() without MAP_FIXED.  The
// heap (sbrk) may grow up to the lowest VMA; mmap() allocates top-down.
// The highest 256 MiB below the 256 GiB TTBR0 limit is left free, for the
// old fixed Unicam window and as a guard.
#define MMAP_MIN   0x0000001000000000ULL   // 64 GiB
#define MMAP_TOP   0x0000003ff0000000ULL   // 256 GiB - 256 MiB

// vm_flags, same bit values as Linux include/linux/mm.h.
#define VM_READ      0x00000001UL
#define VM_WRITE     0x00000002UL
#define VM_EXEC      0x00000004UL
#define VM_SHARED    0x00000008UL
#define VM_MAYWRITE  0x00000020UL   // file opened for writing
#define VM_PFNMAP    0x00000400UL   // raw physical frames, no struct page
#define VM_IO        0x00004000UL   // memory-mapped I/O
#define VM_DONTCOPY  0x00020000UL   // fork() does not inherit this VMA
// xv6 extension: the driver does not need the struct file kept alive.  A
// /dev/video0 mapping must not keep the camera streaming after close(fd).
#define VM_NOFILEREF 0x80000000UL

// vm_fault.flags
#define FAULT_FLAG_WRITE  0x01
#define FAULT_FLAG_INSTR  0x02
#define FAULT_FLAG_KERNEL 0x04      // copyin/copyout touching a user page

// vm_ops->fault() and handle_mm_fault() results.
#define VM_FAULT_OK       0
#define VM_FAULT_SIGSEGV  1         // no VMA or access not permitted
#define VM_FAULT_SIGBUS   2         // beyond end of file / device refused
#define VM_FAULT_OOM      3

struct vm_fault {
  uint64 address;                   // page-aligned faulting user address
  uint64 pgoff;                     // vma->vm_pgoff + page index in VMA
  int flags;                        // FAULT_FLAG_*
  uint64 pfn;                       // out: physical frame for a PFNMAP VMA
};

struct vm_operations_struct {
  void (*open)(struct vm_area_struct *vma);    // VMA duplicated (fork/split)
  void (*close)(struct vm_area_struct *vma);   // VMA removed
  int (*fault)(struct vm_area_struct *vma, struct vm_fault *vmf);
};

struct vm_area_struct {
  uint64 vm_start;                  // first byte, page aligned
  uint64 vm_end;                    // one past the last byte, page aligned
  uint64 vm_flags;                  // VM_*
  uint64 vm_page_prot;              // memory type: PTE_NORMAL/_NC/PTE_DEVICE
  uint64 vm_pgoff;                  // file offset (or frame) in pages
  struct file *vm_file;             // backing file, 0 if anonymous
  const struct vm_operations_struct *vm_ops;
  void *vm_private_data;            // driver state
  uint64 vm_shmkey;                 // page-cache key of shared anon memory
  struct vmspace *vm_mm;            // owning address space
  struct vm_area_struct *vm_next;   // address-sorted list
};

// mm/mmap.c
void   mmap_init(void);
uint64 do_mmap(struct file *f, uint64 addr, uint64 len, int prot, int flags,
               uint64 off);
int    do_munmap(struct vmspace *vm, uint64 addr, uint64 len);
int    do_msync(struct vmspace *vm, uint64 addr, uint64 len, int flags);
int    handle_mm_fault(struct vmspace *vm, uint64 va, int flags);
int    dup_mmap(struct vmspace *old, struct vmspace *new);
void   exit_mmap(struct vmspace *vm);
void   mmap_abort(struct vmspace *vm);
uint64 mmap_lowest(struct vmspace *vm);
void   mmap_dump(struct vmspace *vm);
int    remap_pfn_range(struct vm_area_struct *vma, uint64 addr, uint64 pfn,
                       uint64 size, uint64 prot);
int    fault_in_user_range(uint64 va, uint64 len, int write);
int    pa_kernel_mapped(uint64 pa);

#endif
