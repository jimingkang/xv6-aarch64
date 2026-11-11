// cxxstart.c -- entry point of C++ programs on xv6.
//
// ld-xv6.so jumps to the program's entry as entry(argc, argv) and exits
// with its return value, but runs no constructors.  C++ programs are linked
// with -e _xv6_cxx_start, which runs .init_array (static constructors), main,
// and then the atexit/static-destructor list and the stream flush.
//
// exec gives a process USTACKPAGES (16 KB) of stack, which C++ numerical
// code with large automatic arrays overruns at once.  The program therefore
// runs on a CXX_STACK_SIZE stack taken from the heap; it stays valid across
// fork, so MPI ranks keep it too.

typedef void (*initfn)(int, char **);

extern initfn __preinit_array_start[] __attribute__((weak, visibility("hidden")));
extern initfn __preinit_array_end[] __attribute__((weak, visibility("hidden")));
extern initfn __init_array_start[] __attribute__((weak, visibility("hidden")));
extern initfn __init_array_end[] __attribute__((weak, visibility("hidden")));

int main(int, char **);
void __xv6cxx_fini(void);
void *malloc(unsigned int);

#define CXX_STACK_SIZE (4u << 20)

// int call_on_stack(int argc, char **argv, int (*fn)(int, char **), void *top)
// x29 is callee-saved, so it still holds the old stack after fn returns.
__asm__(
  ".text\n"
  ".p2align 2\n"
  ".type call_on_stack, %function\n"
  "call_on_stack:\n"
  "  stp x29, x30, [sp, #-16]!\n"
  "  mov x29, sp\n"
  "  mov sp, x3\n"
  "  blr x2\n"
  "  mov sp, x29\n"
  "  ldp x29, x30, [sp], #16\n"
  "  ret\n"
  ".size call_on_stack, .-call_on_stack\n");
int call_on_stack(int, char **, int (*)(int, char **), void *);

static int
run(int argc, char **argv)
{
  for(initfn *f = __preinit_array_start; f < __preinit_array_end; f++)
    (*f)(argc, argv);
  for(initfn *f = __init_array_start; f < __init_array_end; f++)
    (*f)(argc, argv);
  int status = main(argc, argv);
  __xv6cxx_fini();
  return status;
}

int
_xv6_cxx_start(int argc, char **argv)
{
  char *stack = malloc(CXX_STACK_SIZE);
  if(stack == 0)
    return run(argc, argv);
  return call_on_stack(argc, argv, run,
                       (void *)(((unsigned long)(stack + CXX_STACK_SIZE)) & ~15UL));
}
