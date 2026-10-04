#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <sys/types.h>
#include "kernel/types.h"
#include "kernel/socket.h"

int socket(int, int, int);
int bind(int, const struct sockaddr *, socklen_t);
int listen(int, int);
int accept(int, struct sockaddr *, socklen_t *);

#endif
