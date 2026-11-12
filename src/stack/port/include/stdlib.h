/* No C library in the plugin: port/libc.c has what lwIP calls. */
#ifndef XE_STACK_STDLIB_H
#define XE_STACK_STDLIB_H
#include <stddef.h>
int atoi(const char *s);
#endif
