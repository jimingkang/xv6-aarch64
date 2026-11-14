#ifdef XV6_AM335X
#include "../ports/am335x/memlayout.h"
#elif defined(XV6_X210)
#include "arch/arm/mach-s5pv210/memlayout.h"
#else
#include "../ports/t113/memlayout.h"
#endif
