/* The few C library functions lwIP calls, and the 64-bit division gcc leaves to a
 * library on a 32-bit PowerPC. The plugin links against nothing. */
#include <stddef.h>

/* Every byte lwIP moves goes through here, twice: into its buffers and out to the program.
 * A TCP payload sits 2 bytes off a word (14 bytes of Ethernet header), and a byte at a time
 * that cost more than the network did: FTP writes into the console stalled on a full window
 * 8 173 times in one run. So: 8 bytes at a time when both are 8-aligned (64-bit loads; the
 * processor is 64-bit, r9-r12 are free to clobber), words when both are word-aligned, and
 * when they are 2 apart, words read aligned from the source and joined. */
#if defined(__powerpc__)
#define BE32(p)     (*(p))
#define SET32(p, v) (*(p) = (v))
#else                                       /* the host test (little-endian) */
#define BE32(p)     __builtin_bswap32(*(p))
#define SET32(p, v) (*(p) = __builtin_bswap32(v))
#endif

void *memcpy(void *to, const void *from, size_t n)
{
    unsigned char *d = (unsigned char *)to;
    const unsigned char *s = (const unsigned char *)from;
    unsigned long apart = ((unsigned long)d ^ (unsigned long)s);

    if (n >= 32 && !(apart & 7)) {
        unsigned long blocks;

        while ((unsigned long)d & 7) {
            *d++ = *s++;
            n--;
        }
        blocks = n >> 5;
        if (blocks) {
            __asm__ volatile(".machine ppc64\n"
                             "    mtctr %2\n"
                             "1:  ld  9, 0(%1)\n"
                             "    ld  10, 8(%1)\n"
                             "    ld  11, 16(%1)\n"
                             "    ld  12, 24(%1)\n"
                             "    std 9, 0(%0)\n"
                             "    std 10, 8(%0)\n"
                             "    std 11, 16(%0)\n"
                             "    std 12, 24(%0)\n"
                             "    addi %0, %0, 32\n"
                             "    addi %1, %1, 32\n"
                             "    bdnz 1b\n"
                             ".machine any\n"
                             : "+b"(d), "+b"(s) : "r"(blocks)
                             : "r9", "r10", "r11", "r12", "ctr", "memory");
            n &= 31;
        }
    } else if (n >= 16 && !(apart & 3)) {
        unsigned int *dw;
        const unsigned int *sw;

        while ((unsigned long)d & 3) {
            *d++ = *s++;
            n--;
        }
        dw = (unsigned int *)d;
        sw = (const unsigned int *)s;
        for (; n >= 16; n -= 16) {
            unsigned int a = sw[0], b = sw[1], c = sw[2], e = sw[3];

            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            dw += 4; sw += 4;
        }
        for (; n >= 4; n -= 4)
            *dw++ = *sw++;
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw;
    } else if (n >= 16 && (apart & 3) == 2) {
        unsigned int *dw, prev;
        const unsigned int *sw;

        while ((unsigned long)d & 3) {
            *d++ = *s++;
            n--;
        }
        /* the source is now 2 past a word: read it a word earlier, join halves (big-endian) */
        sw = (const unsigned int *)(s - 2);
        dw = (unsigned int *)d;
        prev = BE32(sw);
        sw++;
        for (; n >= 10; n -= 8) {           /* 10: the last read takes 2 bytes past these 8 */
            unsigned int a = BE32(sw), b = BE32(sw + 1);

            SET32(dw, prev << 16 | a >> 16);
            SET32(dw + 1, a << 16 | b >> 16);
            prev = b;
            dw += 2; sw += 2;
        }
        d = (unsigned char *)dw;
        s = (const unsigned char *)sw - 2;
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
