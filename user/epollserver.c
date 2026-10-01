#include "kernel/types.h"
#include "kernel/epoll.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define MAX_EVENTS EPOLL_MAX_WATCH
#define PORT 8080
#define OUTPUT_SIZE 4096
#define READ_SIZE 1024

struct client {
  int fd;
  uint32 events;
  int output_len;
  int read_closed;
  char output[OUTPUT_SIZE];
};

static struct client clients[MAX_EVENTS - 1];

static int
set_nonblocking(int fd)
{
  int flags = fcntl(fd, F_GETFL, 0);
  if(flags < 0)
    return -1;
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static struct client*
find_client(int fd)
{
  for(int i = 0; i < MAX_EVENTS - 1; i++)
    if(clients[i].fd == fd)
      return &clients[i];
  return 0;
}

static struct client*
alloc_client(int fd)
{
  for(int i = 0; i < MAX_EVENTS - 1; i++){
    if(clients[i].fd == 0){
      clients[i].fd = fd;
      clients[i].events = EPOLLIN;
      clients[i].output_len = 0;
      clients[i].read_closed = 0;
      return &clients[i];
    }
  }
  return 0;
}

static void
close_client(int epoll_fd, struct client *client)
{
  epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, 0);
  close(client->fd);
  client->fd = 0;
  client->events = 0;
  client->output_len = 0;
  client->read_closed = 0;
}

static int
update_client_events(int epoll_fd, struct client *client)
{
  uint32 events = 0;
  if(!client->read_closed && client->output_len < OUTPUT_SIZE)
    events |= EPOLLIN;
  if(client->output_len > 0)
    events |= EPOLLOUT;
  if(events == client->events)
    return 0;

  struct epoll_event ev;
  ev.events = events;
  ev.data.fd = client->fd;
  if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev) < 0)
    return -1;
  client->events = events;
  return 0;
}

static int
drain_client_input(struct client *client)
{
  char buf[READ_SIZE];
  while(client->output_len < OUTPUT_SIZE){
    int space = OUTPUT_SIZE - client->output_len;
    int size = space < READ_SIZE ? space : READ_SIZE;
    int n = read(client->fd, buf, size);
    if(n == 0)
      return 1;
    if(n < 0)
      return 0;
    memmove(client->output + client->output_len, buf, n);
    client->output_len += n;
  }
  return 0;
}

static void
flush_client_output(struct client *client)
{
  while(client->output_len > 0){
    int n = write(client->fd, client->output, client->output_len);
    if(n <= 0)
      return;
    client->output_len -= n;
    if(client->output_len > 0)
      memmove(client->output, client->output + n, client->output_len);
  }
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
        for(;;){
          int client_fd = socket_accept(server_fd);
          if(client_fd < 0)
            break;
          if(set_nonblocking(client_fd) < 0){
            close(client_fd);
            continue;
          }
          struct client *client = alloc_client(client_fd);
          if(client == 0){
            close(client_fd);
            continue;
          }
          ev.events = EPOLLIN;
          ev.data.fd = client_fd;
          if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0){
            close(client_fd);
            client->fd = 0;
            continue;
          }
          printf("New client connected fd=%d.\n", client_fd);
        }
      } else {
        struct client *client = find_client(fd);
        if(client == 0)
          continue;
        if(events[i].events & EPOLLERR){
          close_client(epoll_fd, client);
          printf("Client fd=%d disconnected.\n", fd);
          continue;
        }
        if(events[i].events & EPOLLIN){
          int input_status = drain_client_input(client);
          if(input_status > 0)
            client->read_closed = 1;
        }
        if(events[i].events & EPOLLOUT)
          flush_client_output(client);
        if(client->read_closed && client->output_len == 0){
          close_client(epoll_fd, client);
          printf("Client fd=%d disconnected.\n", fd);
          continue;
        }
        if(update_client_events(epoll_fd, client) < 0){
          close_client(epoll_fd, client);
          printf("Client fd=%d disconnected.\n", fd);
        }
      }
    }
  }
  close(epoll_fd);
  close(server_fd);
  exit(0);
}
