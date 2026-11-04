/* Byte strings for the servers: what libc would give, which the plugin has not.
 * Inline, so a server takes only what it uses. */

#ifndef XE_SRV_STR_H
#define XE_SRV_STR_H

#include "kernel.h"

static inline u32 str_len(const char *s)
{
    u32 n = 0;

    while (s[n])
        n++;
    return n;
}

/* At most size - 1 bytes and a terminator. */
static inline void str_copy(char *to, const char *from, u32 size)
{
    u32 i;

    for (i = 0; from[i] && i + 1 < size; i++)
        to[i] = from[i];
    to[i] = 0;
}

static inline u32 str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* FNV-1a, for numbers that stand for a path. */
static inline u32 str_hash(const char *s)
{
    u32 h = 2166136261u;

    while (*s)
        h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static inline void mem_copy(void *to, const void *from, u32 n)
{
    u8 *t = (u8 *)to;
    const u8 *f = (const u8 *)from;

    while (n--)
        *t++ = *f++;
}

static inline void mem_zero(void *to, u32 n)
{
    u8 *t = (u8 *)to;

    while (n--)
        *t++ = 0;
}

#endif /* XE_SRV_STR_H */
