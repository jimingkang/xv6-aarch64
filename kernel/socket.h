#ifndef XV6_SOCKET_H
#define XV6_SOCKET_H

#define AF_INET      2
#define SOCK_STREAM  1
#define IPPROTO_TCP  6
#define INADDR_ANY   0U

struct in_addr {
  uint32 s_addr;
};

struct sockaddr {
  uint16 sa_family;
  char sa_data[14];
};

struct sockaddr_in {
  uint16 sin_family;
  uint16 sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

#endif
