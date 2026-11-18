// MMU_TRACE: a readable account of the boot memory map, the two-stage MMU
// bring-up and the page tables of userinit/fork/exec.  See mmu_trace.h.
//
// kvmmake() runs before printf works, so the regions it maps and the table
// pages it takes from memblock are recorded here and printed later by
// mmu_trace_boot().  The bootstrap tables entry.S built are still in .bss
// and are decoded directly.

#include <kernel/types.h>
#include <kernel/param.h>
#include "memlayout.h"
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/page.h>
#include <kernel/memblock.h>
#include <kernel/mmu_trace.h>

int mmu_kvm_building;   // set by kvminit() around kvmmake()

#if MMU_TRACE

extern char etext[], end[], bss_start[], bss_end[];
extern char stack0[];
extern pte_t l1entrypgt[], l2entrypgt[], l1kpgt[], l2kpgt[];
extern pagetable_t kernel_pagetable;
extern uint64 boot_dtb_pa;

// ---- small formatting helpers (printf has no %lx / width) ----------------

#define hx mmu_hx
static char hexbuf[8][20];
static int hexslot;

// Unpadded lower-case hex without 0x, from a small ring of buffers so several
// can appear in one printf.  Only CPU0 at boot or the traced process uses it.
char *
mmu_hx(uint64 v)
{
  char *b = hexbuf[hexslot++ & 7];
  char tmp[20];
  int i = 0, j = 0;
  do {
    tmp[i++] = "0123456789abcdef"[v & 15];
    v >>= 4;
  } while(v);
  while(i > 0)
    b[j++] = tmp[--i];
  b[j] = 0;
  return b;
}

static char *
sz(uint64 bytes)
{
  static char buf[4][24];
  static int slot;
  char *b = buf[slot++ & 3];
  uint64 v;
  char *unit;
  if(bytes >= (1UL << 30) && (bytes & ((1UL << 30) - 1)) == 0){
    v = bytes >> 30; unit = " GiB";
  } else if(bytes >= (1UL << 20) && (bytes & ((1UL << 20) - 1)) == 0){
    v = bytes >> 20; unit = " MiB";
  } else {
    v = (bytes + 1023) >> 10; unit = " KiB";
  }
  char tmp[24];
  int i = 0, j = 0;
  do { tmp[i++] = '0' + v % 10; v /= 10; } while(v);
  while(i > 0) b[j++] = tmp[--i];
  for(; *unit; unit++) b[j++] = *unit;
  b[j] = 0;
  return b;
}

static void
cat(char **p, char *e, char *s)
{
  while(*s && *p < e - 1)
    *(*p)++ = *s++;
  **p = 0;
}

// "Normal-WB RO X kernel" style decoding of a block/page descriptor.
char *
mmu_attr_str(uint64 pte, char *buf, int n)
{
  char *p = buf, *e = buf + n;
  int idx = (pte >> 2) & 7;
  int ap = (pte >> 6) & 3;
  *p = 0;
  cat(&p, e, idx == AI_DEVICE_nGnRnE_IDX ? "Device-nGnRnE" :
             idx == AI_NORMAL_NC_IDX ? "Normal-NC" :
             idx == AI_NORMAL_IDX ? "Normal-WB" : "attr?");
  cat(&p, e, (ap & 2) ? " RO" : " RW");
  cat(&p, e, (ap & 1) ? " EL0+EL1" : " EL1");
  if(pte & PTE_PXN) cat(&p, e, " PXN");
  if(pte & PTE_UXN) cat(&p, e, " UXN");
  if(((pte >> 8) & 3) == 3) cat(&p, e, " ISH");
  if(pte & PTE_COW) cat(&p, e, " COW");
  if(pte & PTE_SHARED) cat(&p, e, " shared");
  return buf;
}

// ---- boot-time recording ---------------------------------------------------

#define NKREG 24
#define NKTAB 48
static struct { char *name; uint64 va, pa, size, perm; int t0, t1; } kreg[NKREG];
static struct { uint64 va, pa; int level, idx; } ktab[NKTAB];
static int nkreg, nktab;

void
mmu_trace_kregion(char *name, uint64 va, uint64 pa, uint64 size, uint64 perm)
{
  if(nkreg > 0)
    kreg[nkreg - 1].t1 = nktab;
  if(nkreg >= NKREG)
    return;
  kreg[nkreg].name = name;
  kreg[nkreg].va = va;
  kreg[nkreg].pa = pa;
  kreg[nkreg].size = size;
  kreg[nkreg].perm = perm;
  kreg[nkreg].t0 = kreg[nkreg].t1 = nktab;
  nkreg++;
}

