#include "kernel/types.h"
#include "user/user.h"

// Kernel PTY smoke test. It verifies both independent byte streams without
// depending on SSH framing or cryptography.
int
main(void)
{
  int fd[2];
  char buf[32];
  int n;

  if(pty_open(fd) < 0){
    printf("ptytest: pty_open failed\n");
    exit(1);
  }
  if(write(fd[0], "input-to-slave\n", 15) != 15 ||
     (n = read(fd[1], buf, sizeof(buf))) != 15 ||
     memcmp(buf, "input-to-slave\n", 15) != 0){
    printf("ptytest: master-to-slave failed\n");
    exit(1);
  }
  if((n = read(fd[0], buf, sizeof(buf))) != 16 ||
     memcmp(buf, "input-to-slave\r\n", 16) != 0){
    printf("ptytest: input echo failed\n");
    exit(1);
  }
  if(write(fd[0], "\033[A\n", 4) != 4 ||
     (n = read(fd[1], buf, sizeof(buf))) != 15 ||
     memcmp(buf, "input-to-slave\n", 15) != 0 ||
     (n = read(fd[0], buf, sizeof(buf))) != 16 ||
     memcmp(buf, "input-to-slave\r\n", 16) != 0){
    printf("ptytest: history recall failed\n");
    exit(1);
  }
  if(write(fd[1], "output-to-master\n", 17) != 17 ||
     (n = read(fd[0], buf, sizeof(buf))) != 18 ||
     memcmp(buf, "output-to-master\r\n", 18) != 0){
    printf("ptytest: slave-to-master failed\n");
    exit(1);
  }
  close(fd[0]);
  close(fd[1]);
  printf("ptytest: ok\n");
  exit(0);
}
