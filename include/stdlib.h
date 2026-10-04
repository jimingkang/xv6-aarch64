#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

void *malloc(size_t);
void free(void *);
int atoi(const char *);
void exit(int) __attribute__((noreturn));

#endif
