#include "kernel/types.h"
#include "kernel/epoll.h"
#include "kernel/fcntl.h"
#include "user/user.h"

// TCP byte-stream smoke test used before the SSH transport is enabled.
// From another machine: nc <wlan0-ip> 2222
int
main(int argc, char **argv)
{
  char buf[256];
  struct epoll_event event, events[8];
  int port = 2222, listener, epfd, n;
  if(argc > 1)
    port = atoi(argv[1]);
  listener = socket_listen(port, 10);
  if(listener < 0){
    printf("tcpd: cannot listen on %d\n", port);
    exit(1);
  }
  printf("tcpd: waiting on port %d\n", port);
  if(fcntl(listener, F_SETFL, O_NONBLOCK) < 0){
    printf("tcpd: cannot set listener non-blocking\n");
    exit(1);
  }
  epfd = epoll_create();
  if(epfd < 0){
    printf("tcpd: epoll_create failed\n");
    close(listener);
    exit(1);
  }
  event.events = EPOLLIN;
  event.data.fd = listener;
  if(epoll_ctl(epfd, EPOLL_CTL_ADD, listener, &event) < 0){
    printf("tcpd: cannot watch listener\n");
    exit(1);
  }
  for(;;){
    n = epoll_wait(epfd, events, 8, -1);
    if(n < 0)
      break;
    for(int i = 0; i < n; i++){
      int fd = events[i].data.fd;
      if(fd == listener){
        int connection = socket_accept(listener);
        if(connection < 0)
          continue;
        printf("tcpd: client connected listener-fd=%d connection-fd=%d\n",
               listener, connection);
        write(connection, "xv6 socket ready\n", 17);
        event.events = EPOLLIN;
        event.data.fd = connection;
        if(epoll_ctl(epfd, EPOLL_CTL_ADD, connection, &event) < 0)
          close(connection);
      } else {
        int got = read(fd, buf, sizeof(buf));
        if(got <= 0 || write(fd, buf, got) != got){
          epoll_ctl(epfd, EPOLL_CTL_DEL, fd, 0);
          close(fd);
          printf("tcpd: client fd=%d disconnected\n", fd);
        }
      }
    }
  }
  close(epfd);
  close(listener);
  exit(1);
}
