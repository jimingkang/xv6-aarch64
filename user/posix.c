#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/socket.h"
#include "user/user.h"
#include <pthread.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

#define POSIX_TICKS_PER_SECOND 10U
#define POSIX_THREAD_STACK (4U * 4096U)
#define SYNC_MUTEX 1
#define SYNC_SEM   2

struct pthread_record {
  struct pthread_record *next;
  int tid;
  volatile int ready;
  void *stack;
  void *(*start)(void *);
  void *argument;
  void *result;
};

static volatile int pthread_lock_word;
static struct pthread_record *pthread_records;
static int posix_errno;

static void
pthread_records_lock(void)
{
  for(;;){
    uint old, failed, one = 1;
    asm volatile("ldaxr %w0, [%1]"
                 : "=&r"(old) : "r"(&pthread_lock_word) : "memory");
    if(old == 0){
      asm volatile("stxr %w0, %w1, [%2]"
                   : "=&r"(failed) : "r"(one), "r"(&pthread_lock_word)
                   : "memory");
      if(failed == 0)
        return;
    } else {
      asm volatile("clrex" ::: "memory");
    }
    sched_yield();
  }
}

static void
pthread_records_unlock(void)
{
  uint zero = 0;
  asm volatile("stlr %w0, [%1]" ::
               "r"(zero), "r"(&pthread_lock_word) : "memory");
}

static int
posix_compare_exchange(volatile int *value, int expected, int desired)
{
  for(;;){
    uint old, failed;
    asm volatile("ldaxr %w0, [%1]"
                 : "=&r"(old) : "r"(value) : "memory");
    if((int)old != expected){
      asm volatile("clrex" ::: "memory");
      return old;
    }
    asm volatile("stxr %w0, %w1, [%2]"
                 : "=&r"(failed) : "r"(desired), "r"(value) : "memory");
    if(failed == 0)
      return expected;
  }
}

static int
posix_exchange(volatile int *value, int desired)
{
  for(;;){
    uint old, failed;
    asm volatile("ldaxr %w0, [%1]"
                 : "=&r"(old) : "r"(value) : "memory");
    asm volatile("stxr %w0, %w1, [%2]"
                 : "=&r"(failed) : "r"(desired), "r"(value) : "memory");
    if(failed == 0)
      return old;
  }
}

static struct pthread_record *
pthread_find(int tid)
{
  struct pthread_record *record;
  for(record = pthread_records; record; record = record->next)
    if(record->tid == tid)
      return record;
  return 0;
}

int *
__errno_location(void)
{
  // Detailed kernel error propagation and TLS are separate future steps.
  // This provides the standard libc entry point without changing syscall ABI.
  return &posix_errno;
}

void
_exit(int status)
{
  exit(status);
}

int
execv(const char *path, char *const arguments[])
{
  return exec((char *)path, (char **)arguments);
}

unsigned int
sleep(unsigned int seconds)
{
  while(seconds > 0){
    unsigned int chunk = seconds > 0x0fffffffU ? 0x0fffffffU : seconds;
    if(sleep_ticks(chunk * POSIX_TICKS_PER_SECOND) < 0)
      return seconds;
    seconds -= chunk;
  }
  return 0;
}

int
usleep(unsigned int microseconds)
{
  unsigned int ticks = (microseconds + 99999U) / 100000U;
  return ticks == 0 ? 0 : sleep_ticks(ticks);
}

int
clock_gettime(int clock_id, struct timespec *value)
{
  uint64 microseconds;
  if(value == 0 || clock_id != CLOCK_MONOTONIC){
    errno = clock_id == CLOCK_REALTIME ? ENOSYS : EINVAL;
    return -1;
  }
  microseconds = clock_us();
  value->tv_sec = microseconds / 1000000ULL;
  value->tv_nsec = (microseconds % 1000000ULL) * 1000ULL;
  return 0;
}

int
nanosleep(const struct timespec *request, struct timespec *remaining)
{
  uint64 microseconds;
  (void)remaining;
  if(request == 0 || request->tv_sec < 0 || request->tv_nsec < 0 ||
     request->tv_nsec >= 1000000000L){
    errno = EINVAL;
    return -1;
  }
  microseconds = (uint64)request->tv_sec * 1000000ULL +
                 (request->tv_nsec + 999) / 1000;
  while(microseconds > 0){
    uint chunk = microseconds > 0xffffffffULL ? 0xffffffffU : microseconds;
    if(usleep(chunk) < 0)
      return -1;
    microseconds -= chunk;
  }
  return 0;
}

static void
pthread_boot(void *opaque)
{
  struct pthread_record *record = opaque;
  while(!__atomic_load_n(&record->ready, __ATOMIC_ACQUIRE))
    sched_yield();
  record->result = record->start(record->argument);
  texit(0);
}

int
pthread_create(pthread_t *thread, const pthread_attr_t *attributes,
               void *(*start)(void *), void *argument)
{
  struct pthread_record *record;
  uint64 top;
  int tid;
  (void)attributes;

  if(thread == 0 || start == 0)
    return EINVAL;
  if((record = malloc(sizeof(*record))) == 0)
    return ENOMEM;
  memset(record, 0, sizeof(*record));
  if((record->stack = malloc(POSIX_THREAD_STACK + 16)) == 0){
    free(record);
    return ENOMEM;
  }
  record->start = start;
  record->argument = argument;
  pthread_records_lock();
  record->next = pthread_records;
  pthread_records = record;
  pthread_records_unlock();

  top = ((uint64)record->stack + POSIX_THREAD_STACK + 16) & ~15ULL;
  tid = clone(pthread_boot, record, (void *)top);
  if(tid < 0){
    pthread_records_lock();
    if(pthread_records == record)
      pthread_records = record->next;
    else {
      struct pthread_record *p;
      for(p = pthread_records; p && p->next != record; p = p->next)
        ;
      if(p)
        p->next = record->next;
    }
    pthread_records_unlock();
    free(record->stack);
    free(record);
    return EAGAIN;
  }
  record->tid = tid;
  *thread = tid;
  __atomic_store_n(&record->ready, 1, __ATOMIC_RELEASE);
  return 0;
}

