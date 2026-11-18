#ifndef XV6_MMU_TRACE_H
#define XV6_MMU_TRACE_H

// MMU_TRACE: how memory and page tables are laid out, as a readable story.
// It complements MM_TRACE (one line per allocator event) and VM_TRACE (full
// L1/L2/L3 dumps):
//
//   make MMU_TRACE=0   off (default)
//   make MMU_TRACE=1   boot: kernel sections, the bootstrap tables entry.S
//                      built, TCR/MAIR/SCTLR/TTBR decoded, every kvmmake()
//                      region with the table pages it took from memblock,
//                      a compact map of the final TTBR1 table, mem_map and
//                      the ranges handed to the buddy allocator
//   make MMU_TRACE=2   also one summary per userinit / fork / exec (ELF
//                      program headers, size, stack, entry, pages used)
//   make MMU_TRACE=3   also every page of a fork/exec: new L1/L2/L3 table
//                      pages, user pages from kalloc, COW-shared pages, each
//                      ELF chunk read from the file, the argv copy
//
// Lines start with "mmu:" or "exec:" so they can be grepped and replayed by
// tools/manim/rpi3_memory_anim.py.
#ifndef MMU_TRACE
#define MMU_TRACE 0
#endif
#define MMUTRACE(level) (MMU_TRACE >= (level))
// Level-3 per-page lines only for the process currently inside fork/exec.
#define MMUTRACE_PAGES() (MMUTRACE(3) && mmu_trace_proc_on())

struct proc;
int  mmu_trace_proc_on(void);
void mmu_trace_kregion(char *name, uint64 va, uint64 pa, uint64 size, uint64 perm);
void mmu_trace_newtable(pagetable_t pt, uint64 va, int level, int idx, uint64 pa);
void mmu_trace_boot(void);
char *mmu_attr_str(uint64 pte, char *buf, int n);
char *mmu_hx(uint64 v);         // unpadded hex, small ring of buffers
extern int mmu_kvm_building;

#endif
