// /dev/mem and /dev/zero, after Linux drivers/char/mem.c.
//
//   /dev/mem   mmap(fd, offset = physical address): map device registers
//              (Device-nGnRnE) or memory the kernel allocator does not own
//              into a process with remap_pfn_range().  Like Linux with
//              CONFIG_STRICT_DEVMEM, RAM below PHYSTOP (kernel image and
//              kalloc pages) is refused.
//   /dev/zero  read returns zeros, write discards; MAP_PRIVATE gives
//              zero-filled private pages and MAP_SHARED gives shared
//              anonymous memory, i.e. the mapping is simply anonymous.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/spinlock.h>
#include <kernel/sleeplock.h>
#include <kernel/fs.h>
#include <kernel/file.h>
#include <kernel/defs.h>
#include <kernel/device.h>
#include <kernel/module.h>
#include <kernel/mm.h>

#define MEM_MINOR   1
#define ZERO_MINOR  5

#define PERIPH_END  (LOCAL_BASE_PA + PGSIZE)   // last MMIO page of BCM2837

static int
mem_minor(struct file *f)
{
  return f->ip ? f->ip->minor : -1;
}

// Memory type for a physical range.  An alias must use the same attribute
// as the kernel's own mapping of that range (mixed cacheability is
// architecturally unsafe), and MMIO must be Device memory.
static int
devmem_prot(uint64 pa, uint64 size, uint64 *prot)
{
  uint64 end = pa + size;

  if(end < pa || pa < PHYSTOP || end > PERIPH_END)
    return -1;
  if(pa >= PERIPHERAL_BASE_PA){
    *prot = PTE_DEVICE;
    return 0;
  }
  if(end > PERIPHERAL_BASE_PA)
    return -1;                       // straddles RAM and MMIO
  if((pa >= RAMDISK_PA && end <= RAMDISK_PA + RAMDISK_SIZE) ||
     (pa >= MODAREA_PA && end <= MODAREA_PA + MODAREA_SIZE))
    *prot = PTE_NORMAL;
  else
    *prot = PTE_NORMAL_NC;           // DMA buffers, GPU memory, framebuffer
  return 0;
}

static int
mem_mmap(struct file *f, struct vm_area_struct *vma)
{
  uint64 size = vma->vm_end - vma->vm_start, prot;

  if(mem_minor(f) == ZERO_MINOR){
    vma->vm_flags |= VM_NOFILEREF;   // becomes a plain anonymous mapping
    return 0;
  }
  if(devmem_prot(vma->vm_pgoff << PGSHIFT, size, &prot) < 0)
    return -1;
  if(prot == PTE_DEVICE && (vma->vm_flags & VM_EXEC))
    return -1;
  return remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff, size, prot);
}

static int
mem_fread(struct file *f, int user_dst, uint64 dst, int n)
{
  static char zeros[64];

  if(mem_minor(f) != ZERO_MINOR)
    return -1;                       // /dev/mem: use mmap
  for(int done = 0; done < n; ){
    int m = n - done < (int)sizeof(zeros) ? n - done : (int)sizeof(zeros);
    if(either_copyout(user_dst, dst + done, zeros, m) < 0)
      return done ? done : -1;
    done += m;
  }
  return n;
}

static int
mem_fwrite(struct file *f, int user_src, uint64 src, int n)
{
  return mem_minor(f) == ZERO_MINOR ? n : -1;
}

static void
mem_init(void)
{
  static struct file_operations fops = {
    .fread = mem_fread,
    .fwrite = mem_fwrite,
    .mmap = mem_mmap,
  };
  static struct device mem_device = {
    .name = "mem",
    .class = "mem",
    .devnode = "mem",
    .dev_major = MEM,
    .dev_minor = MEM_MINOR,
  };
  static struct device zero_device = {
    .name = "zero",
    .class = "mem",
    .devnode = "zero",
    .dev_major = MEM,
    .dev_minor = ZERO_MINOR,
  };

  if(register_chrdev(MEM, "mem", &fops) < 0){
    printf("mem: cannot register major %d\n", MEM);
    return;
  }
  if(device_register(&mem_device) < 0 || device_register(&zero_device) < 0)
    printf("mem: cannot register /dev/mem or /dev/zero\n");
}

module_init("mem", mem_init);
