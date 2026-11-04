/* srv.h on POSIX, for the host tests only: the servers of src/servers/ built and
 * run on the PC, mounted with the PC's own clients. Nothing here goes into the
 * plugin. Files. */

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