int
mmu_trace_proc_on(void)
{
  struct proc *p;
  if(mmu_kvm_building)
    return 0;
  p = myproc();
  return p && p->mmutrace;
}

// A new table page: level is the level of the NEW table (1 = a root),
// idx the slot in its parent (level - 1).
void
mmu_trace_newtable(pagetable_t pt, uint64 va, int level, int idx, uint64 pa)
{
  if(mmu_kvm_building){
    if(nktab < NKTAB){
      ktab[nktab].va = va;
      ktab[nktab].pa = pa;
      ktab[nktab].level = level;
      ktab[nktab].idx = idx;
      nktab++;
    }
    return;
  }
  if(!MMUTRACE_PAGES())
    return;
  struct proc *p = myproc();
  int user = pt != kernel_pagetable;
  if(user) p->mmu_tables++;
  if(level == 1){
    printf("mmu:   L1 root: kalloc pa %s (user TTBR0 table, 512 x 8-byte entries, empty)\n", hx(pa));
    return;
  }
  uint64 span = 1UL << PXSHIFT(level - 1);
  uint64 base = va & ~(span - 1);
  printf("mmu:   L%d table: kalloc pa %s -> %s L%d[%d] = %s (VA %s-%s)\n",
         level, hx(pa), user ? "user" : "kernel", level - 1, idx,
         hx(PA2PTE(pa) | PTE_TABLE | PTE_VALID), hx(base), hx(base + span - 1));
}

// ---- boot report -----------------------------------------------------------

// Coalesce the valid leaves of one table (level 2 blocks or level 3 pages)
// into runs of equal attributes and contiguous PA.
static void
dump_leaves(char *indent, pagetable_t t, int level, uint64 vbase)
{
  uint64 step = 1UL << PXSHIFT(level);
  char ab[80];
  for(int i = 0; i < 512; ){
    pte_t e = t[i];
    int leaf = level == 3 ? (e & 3) == 3 : (e & 3) == 1;
    if(!leaf){
      i++;
      continue;
    }
    int j = i;
    while(j + 1 < 512){
      pte_t n = t[j + 1];
      int nl = level == 3 ? (n & 3) == 3 : (n & 3) == 1;
      if(!nl || PTE_FLAGS(n) != PTE_FLAGS(e) ||
         PTE2PA(n) != PTE2PA(e) + (uint64)(j + 1 - i) * step)
        break;
      j++;
    }
    uint64 va = vbase + (uint64)i * step;
    uint64 len = (uint64)(j - i + 1) * step;
    printf("mmu: %sL%d[%d..%d] VA %s-%s -> PA %s  %s %s %s\n", indent, level,
           i, j, hx(va), hx(va + len - 1), hx(PTE2PA(e)), sz(len),
           level == 2 ? "block" : "pages", mmu_attr_str(e, ab, sizeof(ab)));
    i = j + 1;
  }
}

static void
dump_bootstrap(char *name, pte_t *l1, pte_t *l2, uint64 vbase)
{
  for(int i = 0; i < 512; i++){
    if((l1[i] & 3) != 3)
      continue;
    printf("mmu:   %s L1[%d] = %s -> L2 at pa %s (VA %s..)\n", name, i,
           hx(l1[i]), hx(PTE2PA(l1[i])), hx(vbase + ((uint64)i << 30)));
    dump_leaves("     ", l2, 2, vbase + ((uint64)i << 30));
  }
}

static void
dump_kernel_table(void)
{
  pagetable_t root = kernel_pagetable;
  int tables = 1, l2b = 0, l3p = 0;
  printf("mmu: final kernel table (TTBR1) root pa %s:\n", hx(V2P(root)));
  for(int i = 0; i < 512; i++){
    pte_t e1 = root[i];
    if((e1 & 3) != 3)
      continue;
    uint64 v1 = KERNBASE + ((uint64)i << 30);
    pagetable_t l2 = (pagetable_t)P2V(PTE2PA(e1));
    tables++;
    printf("mmu:   L1[%d] VA %s-%s -> L2 table pa %s\n", i, hx(v1),
           hx(v1 + (1UL << 30) - 1), hx(PTE2PA(e1)));
    dump_leaves("     ", l2, 2, v1);
    for(int j = 0; j < 512; j++){
      pte_t e2 = l2[j];
      if((e2 & 3) == 1)
        l2b++;
      if((e2 & 3) != 3)
        continue;
      uint64 v2 = v1 + ((uint64)j << 21);
      pagetable_t l3 = (pagetable_t)P2V(PTE2PA(e2));
      tables++;
      int n = 0;
      for(int k = 0; k < 512; k++)
        if((l3[k] & 3) == 3)
          n++;
      l3p += n;
      printf("mmu:     L2[%d] VA %s-%s -> L3 table pa %s (%d of 512 pages)\n",
             j, hx(v2), hx(v2 + (1UL << 21) - 1), hx(PTE2PA(e2)), n);
      dump_leaves("       ", l3, 3, v2);
    }
  }
  printf("mmu: kernel table uses %d table pages (%s): %d x 2 MiB blocks, %d x 4 KiB pages\n",
         tables, sz((uint64)tables * PGSIZE), l2b, l3p);
}

