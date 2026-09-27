#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  char buf[512];
  uint64 off = 0;
  int n;
  if(argc != 2){ fprintf(2, "usage: ext2cat /path\n"); exit(1); }
  while((n = ext2read(argv[1], buf, sizeof(buf), off)) > 0){
    if(write(1, buf, n) != n){ fprintf(2, "ext2cat: stdout write failed\n"); exit(1); }
    off += n;
  }
  if(n < 0){ fprintf(2, "ext2cat: cannot read %s\n", argv[1]); exit(1); }
  exit(0);
}
