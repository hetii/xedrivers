/* srv.h on POSIX, for the host tests only: the servers of src/servers/ built and
 * run on the PC, mounted with the PC's own clients. Nothing here goes into the
 * plugin. Threads, memory, the lock, time and settings. */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "srv.h"

/* --- threads ----------------------------------------------------------------- */

typedef struct {
    void (*fn)(void *);
    void *arg;
} START;

static void *trampoline(void *p)
{
    START s = *(START *)p;

    free(p);
    s.fn(s.arg);
    return 0;
}

s32 srv_thread(void (*fn)(void *arg), void *arg)
{
    pthread_t t;
    START *s = malloc(sizeof(*s));

    if (!s)
        return -1;
    s->fn = fn;
    s->arg = arg;
    if (pthread_create(&t, 0, trampoline, s)) {
        free(s);
        return -1;
    }
    pthread_detach(t);
    return 0;
}

void srv_sleep(u32 ms)
{
    usleep(ms * 1000u);
}

/* --- memory ------------------------------------------------------------------ */

void *srv_alloc(u32 bytes)
{
    return calloc(1, bytes);
}

void srv_free(void *p)
{
    free(p);
}

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void srv_lock(void) { pthread_mutex_lock(&lock); }
void srv_unlock(void) { pthread_mutex_unlock(&lock); }

/* --- time and settings -------------------------------------------------------- */

u32 srv_now(void)
{
    return (u32)time(0);
}

/* Settings come from the environment in the tests: XE_SRV_<KEY>. */
u32 srv_setting(const char *key, char *out, u32 size)
{
    char name[128];
    const char *v;

    snprintf(name, sizeof(name), "XE_SRV_%s", key);
    v = getenv(name);
    if (!v)
        return 0;
    snprintf(out, size, "%s", v);
    return 1;
}

u32 srv_setting_u32(const char *key, u32 fallback)
{
    char v[32];

    return srv_setting(key, v, sizeof(v)) ? (u32)strtoul(v, 0, 0) : fallback;
}
