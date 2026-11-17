#ifndef XV6_MMTRACE_H
#define XV6_MMTRACE_H
// Memory/MMU trace (make MMU_TRACE=n, default 1 on Tiny210):
//   0  off
//   1  boot: DRAM, kernel sections, kalloc range, the kernel L1 map
//      (coalesced sections + attributes) and MMU registers, /proc/iomem
//   2  also one summary line per userinit / fork / exec (ELF segments)
//   3  also every page of a fork/exec: L1 root, L2 tables and user pages
//      taken from kalloc, and each ELF segment chunk read from the file
// Lines start with "mm:", "mmu:" or "exec:" so they can be grepped (and
// replayed by tools/manim/memory_anim.py).
#ifndef MMU_TRACE
#define MMU_TRACE 0
#endif
#define MMTRACE(level) (MMU_TRACE >= (level))
// Level-3 per-page lines only for the process currently in fork/exec.
#define MMTRACE_PAGES() (MMTRACE(3) && myproc() && myproc()->mmtrace)
#endif
