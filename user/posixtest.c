#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <ctype.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int counter;

static void *
worker(void *argument)
{
  int loops = *(int *)argument;
  for(int i = 0; i < loops; i++){
    if(pthread_mutex_lock(&lock) != 0)
      return (void *)1;
    counter++;
    pthread_mutex_unlock(&lock);
  }
  return (void *)42;
}

int
main(void)
{
  pthread_t first, second;
  void *result = 0;
  struct stat status;
  sem_t semaphore;
  char buffer[6] = {0};
  int loops = 500;
  int fd, server;
  int child, child_status, pipes[2];
  char cwd[128], pipe_byte = 0;
  char formatted[32];
  char *dynamic;
  struct sockaddr_in address;

  fd = open("posix.tmp", O_CREAT | O_TRUNC | O_RDWR, 0644);
  if(fd < 0 || write(fd, "POSIX", 5) != 5 ||
     lseek(fd, 0, SEEK_SET) != 0 || read(fd, buffer, 5) != 5 ||
     strcmp(buffer, "POSIX") != 0 || fstat(fd, &status) < 0 ||
     !S_ISREG(status.st_mode)){
    printf("posixtest: file API failed\n");
    exit(1);
  }
  close(fd);
  unlink("posix.tmp");

  dynamic = calloc(4, 4);
  if(dynamic == 0 || dynamic[0] != 0 ||
     (dynamic = realloc(dynamic, 64)) == 0 ||
     strstr("xv6-libc", "libc") == 0 || strrchr("a/b", '/') == 0 ||
     !isdigit('7') || !isalpha('x') ||
     snprintf(formatted, sizeof(formatted), "%s-%d", "libc", 86) != 7 ||
     strcmp(formatted, "libc-86") != 0 ||
     getcwd(cwd, sizeof(cwd)) == 0 || cwd[0] != '/'){
    printf("posixtest: libc/string/getcwd API failed\n");
    exit(1);
  }
  free(dynamic);

  if(pipe(pipes) < 0 || dup2(pipes[1], 10) != 10 || close(pipes[1]) < 0 ||
     write(10, "D", 1) != 1 || close(10) < 0 ||
     read(pipes[0], &pipe_byte, 1) != 1 || pipe_byte != 'D' ||
     close(pipes[0]) < 0){
    printf("posixtest: dup2 API failed\n");
    exit(1);
  }

  child = fork();
  if(child == 0)
    exit(getppid() == getpid() ? 99 : 7);
  if(child < 0 || waitpid(child, &child_status, 0) != child ||
     !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 7){
    printf("posixtest: waitpid/getppid API failed status=%d\n", child_status);
    exit(1);
  }

  if(sem_init(&semaphore, 0, 0) < 0 || sem_post(&semaphore) < 0 ||
     sem_wait(&semaphore) < 0 || sem_destroy(&semaphore) < 0){
    printf("posixtest: semaphore API failed\n");
    exit(1);
  }

  if(pthread_create(&first, 0, worker, &loops) != 0 ||
     pthread_create(&second, 0, worker, &loops) != 0 ||
     pthread_join(first, &result) != 0 || result != (void *)42 ||
     pthread_join(second, &result) != 0 || result != (void *)42 ||
     counter != 1000){
    printf("posixtest: pthread API failed counter=%d\n", counter);
    exit(1);
  }
  pthread_mutex_destroy(&lock);

  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(18080);
  address.sin_addr.s_addr = INADDR_ANY;
  server = socket(AF_INET, SOCK_STREAM, 0);
  if(server < 0 || bind(server, (struct sockaddr *)&address,
                        sizeof(address)) < 0 || listen(server, 4) < 0){
    printf("posixtest: socket/bind/listen API failed\n");
    exit(1);
  }
  close(server);
  printf("posixtest: PASS pid=%d tid=%d counter=%d\n",
         getpid(), gettid(), counter);
  exit(0);
}
