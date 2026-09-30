#include "kernel/types.h"
#include "kernel/epoll.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define MAX_EVENTS 10
#define PORT 8080

static int
set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int
main(void)
{
  struct epoll_event ev, events[MAX_EVENTS];
  int server_fd = socket_listen(PORT, 10);
  if(server_fd < 0 || set_nonblocking(server_fd) < 0){
    printf("epollserver: cannot listen on %d\n", PORT);
    exit(1);
  }

  int epoll_fd = epoll_create();
  if(epoll_fd < 0){
    close(server_fd);
    exit(1);
  }
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) < 0){
    printf("epollserver: epoll_ctl listener failed\n");
    exit(1);
  }
  printf("Epoll server started on port %d...\n", PORT);

  for(;;){
    int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    if(nfds < 0)
      break;
    for(int i = 0; i < nfds; i++){
      int fd = events[i].data.fd;
      if(fd == server_fd){
        int client_fd = socket_accept(server_fd);
        if(client_fd < 0)
          continue;
        if(set_nonblocking(client_fd) < 0){
          close(client_fd);
          continue;
        }
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = client_fd;
        if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0){
          close(client_fd);
          continue;
        }
        printf("New client connected fd=%d.\n", client_fd);
      } else {
        char buf[1024];
        int len, disconnected = 0;
        // EPOLLET requires draining input until non-blocking read says that
        // no more bytes are currently available.
        while((len = read(fd, buf, sizeof(buf))) > 0){
          int written = write(fd, buf, len);
          if(written < 0)
            break;
        }
        if(len == 0 || (events[i].events & EPOLLERR))
          disconnected = 1;
        if(disconnected){
          epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, 0);
          close(fd);
          printf("Client fd=%d disconnected.\n", fd);
        }
      }
    }
  }
  close(epoll_fd);
  close(server_fd);
  exit(0);
}
