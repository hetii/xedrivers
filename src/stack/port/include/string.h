/* The plugin has no C library; port/libc.c has these for lwIP. */
#ifndef XE_STACK_STRING_H
#define XE_STACK_STRING_H
#include <stddef.h>
void *memcpy(void *to, const void *from, size_t n);
void *memmove(void *to, const void *from, size_t n);
void *memset(void *to, int value, size_t n);
int   memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, size_t n);
#endif
