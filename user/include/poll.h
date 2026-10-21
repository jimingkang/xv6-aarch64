#ifndef _POLL_H
#define _POLL_H

#include <uapi/poll.h>

int poll(struct pollfd *, unsigned long, int);

#endif
