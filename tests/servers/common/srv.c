/* srv.h on POSIX, for the host tests only: the servers of src/servers/ built and
 * run on the PC, mounted with the PC's own clients. Nothing here goes into the
 * plugin. */

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

static s32 err(void)
{
    switch (errno) {
    case ENOENT: return SRV_ENOENT;
    case EACCES: case EPERM: return SRV_EACCES;
    case EEXIST: return SRV_EEXIST;
    case ENOTDIR: return SRV_ENOTDIR;
    case EISDIR: return SRV_EISDIR;
    case EINVAL: return SRV_EINVAL;
    case ENOSPC: return SRV_ENOSPC;
    case ENAMETOOLONG: return SRV_ENAMETOOLONG;
    case ENOTEMPTY: return SRV_ENOTEMPTY;
    default: return SRV_EIO;
    }
}

/* --- sockets ----------------------------------------------------------------- */

SRV_SOCK srv_listen(u32 port, u32 backlog)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;

    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, (int)backlog) < 0) {
        close(s);
        return -1;
    }
    return s;
}

SRV_SOCK srv_udp(u32 port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1;

    if (s < 0)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

SRV_SOCK srv_accept(SRV_SOCK listener, u32 *peer)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    int s = accept(listener, (struct sockaddr *)&a, &n), one = 1;

    if (s < 0)
        return -1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (peer)
        *peer = ntohl(a.sin_addr.s_addr);
    return s;
}

SRV_SOCK srv_connect(u32 address, u32 port)
{
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0)
        return -1;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(address);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

s32 srv_recv(SRV_SOCK s, void *buffer, u32 length)
{
    ssize_t n = recv(s, buffer, length, 0);

    return n < 0 ? -1 : (s32)n;
}

s32 srv_send(SRV_SOCK s, const void *buffer, u32 length)
{
    const char *p = buffer;
    u32 done = 0;

    while (done < length) {
        ssize_t n = send(s, p + done, length - done, MSG_NOSIGNAL);

        if (n <= 0)
            return -1;
        done += (u32)n;
    }
    return (s32)done;
}

s32 srv_recvfrom(SRV_SOCK s, void *buffer, u32 length, u32 *address, u32 *port)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);
    ssize_t r = recvfrom(s, buffer, length, 0, (struct sockaddr *)&a, &n);

    if (r < 0)
        return -1;
    *address = ntohl(a.sin_addr.s_addr);
    *port = ntohs(a.sin_port);
    return (s32)r;
}

s32 srv_sendto(SRV_SOCK s, const void *buffer, u32 length, u32 address, u32 port)
{
    struct sockaddr_in a;

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u16)port);
    a.sin_addr.s_addr = htonl(address);
    return sendto(s, buffer, length, 0, (struct sockaddr *)&a, sizeof(a)) < 0 ? -1
                                                                             : (s32)length;
}

void srv_close(SRV_SOCK s)
{
    if (s >= 0)
        close(s);
}

u32 srv_local_address(SRV_SOCK s)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);

    return getsockname(s, (struct sockaddr *)&a, &n) < 0 ? 0 : ntohl(a.sin_addr.s_addr);
}

u32 srv_local_port(SRV_SOCK s)
{
    struct sockaddr_in a;
    socklen_t n = sizeof(a);

    return getsockname(s, (struct sockaddr *)&a, &n) < 0 ? 0 : ntohs(a.sin_port);
}

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

/* --- files ------------------------------------------------------------------- */

static void join(char *out, const char *root, const char *path)
{
    while (*path == '/')
        path++;
    snprintf(out, 4096, "%s/%s", root, path);
}

static void fill(const struct stat *s, SRV_STAT *out)
{
    out->is_dir = S_ISDIR(s->st_mode) != 0;
    out->size_lo = (u32)s->st_size;
    out->size_hi = (u32)((unsigned long long)s->st_size >> 32);
    out->mtime = (u32)s->st_mtime;
    out->atime = (u32)s->st_atime;
    out->ctime = (u32)s->st_ctime;
    out->id = (u32)s->st_ino;
}