int
pthread_join(pthread_t thread, void **result)
{
  struct pthread_record *record, *previous = 0;
  if(thread <= 0 || thread == gettid())
    return EINVAL;
  if(tjoin(thread, 0) < 0)
    return ESRCH;

  pthread_records_lock();
  for(record = pthread_records; record && record->tid != thread;
      previous = record, record = record->next)
    ;
  if(record){
    if(previous)
      previous->next = record->next;
    else
      pthread_records = record->next;
  }
  pthread_records_unlock();
  if(record == 0)
    return ESRCH;
  if(result)
    *result = record->result;
  free(record->stack);
  free(record);
  return 0;
}

pthread_t
pthread_self(void)
{
  return gettid();
}

int
pthread_equal(pthread_t first, pthread_t second)
{
  return first == second;
}

void
pthread_exit(void *result)
{
  struct pthread_record *record;
  pthread_records_lock();
  record = pthread_find(gettid());
  if(record)
    record->result = result;
  pthread_records_unlock();
  texit(0);
}

int
pthread_mutex_init(pthread_mutex_t *mutex,
                   const pthread_mutexattr_t *attributes)
{
  int handle;
  (void)attributes;
  if(mutex == 0 || (handle = sync_create(SYNC_MUTEX, 1)) < 0)
    return ENOMEM;
  *mutex = handle;
  return 0;
}

int
pthread_mutex_lock(pthread_mutex_t *mutex)
{
  int handle, old;
  if(mutex == 0)
    return EINVAL;
  handle = __atomic_load_n(mutex, __ATOMIC_ACQUIRE);
  if(handle == 0){
    handle = sync_create(SYNC_MUTEX, 1);
    if(handle < 0)
      return ENOMEM;
    old = posix_compare_exchange(mutex, 0, handle);
    if(old != 0){
      sync_destroy(handle);
      handle = old;
    }
  }
  return sync_wait(handle) < 0 ? EINVAL : 0;
}

int
pthread_mutex_unlock(pthread_mutex_t *mutex)
{
  if(mutex == 0 || *mutex == 0)
    return EINVAL;
  return sync_signal(*mutex, 1) < 0 ? EPERM : 0;
}

int
pthread_mutex_destroy(pthread_mutex_t *mutex)
{
  int handle;
  if(mutex == 0)
    return EINVAL;
  handle = posix_exchange(mutex, 0);
  return handle == 0 || sync_destroy(handle) == 0 ? 0 : EBUSY;
}

int
sem_init(sem_t *semaphore, int shared, unsigned int value)
{
  int handle;
  if(semaphore == 0 || shared != 0 ||
     (handle = sync_create(SYNC_SEM, value)) < 0)
    return -1;
  *semaphore = handle;
  return 0;
}

int sem_wait(sem_t *semaphore)
{
  return semaphore && sync_wait(*semaphore) == 0 ? 0 : -1;
}

int sem_post(sem_t *semaphore)
{
  return semaphore && sync_signal(*semaphore, 1) == 0 ? 0 : -1;
}

int sem_destroy(sem_t *semaphore)
{
  return semaphore && sync_destroy(*semaphore) == 0 ? 0 : -1;
}

int
epoll_create1(int flags)
{
  if(flags != 0){
    errno = EINVAL;
    return -1;
  }
  return epoll_create();
}

int
accept(int descriptor, struct sockaddr *address, unsigned int *length)
{
  int child = socket_accept(descriptor);
  if(child >= 0 && address && length){
    memset(address, 0, *length < sizeof(*address) ? *length : sizeof(*address));
    address->sa_family = AF_INET;
    *length = sizeof(*address);
  }
  return child;
}

uint16
htons(uint16 value)
{
  return (value << 8) | (value >> 8);
}

uint16 ntohs(uint16 value) { return htons(value); }

uint32
htonl(uint32 value)
{
  return ((value & 0xffU) << 24) | ((value & 0xff00U) << 8) |
         ((value >> 8) & 0xff00U) | ((value >> 24) & 0xffU);
}

uint32 ntohl(uint32 value) { return htonl(value); }

uint32
inet_addr(const char *text)
{
  uint32 address = 0;
  for(int part = 0; part < 4; part++){
    uint value = 0;
    if(text == 0 || *text < '0' || *text > '9')
      return 0xffffffffU;
    while(*text >= '0' && *text <= '9'){
      value = value * 10 + (*text++ - '0');
      if(value > 255)
        return 0xffffffffU;
    }
    address |= value << (part * 8);
    if(part != 3 && *text++ != '.')
      return 0xffffffffU;
  }
  return *text == 0 ? address : 0xffffffffU;
}

int putchar(int character)
{
  char c = character;
  return write(1, &c, 1) == 1 ? (unsigned char)c : -1;
}

int puts(const char *text)
{
  int length = strlen(text);
  if(write(1, text, length) != length || write(1, "\n", 1) != 1)
    return -1;
  return length + 1;
}
