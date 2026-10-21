#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

int printf(const char *, ...);
int fprintf(int, const char *, ...);
int vprintf(const char *, va_list);
int vfprintf(int, const char *, va_list);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int puts(const char *);
int putchar(int);
int getchar(void);

#define EOF (-1)

#endif
