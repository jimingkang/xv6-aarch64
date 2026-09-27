#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  if(argc < 2){
    fprintf(2, "usage: touch file...\n");
    exit(1);
  }
  for(int i = 1; i < argc; i++){
    int fd = open(argv[i], O_CREATE | O_WRONLY);
    if(fd < 0){
      fprintf(2, "touch: cannot create %s\n", argv[i]);
      exit(1);
    }
    close(fd);
  }
  exit(0);
}
