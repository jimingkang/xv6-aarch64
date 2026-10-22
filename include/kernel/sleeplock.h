#ifndef XV6_SLEEPLOCK_H
#define XV6_SLEEPLOCK_H

#include <kernel/mutex.h>

// Compatibility name for the original xv6 sleep lock.  New code should use
// struct mutex directly; existing drivers retain the acquiresleep API.
struct sleeplock {
  struct mutex mutex;
};

#endif
