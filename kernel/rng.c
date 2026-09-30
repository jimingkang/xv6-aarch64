#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#define BCM2835_RNG       (PERIPHERAL_BASE + 0x104000UL)
#define RNG_CTRL          0x00
#define RNG_STATUS        0x04
#define RNG_DATA          0x08
#define RNG_INT_MASK      0x10
#define RNG_RBGEN         0x01
#define RNG_INT_OFF       0x01
#define RNG_WARMUP_COUNT  0x40000

static struct spinlock rnglock;

static uint32
rngread(uint64 offset)
{
  return *(volatile uint32 *)(BCM2835_RNG + offset);
}

static void
rngwrite(uint64 offset, uint32 value)
{
  *(volatile uint32 *)(BCM2835_RNG + offset) = value;
}

void
rnginit(void)
{
  initlock(&rnglock, "rng");
  rngwrite(RNG_INT_MASK, rngread(RNG_INT_MASK) | RNG_INT_OFF);
  rngwrite(RNG_STATUS, RNG_WARMUP_COUNT);
  rngwrite(RNG_CTRL, rngread(RNG_CTRL) | RNG_RBGEN);
}

// Return raw bytes from the BCM2835/BCM2837 hardware RNG.  A bounded wait is
// important under emulators that do not implement this MMIO device.
int
rngbytes(void *destination, int length)
{
  uchar *out = destination;
  uint64 deadline;
  int done = 0;

  if(length < 0)
    return -1;
  acquire(&rnglock);
  while(done < length){
    deadline = r_cntvct_el0() + r_cntfrq_el0();
    while((rngread(RNG_STATUS) >> 24) == 0){
      if(r_cntvct_el0() >= deadline){
        release(&rnglock);
        return -1;
      }
    }
    uint32 value = rngread(RNG_DATA);
    for(int i = 0; i < 4 && done < length; i++, done++)
      out[done] = value >> (i * 8);
  }
  release(&rnglock);
  return done;
}