s32 srv_open(const char *root, const char *path, u32 mode, SRV_FILE *out)
{
    char p[4096];
    int flags = (mode & SRV_WRITE) ? ((mode & SRV_READ) ? O_RDWR : O_WRONLY) : O_RDONLY, fd;

    if (mode & SRV_CREATE)
        flags |= O_CREAT;
    if (mode & SRV_EXCL)
        flags |= O_EXCL;
    if (mode & SRV_TRUNC)
        flags |= O_TRUNC;
    join(p, root, path);
    fd = open(p, flags, 0644);
    if (fd < 0)
        return err();
    *out = (SRV_FILE)(long)(fd + 1);
    return SRV_OK;
}

static int fd_of(SRV_FILE f) { return (int)(long)f - 1; }

s32 srv_read(SRV_FILE f, u32 offset_lo, u32 offset_hi, void *buffer, u32 length)
{
    ssize_t n = pread(fd_of(f), buffer, length,
                      (off_t)((unsigned long long)offset_hi << 32 | offset_lo));

    return n < 0 ? err() : (s32)n;
}

s32 srv_write(SRV_FILE f, u32 offset_lo, u32 offset_hi, const void *buffer, u32 length)
{
    ssize_t n = pwrite(fd_of(f), buffer, length,
                       (off_t)((unsigned long long)offset_hi << 32 | offset_lo));

    return n < 0 ? err() : (s32)n;
}

void srv_fclose(SRV_FILE f)
{
    if (f)
        close(fd_of(f));
}

s32 srv_stat(const char *root, const char *path, SRV_STAT *out)
{
    char p[4096];
    struct stat s;

    join(p, root, path);
    if (stat(p, &s) < 0)
        return err();
    fill(&s, out);
    return SRV_OK;
}

s32 srv_truncate(const char *root, const char *path, u32 size_lo, u32 size_hi)
{
    char p[4096];

    join(p, root, path);
    return truncate(p, (off_t)((unsigned long long)size_hi << 32 | size_lo)) < 0 ? err()
                                                                                 : SRV_OK;
}

s32 srv_mkdir(const char *root, const char *path)
{
    char p[4096];

    join(p, root, path);
    return mkdir(p, 0755) < 0 ? err() : SRV_OK;
}

s32 srv_rmdir(const char *root, const char *path)
{
    char p[4096];

    join(p, root, path);
    return rmdir(p) < 0 ? err() : SRV_OK;
}

s32 srv_unlink(const char *root, const char *path)
{
    char p[4096];

    join(p, root, path);
    return unlink(p) < 0 ? err() : SRV_OK;
}

s32 srv_rename(const char *root, const char *from, const char *to)
{
    char a[4096], b[4096];

    join(a, root, from);
    join(b, root, to);
    return rename(a, b) < 0 ? err() : SRV_OK;
}

s32 srv_readdir(const char *root, const char *path, u32 *cookie, char *name,
                u32 name_size, SRV_STAT *st)
{
    char p[4096], full[8192];
    DIR *d;
    struct dirent *e;
    struct stat s;
    u32 index = 0;

    join(p, root, path);
    d = opendir(p);
    if (!d)
        return err();
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (index++ < *cookie)
            continue;
        snprintf(name, name_size, "%s", e->d_name);
        snprintf(full, sizeof(full), "%s/%s", p, e->d_name);
        if (st && stat(full, &s) == 0)
            fill(&s, st);
        *cookie = index;
        closedir(d);
        return 1;
    }
    closedir(d);
    return 0;
}

s32 srv_volume(const char *root, u32 *total_lo, u32 *total_hi, u32 *free_lo, u32 *free_hi)
{
    struct statvfs v;
    unsigned long long t, f;

    if (statvfs(root, &v) < 0)
        return err();
    t = (unsigned long long)v.f_blocks * v.f_frsize;
    f = (unsigned long long)v.f_bavail * v.f_frsize;
    *total_lo = (u32)t;
    *total_hi = (u32)(t >> 32);
    *free_lo = (u32)f;
    *free_hi = (u32)(f >> 32);
    return SRV_OK;
}

/* --- memory ------------------------------------------------------------------ */

void *srv_alloc(u32 bytes)
{
    return calloc(1, bytes);
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
