#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "Usage: mv old-path new-path\n");
    exit(1);
  }
  if(rename(argv[1], argv[2]) < 0){
    fprintf(2, "mv: cannot rename %s to %s\n", argv[1], argv[2]);
    exit(1);
  }
  exit(0);
}
