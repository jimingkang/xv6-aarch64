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
