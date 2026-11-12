/* lwIP's view of the compiler and the machine: a big-endian 32-bit PowerPC, gcc,
 * freestanding (no C library: port/libc.c has the few functions lwIP calls). */
#ifndef XE_LWIP_CC_H
#define XE_LWIP_CC_H

#define BYTE_ORDER              BIG_ENDIAN

#define LWIP_NO_INTTYPES_H      1
#define LWIP_NO_CTYPE_H         1
#define LWIP_NO_UNISTD_H        1
#define X8_F                    "02x"
#define U16_F                   "u"
#define S16_F                   "d"
#define X16_F                   "x"
#define U32_F                   "u"
#define S32_F                   "d"
#define X32_F                   "x"
#define SZT_F                   "u"

#define LWIP_PROVIDE_ERRNO      1

/* The cross compiler's limits.h has SSIZE_MAX, so lwIP expects unistd.h to have the
 * type; there is no unistd.h here. */
#include <limits.h>
#ifdef SSIZE_MAX
typedef int ssize_t;
#endif

/* Nothing is printed on the console: an assertion is counted and its text kept
 * (ipstack_stats, read with xectl). */
void ipstack_assert(const char *what);
#define LWIP_PLATFORM_ASSERT(x) ipstack_assert(x)
#define LWIP_PLATFORM_DIAG(x)   do { } while (0)

unsigned int ipstack_rand(void);
#define LWIP_RAND()             ipstack_rand()

#endif
