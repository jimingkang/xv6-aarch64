#include "user.h"

int
main(int argc, char **argv)
{
  if(argc != 2){
    fprintf(2, "usage: umount target\n");
    exit(1);
  }
  if(umount(argv[1]) < 0){
    fprintf(2, "umount: %s failed (busy, root, or flush error)\n", argv[1]);
    exit(1);
  }
  exit(0);
}
