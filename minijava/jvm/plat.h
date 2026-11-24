// Platform layer for mjvm: the handful of OS services a JVM needs.
// plat_xv6.c maps them onto xv6-aarch64 system calls; plat_host.c onto
// POSIX/pthreads so the same VM can be tested on a development machine.
#ifndef MJVM_PLAT_H
#define MJVM_PLAT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long               int64_t;
typedef unsigned long      uint64_t;
typedef unsigned long      size_t_;

void   *plat_chunk(long size);                    // zeroed memory from the OS
int     plat_read_file(const char *path, char **data, int *len);   // 0 ok
void    plat_write(const char *s, int n);         // stdout
void    plat_exit(int code) __attribute__((noreturn));

// threads: start fn(arg) on a new kernel thread sharing the address space
long    plat_thread_start(void (*fn)(void *), void *arg, char *stack, long stacksize);
void    plat_thread_join(long handle);
void    plat_thread_exit(void) __attribute__((noreturn));
void    plat_yield(void);
void    plat_sleep_ms(long ms);
long    plat_now_us(void);

// a counting semaphore = this VM's "ParkEvent"
int     plat_sem_create(void);
void    plat_sem_wait(int sem);
void    plat_sem_post(int sem);

// libc bits used by the VM
void   *memcpy(void *, const void *, unsigned long);
void   *memset(void *, int, unsigned long);
int     memcmp(const void *, const void *, unsigned long);
int     strcmp(const char *, const char *);
#endif
