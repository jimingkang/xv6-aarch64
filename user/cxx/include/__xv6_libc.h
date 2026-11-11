// The parts of xv6's C library that the C++ headers use, with C linkage.
#pragma once
#include <cstddef>
extern "C" {
void *malloc(unsigned int);
void free(void *);
void *realloc(void *, std::size_t);
void *calloc(std::size_t, std::size_t);
int exit(int) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));
int atoi(const char *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
void *memset(void *, int, unsigned int);
void *memmove(void *, const void *, int);
void *memcpy(void *, const void *, unsigned int);
int memcmp(const void *, const void *, unsigned int);
unsigned int strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, unsigned int);
char *strcpy(char *, const char *);
char *strchr(const char *, char);
int write(int, const void *, int);
int printf(const char *, ...);
int fprintf(int, const char *, ...);
int snprintf(char *, unsigned int, const char *, ...);
int vfprintf(int, const char *, __builtin_va_list);
int vsnprintf(char *, unsigned int, const char *, __builtin_va_list);
int vprintf(const char *, __builtin_va_list);
unsigned long clock_us(void);
int fmt_double(char *, int, double, int, int, int, int);
}
