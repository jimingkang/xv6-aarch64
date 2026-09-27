#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  char buf[128];
  int n;

  if(argc != 2){
    fprintf(2, "usage: edit file\n");
    exit(1);
  }

  int fd = open(argv[1], O_CREATE | O_WRONLY | O_TRUNC);
  if(fd < 0){
    fprintf(2, "edit: cannot open %s\n", argv[1]);
    exit(1);
  }

  printf("edit: enter text; press Ctrl-D on a new line to save\n");
  while((n = read(0, buf, sizeof(buf))) > 0){
    if(write(fd, buf, n) != n){
      fprintf(2, "edit: write failed\n");
      close(fd);
      exit(1);
    }
  }
  close(fd);
  if(n < 0){
    fprintf(2, "edit: read failed\n");
    exit(1);
  }
  exit(0);
}
