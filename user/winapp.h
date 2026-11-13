// Small helpers shared by the windowed programs (explorer, notepad, calc,
// cmd, taskmgr).  Header-only so each program stays one .c file.

#ifndef XV6_WINAPP_H
#define XV6_WINAPP_H

#include "user/win32.h"

static inline void
scopy(char *d, const char *s, int n)
{
  if(n <= 0)
    return;
  while(--n > 0 && *s)
    *d++ = *s++;
  *d = 0;
}

static inline void
itoa_(char *d, long v)
{
  char t[24];
  int n = 0, neg = v < 0;
  unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;
  do t[n++] = '0' + u % 10; while((u /= 10) && n < 22);
  if(neg)
    *d++ = '-';
  while(n)
    *d++ = t[--n];
  *d = 0;
}

static inline void
two(char *d, int v)
{
  d[0] = '0' + (v / 10) % 10;
  d[1] = '0' + v % 10;
}

static inline void
box(HDC dc, int x, int y, int w, int h, COLORREF c)
{
  RECT r = { x, y, x + w, y + h };
  FillRect(dc, &r, c);
}

// Start another windowed program: fork + exec, as the shell's
// ShellExecute.  The child is reaped by the caller's timer.
static inline int
spawn(const char *path, const char *arg)
{
  int pid = fork();
  if(pid == 0){
    char *argv[3] = { (char *)path, (char *)arg, 0 };
    for(int i = 3; i < 32; i++)
      close(i);
    exec((char *)path, argv);
    printf("cannot start %s\n", path);
    exit(127);
  }
  return pid;
}

// The standard top-level message loop.
static inline int
run_message_loop(void)
{
  MSG msg;
  while(GetMessage(&msg, 0, 0, 0))
    DispatchMessage(&msg);
  return 0;
}

#endif
