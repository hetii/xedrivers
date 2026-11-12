/* The few C library functions lwIP calls, and the 64-bit division gcc leaves to a
 * library on a 32-bit PowerPC. The plugin links against nothing. */
#include <stddef.h>

void *memcpy(void *to, const void *from, size_t n)
{
    unsigned char *d = (unsigned char *)to;
    const unsigned char *s = (const unsigned char *)from;

    if ((((unsigned long)d | (unsigned long)s) & 3u) == 0) {
        unsigned int *dw = (unsigned int *)d;
        const unsigned int *sw = (const unsigned int *)s;

        for (; n >= 16; n -= 16) {
            dw[0] = sw[0]; dw[1] = sw[1]; dw[2] = sw[2]; dw[3] = sw[3];
            dw += 4; sw += 4;
        }
        for (; n >= 4; n -= 4)
            *dw++ = *sw++;
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    }
    while (n--)
        *d++ = *s++;
    return to;
}

void *memmove(void *to, const void *from, size_t n)
{
    unsigned char *d = (unsigned char *)to;
    const unsigned char *s = (const unsigned char *)from;

    if (d <= s || d >= s + n)
        return memcpy(to, from, n);
    while (n--)
        d[n] = s[n];
    return to;
}

void *memset(void *to, int value, size_t n)
{
    unsigned char *d = (unsigned char *)to;

    if (((unsigned long)d & 3u) == 0) {
        unsigned int w = (unsigned char)value * 0x01010101u, *dw = (unsigned int *)d;

        for (; n >= 4; n -= 4)
            *dw++ = w;
        d = (unsigned char *)dw;
    }
    while (n--)
        *d++ = (unsigned char)value;
    return to;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;

    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++)
        ;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n && *a && *a == *b; n--, a++, b++)
        ;
    return n ? (unsigned char)*a - (unsigned char)*b : 0;
}

/* Shift and subtract: called a few times a tick (sys_now), never per packet. */
static unsigned long long divide(unsigned long long n, unsigned long long d,
                                 unsigned long long *rest)
{
    unsigned long long q = 0, r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= 1ull << i;
        }
    }
    if (rest)
        *rest = r;
    return q;
}

unsigned long long __udivdi3(unsigned long long n, unsigned long long d)
{
    return divide(n, d, 0);
}

unsigned long long __umoddi3(unsigned long long n, unsigned long long d)
{
    unsigned long long r;

    divide(n, d, &r);
    return r;
}

int atoi(const char *s)
{
    int n = 0, minus = 0;

    while (*s == ' ')
        s++;
    if (*s == '-' || *s == '+')
        minus = *s++ == '-';
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return minus ? -n : n;
}
