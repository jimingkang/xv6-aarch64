#ifndef _UNISTD_H
#define _UNISTD_H

#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

pid_t fork(void);
pid_t vfork(void);
void _exit(int) __attribute__((noreturn));
int execv(const char *, char *const []);
pid_t getpid(void);
pid_t getppid(void);
pid_t gettid(void);
pid_t wait(int *);
pid_t waitpid(pid_t, int *, int);
int pipe(int [2]);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
int close(int);
int dup(int);
int dup2(int, int);
int chdir(const char *);
char *getcwd(char *, size_t);
int access(const char *, int);
int rmdir(const char *);
int unlink(const char *);
int link(const char *, const char *);
off_t lseek(int, off_t, int);
unsigned int sleep(unsigned int);
int usleep(unsigned int);
int sched_yield(void);

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#endif
