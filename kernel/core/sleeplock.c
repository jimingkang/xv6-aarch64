// Sleeping locks

#include <kernel/types.h>
#include "aarch64.h"
#include <kernel/defs.h>
#include <kernel/param.h>
#include "memlayout.h"
#include <kernel/spinlock.h>
#include <kernel/proc.h>
#include <kernel/sleeplock.h>

void
initsleeplock(struct sleeplock *lk, char *name)
{
  mutex_init(&lk->mutex, name);
}

void
acquiresleep(struct sleeplock *lk)
{
  mutex_lock(&lk->mutex);
}

void
releasesleep(struct sleeplock *lk)
{
  mutex_unlock(&lk->mutex);
}

int
holdingsleep(struct sleeplock *lk)
{
  return mutex_is_owned(&lk->mutex);
}


