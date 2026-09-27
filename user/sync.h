#ifndef XV6_USER_SYNC_H
#define XV6_USER_SYNC_H

#include "user/user.h"

enum {
  SYNC_MUTEX = 1,
  SYNC_SEM,
  SYNC_EVENT,
  SYNC_ATOMIC,
};

enum {
  ATOMIC_LOAD = 1,
  ATOMIC_STORE,
  ATOMIC_FETCH_ADD,
  ATOMIC_CAS,
};

typedef int mutex_t;
typedef int semaphore_t;
typedef int event_t;
typedef int katomic_t;

static inline int mutex_init(mutex_t *m) { return (*m = sync_create(SYNC_MUTEX, 1)) < 0 ? -1 : 0; }
static inline int mutex_lock(mutex_t *m) { return sync_wait(*m); }
static inline int mutex_unlock(mutex_t *m) { return sync_signal(*m, 1); }
static inline int mutex_destroy(mutex_t *m) { return sync_destroy(*m); }

static inline int sem_init(semaphore_t *s, int value) { return (*s = sync_create(SYNC_SEM, value)) < 0 ? -1 : 0; }
static inline int sem_wait(semaphore_t *s) { return sync_wait(*s); }
static inline int sem_post(semaphore_t *s) { return sync_signal(*s, 1); }
static inline int sem_post_n(semaphore_t *s, int n) { return sync_signal(*s, n); }
static inline int sem_destroy(semaphore_t *s) { return sync_destroy(*s); }

// Manual-reset event: notify wakes all present/future waiters until reset.
static inline int event_init(event_t *e, int signaled) { return (*e = sync_create(SYNC_EVENT, signaled)) < 0 ? -1 : 0; }
static inline int event_wait(event_t *e) { return sync_wait(*e); }
static inline int event_notify(event_t *e) { return sync_signal(*e, 1); }
static inline int event_signal(event_t *e) { return sync_signal(*e, 1); }
static inline int event_reset(event_t *e) { return sync_reset(*e); }
static inline int event_destroy(event_t *e) { return sync_destroy(*e); }

static inline int katomic_init(katomic_t *a, int value) { return (*a = sync_create(SYNC_ATOMIC, value)) < 0 ? -1 : 0; }
static inline int katomic_load(katomic_t *a) { return sync_atomic(*a, ATOMIC_LOAD, 0, 0); }
static inline int katomic_store(katomic_t *a, int value) { return sync_atomic(*a, ATOMIC_STORE, value, 0); }
static inline int katomic_fetch_add(katomic_t *a, int value) { return sync_atomic(*a, ATOMIC_FETCH_ADD, value, 0); }
static inline int katomic_compare_exchange(katomic_t *a, int expected, int value) { return sync_atomic(*a, ATOMIC_CAS, value, expected); }
static inline int katomic_destroy(katomic_t *a) { return sync_destroy(*a); }

// Local atomics use AArch64 exclusive instructions. They are useful for future
// threads, but after fork() the parent and child do not share this memory.
typedef volatile int atomic_int;
static inline int atomic_load(atomic_int *a) { return __atomic_load_n(a, __ATOMIC_SEQ_CST); }
static inline void atomic_store(atomic_int *a, int v) { __atomic_store_n(a, v, __ATOMIC_SEQ_CST); }
static inline int atomic_fetch_add(atomic_int *a, int v) { return __atomic_fetch_add(a, v, __ATOMIC_SEQ_CST); }
static inline int atomic_compare_exchange(atomic_int *a, int *expected, int desired)
{
  return __atomic_compare_exchange_n(a, expected, desired, 0,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

#endif
