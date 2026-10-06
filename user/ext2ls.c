#include "kernel/types.h"
#include "kernel/ext2.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  struct ext2_user_dirent de;
  char *path = argc > 1 ? argv[1] : "/";
  int i, r;
  for(i = 0; ; i++){
    r = ext2readdir(path, i, &de);
    if(r < 0){ fprintf(2, "ext2ls: cannot read %s (not mounted or not a directory)\n", path); exit(1); }
    if(r == 0) break;
    printf("%s%c %l bytes inode %d\n", de.name,
           (de.mode & 0xf000) == 0x4000 ? '/' : ' ', de.size, de.inode);
  }
  exit(0);
}
