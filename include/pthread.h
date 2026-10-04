#ifndef _PTHREAD_H
#define _PTHREAD_H

#include <sys/types.h>

typedef int pthread_t;
typedef struct { int unused; } pthread_attr_t;
typedef int pthread_mutex_t;
typedef struct { int unused; } pthread_mutexattr_t;

#define PTHREAD_MUTEX_INITIALIZER 0

int pthread_create(pthread_t *, const pthread_attr_t *,
                   void *(*)(void *), void *);
int pthread_join(pthread_t, void **);
pthread_t pthread_self(void);
int pthread_equal(pthread_t, pthread_t);
void pthread_exit(void *) __attribute__((noreturn));
int pthread_mutex_init(pthread_mutex_t *, const pthread_mutexattr_t *);
int pthread_mutex_destroy(pthread_mutex_t *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);

#endif