static uint64
r_sysreg_tcr(void) { uint64 x; asm volatile("mrs %0, tcr_el1" : "=r"(x)); return x; }
static uint64
r_sysreg_mair(void) { uint64 x; asm volatile("mrs %0, mair_el1" : "=r"(x)); return x; }
static uint64
r_sysreg_sctlr(void) { uint64 x; asm volatile("mrs %0, sctlr_el1" : "=r"(x)); return x; }
static uint64
r_sysreg_mmfr0(void) { uint64 x; asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(x)); return x; }

static void
dump_regs(void)
{
  uint64 tcr = r_sysreg_tcr(), mair = r_sysreg_mair(), sctlr = r_sysreg_sctlr();
  uint64 mmfr0 = r_sysreg_mmfr0();
  static char *tg0[] = { "4K", "64K", "16K", "?" };
  static char *tg1[] = { "?", "16K", "4K", "64K" };
  static char *ips[] = { "32", "36", "40", "42", "44", "48", "52", "?" };
  static char *rgn[] = { "NC", "WB-WA", "WT", "WB-noWA" };
  int t0 = tcr & 0x3f, t1 = (tcr >> 16) & 0x3f;

  printf("mmu: TTBR0_EL1=%s (user root; 0 while no process runs)  TTBR1_EL1=%s (kernel root)\n",
         hx(r_ttbr0_el1()), hx(r_ttbr1_el1()));
  printf("mmu: TCR_EL1=%s: T0SZ=%d -> user VA 0-%s (%d bits), T1SZ=%d -> kernel VA %s-ffffffffffffffff\n",
         hx(tcr), t0, hx((1UL << (64 - t0)) - 1), 64 - t0, t1, hx(~0UL << (64 - t1)));
  printf("mmu:   TG0=%s TG1=%s granule, walk = L1 -> L2 -> L3 (9+9+9+12 bits, no L0), IPS=%s-bit PA\n",
         tg0[(tcr >> 14) & 3], tg1[(tcr >> 30) & 3], ips[(tcr >> 32) & 7]);
  printf("mmu:   table walks: IRGN0=%s ORGN0=%s SH0=%d, IRGN1=%s ORGN1=%s SH1=%d (3 = inner shareable)\n",
         rgn[(tcr >> 8) & 3], rgn[(tcr >> 10) & 3], (int)((tcr >> 12) & 3),
         rgn[(tcr >> 24) & 3], rgn[(tcr >> 26) & 3], (int)((tcr >> 28) & 3));
  printf("mmu: MAIR_EL1=%s: Attr0=%s Device-nGnRnE, Attr1=%s Normal non-cacheable, Attr2=%s Normal write-back\n",
         hx(mair), hx(mair & 0xff), hx((mair >> 8) & 0xff), hx((mair >> 16) & 0xff));
  printf("mmu: SCTLR_EL1=%s: M(MMU)=%d C(D-cache)=%d I(I-cache)=%d A(align check)=%d WXN=%d\n",
         hx(sctlr), (int)(sctlr & 1), (int)((sctlr >> 2) & 1), (int)((sctlr >> 12) & 1),
         (int)((sctlr >> 1) & 1), (int)((sctlr >> 19) & 1));
  printf("mmu: ID_AA64MMFR0_EL1=%s: CPU supports %s-bit PA, 4K granule %s\n",
         hx(mmfr0), ips[mmfr0 & 7], ((mmfr0 >> 28) & 0xf) == 0 ? "yes" : "no");
}

static void
print_free(uint64 start, uint64 stop)
{
  printf("mmu:   buddy gets PA %s-%s (%s, %d pages)\n", hx(start), hx(stop - 1),
         sz(stop - start), (int)((stop - start) >> PGSHIFT));
}

