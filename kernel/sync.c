#include "types.h"
#include "param.h"
#include "aarch64.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// These values are part of the user/kernel ABI; keep them in sync with
// user/sync.h.
#define SYNC_MUTEX  1
#define SYNC_SEM    2
#define SYNC_EVENT  3
#define SYNC_ATOMIC 4

#define ATOMIC_LOAD      1
#define ATOMIC_STORE     2
#define ATOMIC_FETCH_ADD 3
#define ATOMIC_CAS       4

struct ksync {
  struct spinlock lock;
  int used;
  int generation;
  int type;
  int value;
  int waiters;
  int owner;
};

static struct {
  struct spinlock lock;
  struct ksync object[NSYNC];
} synctable;

void
syncinit(void)
{
  int i;
  initlock(&synctable.lock, "synctable");
  for(i = 0; i < NSYNC; i++){
    initlock(&synctable.object[i].lock, "sync");
    synctable.object[i].generation = 1;
  }
}

static int
makehandle(int index, int generation)
{
  return (generation << 8) | (index + 1);
}

// Return with the object locked, or null for an invalid/stale handle.
static struct ksync*
getobject(int handle)
{
  int index = (handle & 0xff) - 1;
  int generation = ((uint)handle) >> 8;
  struct ksync *o;

  if(index < 0 || index >= NSYNC)
    return 0;
  o = &synctable.object[index];
  acquire(&o->lock);
  if(!o->used || o->generation != generation){
    release(&o->lock);
    return 0;
  }
  return o;
}

int
ksync_create(int type, int initial)
{
  int i, handle = -1;
  struct ksync *o;

  if(type < SYNC_MUTEX || type > SYNC_ATOMIC || initial < 0)
    return -1;
  if(type == SYNC_MUTEX && initial > 1)
    return -1;
  if(type == SYNC_EVENT && initial > 1)
    return -1;

  acquire(&synctable.lock);
  for(i = 0; i < NSYNC; i++){
    o = &synctable.object[i];
    acquire(&o->lock);
    if(!o->used){
      o->used = 1;
      o->type = type;
      o->value = initial;
      o->waiters = 0;
      o->owner = 0;
      handle = makehandle(i, o->generation);
      release(&o->lock);
      break;
    }
    release(&o->lock);
  }
  release(&synctable.lock);
  return handle;
}

int
ksync_wait(int handle)
{
  struct ksync *o = getobject(handle);
  if(o == 0)
    return -1;
  if(o->type == SYNC_ATOMIC){
    release(&o->lock);
    return -1;
  }

  o->waiters++;
  while(o->value == 0){
    if(myproc()->killed){
      o->waiters--;
      release(&o->lock);
      return -1;
    }
    sleep(o, &o->lock);
    // Destruction is forbidden while waiters is non-zero, so o remains valid.
  }
  if(o->type != SYNC_EVENT)
    o->value--;
  if(o->type == SYNC_MUTEX)
    o->owner = myproc()->pid;
  o->waiters--;
  release(&o->lock);
  return 0;
}

int
ksync_signal(int handle, int count)
{
  struct ksync *o = getobject(handle);
  if(o == 0 || count <= 0){
    if(o)
      release(&o->lock);
    return -1;
  }

  if(o->type == SYNC_MUTEX){
    if(o->owner != myproc()->pid){
      release(&o->lock);
      return -1;
    }
    o->owner = 0;
    o->value = 1;
  } else if(o->type == SYNC_SEM){
    if(o->value > 0x7fffffff - count){
      release(&o->lock);
      return -1;
    }
    o->value += count;
  } else if(o->type == SYNC_EVENT){
    o->value = 1;              // manual-reset event: wake and stay signaled
  } else {
    release(&o->lock);
    return -1;
  }
  wakeup(o);
  release(&o->lock);
  return 0;
}

int
ksync_reset(int handle)
{
  struct ksync *o = getobject(handle);
  if(o == 0 || o->type != SYNC_EVENT){
    if(o)
      release(&o->lock);
    return -1;
  }
  o->value = 0;
  release(&o->lock);
  return 0;
}

int
ksync_destroy(int handle)
{
  struct ksync *o = getobject(handle);
  if(o == 0)
    return -1;
  if(o->waiters != 0){
    release(&o->lock);
    return -1;
  }
  o->used = 0;
  o->generation++;
  if(o->generation <= 0)
    o->generation = 1;
  release(&o->lock);
  return 0;
}

int
ksync_atomic(int handle, int op, int value, int compare)
{
  int old;
  struct ksync *o = getobject(handle);
  if(o == 0 || o->type != SYNC_ATOMIC){
    if(o)
      release(&o->lock);
    return -1;
  }

  old = o->value;
  switch(op){
  case ATOMIC_LOAD:
    break;
  case ATOMIC_STORE:
    o->value = value;
    break;
  case ATOMIC_FETCH_ADD:
    o->value += value;
    break;
  case ATOMIC_CAS:
    if(old == compare)
      o->value = value;
    break;
  default:
    release(&o->lock);
    return -1;
  }
  release(&o->lock);
  return old;
}
