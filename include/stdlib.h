#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
int atoi(const char *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
int abs(int);
long labs(long);
void exit(int) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

#endif
