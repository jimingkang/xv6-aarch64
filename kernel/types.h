typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
#ifdef XV6_ARM32
typedef unsigned long long uint64;
#else
typedef unsigned long uint64;
#endif
typedef unsigned long uintptr;

#ifdef XV6_ARM32
typedef uint32 pde_t;
#else
typedef uint64 pde_t;
#endif