void
mmu_trace_boot(void)
{
  char ab[80];
  uint64 textsz = (uint64)etext - KERNLINK;

  printf("mmu: ---- MMU_TRACE=%d: memory layout and page tables ----\n", MMU_TRACE);
  printf("mmu: kernel image PA %s-%s, linked at VA %s (KERNBASE %s + PA)\n",
         hx(KERNPA), hx(V2P(end) - 1), hx(KERNLINK), hx(KERNBASE));
  printf("mmu:   text          PA %s-%s %s\n", hx(KERNPA), hx(V2P(etext) - 1), sz(textsz));
  printf("mmu:   rodata+data   PA %s-%s %s\n", hx(V2P(etext)), hx(V2P(bss_start) - 1),
         sz((uint64)bss_start - (uint64)etext));
  printf("mmu:   bss           PA %s-%s %s (stack0 %s, boot tables l1entrypgt %s l2entrypgt %s l1kpgt %s l2kpgt %s)\n",
         hx(V2P(bss_start)), hx(V2P(bss_end) - 1), sz((uint64)bss_end - (uint64)bss_start),
         hx(V2P(stack0)), hx(V2P(l1entrypgt)), hx(V2P(l2entrypgt)), hx(V2P(l1kpgt)), hx(V2P(l2kpgt)));

  printf("mmu: stage 1 (entry.S, MMU off): entrypgtinit() fills 4 static tables, DTB at pa %s\n",
         hx(boot_dtb_pa));
  dump_bootstrap("TTBR0 identity", l1entrypgt, l2entrypgt, 0);
  dump_bootstrap("TTBR1 high", l1kpgt, l2kpgt, KERNBASE);
  printf("mmu:   then TTBR0=l1entrypgt TTBR1=l1kpgt, TCR, MAIR, SCTLR.M=1, br to VA _start\n");

  printf("mmu: stage 2 (kvminit, MMU on, bootstrap map): kvmmake() regions, table pages from memblock below %s:\n",
         hx(EARLYTOP));
  if(nkreg > 0)
    kreg[nkreg - 1].t1 = nktab;
  for(int i = 0; i < nkreg; i++){
    printf("mmu:   map %s VA %s-%s -> PA %s %s %s\n", kreg[i].name, hx(kreg[i].va),
           hx(kreg[i].va + kreg[i].size - 1), hx(kreg[i].pa), sz(kreg[i].size),
           mmu_attr_str(kreg[i].perm | PTE_AF, ab, sizeof(ab)));
    for(int t = kreg[i].t0; t < kreg[i].t1; t++){
      if(ktab[t].level == 1)
        printf("mmu:     L1 root: memblock pa %s\n", hx(ktab[t].pa));
      else
        printf("mmu:     L%d table: memblock pa %s -> L%d[%d] (VA %s..)\n", ktab[t].level,
               hx(ktab[t].pa), ktab[t].level - 1, ktab[t].idx,
               hx(ktab[t].va & ~((1UL << PXSHIFT(ktab[t].level - 1)) - 1)));
    }
  }
  printf("mmu:   kvminithart: TTBR1=%s, TTBR0=0, tlbi vmalle1 -> bootstrap tables retired\n",
         hx(V2P(kernel_pagetable)));
  dump_regs();
  dump_kernel_table();
  {
    uint64 n4k, n2m;
    kvm_directmap(&n4k, &n2m);
    printf("mmu: linear map of RAM: %d x 4 KiB + %d x 2 MiB (DirectMap4k %s, DirectMap2M %s)\n",
           (int)n4k, (int)n2m, sz(n4k * PGSIZE), sz(n2m << 21));
  }
  printf("mmu: page_alloc_init: mem_map pa %s, %d struct page x %d bytes = %s, max_pfn %s\n",
         hx(V2P(mem_map)), (int)max_pfn, (int)sizeof(struct page),
         sz(PGROUNDUP(max_pfn * sizeof(struct page))), hx(max_pfn));
  memblock_free_all(print_free);
  printf("mmu: user VA (TTBR0): 0..sz ELF image + interp, guard + %d stack pages, brk heap, mmap below %s; kernel stacks at KSTACK(slot) below %s\n",
         USTACKPAGES, hx(0x3ff0000000UL), hx(MAXVA));
  printf("mmu: ---- end of boot layout ----\n");
}

#else  // !MMU_TRACE: keep the symbols, do nothing

void mmu_trace_kregion(char *n, uint64 v, uint64 p, uint64 s, uint64 perm) { }
void mmu_trace_newtable(pagetable_t pt, uint64 va, int level, int idx, uint64 pa) { }
void mmu_trace_boot(void) { }
int mmu_trace_proc_on(void) { return 0; }
char *mmu_attr_str(uint64 pte, char *buf, int n) { buf[0] = 0; return buf; }
char *mmu_hx(uint64 v) { return ""; }

#endif
