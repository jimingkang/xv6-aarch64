#ifndef _SYS_WAIT_H
#define _SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG 1

pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);

#define WIFEXITED(status) 1
#define WEXITSTATUS(status) (status)

#endif
