#ifndef XV6_T113_ARM_H
#define XV6_T113_ARM_H
#ifndef __ASSEMBLER__
static inline uint cpuid(void) { return 0; }
static inline void intr_on(void) { asm volatile("cpsie i" ::: "memory"); }
static inline void intr_off(void) { asm volatile("cpsid i" ::: "memory"); }
static inline int intr_get(void) { uint v; asm volatile("mrs %0, cpsr":"=r"(v)); return !(v & 128); }
static inline void isb(void) { asm volatile("isb" ::: "memory"); }
static inline void dsb(void) { asm volatile("dsb sy" ::: "memory"); }
static inline void flush_tlb(void) {
  uint z=0; dsb(); asm volatile("mcr p15, 0, %0, c8, c7, 0"::"r"(z):"memory"); dsb(); isb();
}
static inline uint64 r_cntvct_el0(void) {
  uint lo, hi; asm volatile("mrrc p15, 1, %0, %1, c14":"=r"(lo),"=r"(hi)); return ((uint64)hi<<32)|lo;
}
static inline uint r_cntfrq_el0(void) { uint v; asm volatile("mrc p15, 0, %0, c14, c0, 0":"=r"(v)); return v; }
typedef uint32 pte_t;
typedef uint32 *pagetable_t;
#endif
#define PGSIZE 4096
#define PGSHIFT 12
#define PGROUNDUP(n) (((n)+4095)&~4095ULL)
#define PGROUNDDOWN(n) ((n)&~4095ULL)
/* ARMv7 short descriptors: small page, AP=privileged RW, user none. */
#define PTE_V 2
#define PTE_VALID 2
#define PTE_AF 0
#define PTE_U (2U<<4)
#define PTE_RO (1U<<9)
#define PTE_XN 1
#define PTE_NORMAL ((1U<<6)|(1U<<4)) // TEX=1: normal non-cacheable
#define PTE_DEVICE (1U<<4)
#define PTE_FLAGS(p) ((p)&0xfff)
#define PTE2PA(p) ((p)&0xfffff000U)
#endif
