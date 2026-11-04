/* The NFSv3 server; see nfs.h. RFC 1813 (NFS v3, MOUNT v3), RFC 5531 (RPC, record
 * marking), RFC 4506 (XDR), RFC 1833 (portmapper v2). */

#include "nfs.h"

#define PROG_PORTMAP    100000u
#define PROG_NFS        100003u
#define PROG_MOUNT      100005u

#define IO_MAX          65536u      /* rtmax / wtmax */
#define BUF_SIZE        (IO_MAX + 4096u)
#define UDP_SIZE        8192u
#define CONN_MAX        16
#define HANDLES         2048u       /* paths a client may hold handles to */
#define FH_SIZE         8u

/* NFS3 status codes. */
#define NFS3_OK             0u
#define NFS3ERR_PERM        1u
#define NFS3ERR_NOENT       2u
#define NFS3ERR_IO          5u
#define NFS3ERR_ACCES       13u
#define NFS3ERR_EXIST       17u
#define NFS3ERR_NOTDIR      20u
#define NFS3ERR_ISDIR       21u
#define NFS3ERR_INVAL       22u
#define NFS3ERR_NOSPC       28u
#define NFS3ERR_NAMETOOLONG 63u
#define NFS3ERR_NOTEMPTY    66u
#define NFS3ERR_STALE       70u
#define NFS3ERR_BADHANDLE   10001u
#define NFS3ERR_NOTSUPP     10004u
#define NFS3ERR_SERVERFAULT 10006u

volatile NFS_STATS nfs_stats;

static NFS_CONFIG cfg;
static char root[SRV_PATH_MAX];

/* --- XDR ---------------------------------------------------------------------- */

typedef struct {
    const u8 *p;
    u32 len, at, bad;
} XIN;

typedef struct {
    u8 *p;
    u32 size, at, bad;
} XOUT;

static u32 get32(XIN *x)
{
    u32 v;

    if (x->at + 4 > x->len) {
        x->bad = 1;
        return 0;
    }
    v = (u32)x->p[x->at] << 24 | (u32)x->p[x->at + 1] << 16 | (u32)x->p[x->at + 2] << 8 |
        x->p[x->at + 3];
    x->at += 4;
    return v;
}

static void get64(XIN *x, u32 *hi, u32 *lo)
{
    *hi = get32(x);
    *lo = get32(x);
}

/* An opaque or string: its bytes stay in the input; *n its length. */
static const u8 *get_opaque(XIN *x, u32 *n, u32 max)
{
    const u8 *p;

    *n = get32(x);
    if (x->bad || *n > max || x->at + *n > x->len) {
        x->bad = 1;
        *n = 0;
        return 0;
    }
    p = x->p + x->at;
    x->at += (*n + 3) & ~3u;
    if (x->at > x->len)
        x->bad = 1;
    return p;
}

static void put32(XOUT *x, u32 v)
{
    if (x->at + 4 > x->size) {
        x->bad = 1;
        return;
    }
    x->p[x->at] = (u8)(v >> 24);
    x->p[x->at + 1] = (u8)(v >> 16);
    x->p[x->at + 2] = (u8)(v >> 8);
    x->p[x->at + 3] = (u8)v;
    x->at += 4;
}

static void put64(XOUT *x, u32 hi, u32 lo)
{
    put32(x, hi);
    put32(x, lo);
}

static void put_opaque(XOUT *x, const void *data, u32 n)
{
    u32 i;

    put32(x, n);
    if (x->at + ((n + 3) & ~3u) > x->size) {
        x->bad = 1;
        return;
    }
    for (i = 0; i < n; i++)
        x->p[x->at + i] = ((const u8 *)data)[i];
    for (; i & 3; i++)
        x->p[x->at + i] = 0;
    x->at += i;
}

/* --- file handles ------------------------------------------------------------- */

typedef struct {
    char path[SRV_PATH_MAX];
    u32 gen;
    u32 used;
} HANDLE;

static HANDLE *handles;
static u32 generation = 1;

static u32 fnv(const char *s)
{
    u32 h = 2166136261u;

    while (*s)
        h = (h ^ (u8)*s++) * 16777619u;
    return h;
}

static u32 str_len(const char *s)
{
    u32 n = 0;

    while (s[n])
        n++;
    return n;
}

static void str_copy(char *to, const char *from, u32 size)
{
    u32 i;

    for (i = 0; from[i] && i + 1 < size; i++)
        to[i] = from[i];
    to[i] = 0;
}

static u32 str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* The handle of `path` ("" the root), made if new. Index 0 is the root's. */
static void handle_of(const char *path, u8 *fh)
{
    u32 i, h, slot = 0, n;

    srv_lock();
    if (path[0]) {
        h = fnv(path) % (HANDLES - 1);
        slot = 1 + h;
        for (n = 0; n < 16; n++) {
            i = 1 + (h + n) % (HANDLES - 1);
            if (handles[i].used && str_eq(handles[i].path, path)) {
                slot = i;
                goto found;
            }
            if (!handles[i].used) {
                slot = i;
                break;
            }
        }
        if (!handles[slot].used)
            nfs_stats.handles++;
        str_copy(handles[slot].path, path, SRV_PATH_MAX);
        handles[slot].gen = generation++;
        handles[slot].used = 1;
    }
found:
    fh[0] = (u8)(slot >> 24);
    fh[1] = (u8)(slot >> 16);
    fh[2] = (u8)(slot >> 8);
    fh[3] = (u8)slot;
    n = slot ? handles[slot].gen : 0;
    fh[4] = (u8)(n >> 24);
    fh[5] = (u8)(n >> 16);
    fh[6] = (u8)(n >> 8);
    fh[7] = (u8)n;
    srv_unlock();
}

/* The path of a handle into `out`; an NFS3 status. */
static u32 path_of(const u8 *fh, u32 len, char *out)
{
    u32 slot, gen, status = NFS3_OK;

    if (len != FH_SIZE)
        return NFS3ERR_BADHANDLE;
    slot = (u32)fh[0] << 24 | (u32)fh[1] << 16 | (u32)fh[2] << 8 | fh[3];
    gen = (u32)fh[4] << 24 | (u32)fh[5] << 16 | (u32)fh[6] << 8 | fh[7];
    if (!slot) {
        out[0] = 0;
        return gen ? NFS3ERR_STALE : NFS3_OK;
    }
    srv_lock();
    if (slot >= HANDLES || !handles[slot].used || handles[slot].gen != gen)
        status = NFS3ERR_STALE;
    else
        str_copy(out, handles[slot].path, SRV_PATH_MAX);
    srv_unlock();
    if (status)
        nfs_stats.stale++;
    return status;
}

/* After a rename: every held path under `from` now lives under `to`. */
static void handles_renamed(const char *from, const char *to)
{
    u32 i, n = str_len(from), m = str_len(to), k;
    char tmp[SRV_PATH_MAX];

    srv_lock();
    for (i = 1; i < HANDLES; i++) {
        char *p = handles[i].path;

        if (!handles[i].used)
            continue;
        for (k = 0; k < n && p[k] == from[k]; k++)
            ;
        if (k < n || (p[n] && p[n] != '/'))
            continue;
        if (m + str_len(p + n) + 1 > SRV_PATH_MAX) {
            handles[i].used = 0;
            continue;
        }
        str_copy(tmp, to, SRV_PATH_MAX);
        str_copy(tmp + m, p + n, SRV_PATH_MAX - m);
        str_copy(p, tmp, SRV_PATH_MAX);
    }
    srv_unlock();
}

/* `dir` + "/" + `name` into `out`, with "." and ".." resolved; an NFS3 status. */
static u32 child(const char *dir, const u8 *name, u32 n, char *out)
{
    u32 d = str_len(dir), i;

    if (!n)
        return NFS3ERR_INVAL;
    if (n > SRV_NAME_MAX)
        return NFS3ERR_NAMETOOLONG;
    for (i = 0; i < n; i++)
        if (name[i] == '/' || !name[i])
            return NFS3ERR_INVAL;
    if (n == 1 && name[0] == '.') {
        str_copy(out, dir, SRV_PATH_MAX);
        return NFS3_OK;
    }
    if (n == 2 && name[0] == '.' && name[1] == '.') {
        str_copy(out, dir, SRV_PATH_MAX);
        for (i = str_len(out); i > 0 && out[i - 1] != '/'; i--)
            ;
        out[i ? i - 1 : 0] = 0;
        return NFS3_OK;
    }
    if (d + 1 + n + 1 > SRV_PATH_MAX)
        return NFS3ERR_NAMETOOLONG;
    str_copy(out, dir, SRV_PATH_MAX);
    if (d)
        out[d++] = '/';
    for (i = 0; i < n; i++)
        out[d + i] = (char)name[i];
    out[d + n] = 0;
    return NFS3_OK;
}

static u32 nfs_err(s32 e)
{
    switch (e) {
    case SRV_OK: return NFS3_OK;
    case SRV_ENOENT: return NFS3ERR_NOENT;
    case SRV_EACCES: return NFS3ERR_ACCES;
    case SRV_EEXIST: return NFS3ERR_EXIST;
    case SRV_ENOTDIR: return NFS3ERR_NOTDIR;
    case SRV_EISDIR: return NFS3ERR_ISDIR;
    case SRV_EINVAL: return NFS3ERR_INVAL;
    case SRV_ENOSPC: return NFS3ERR_NOSPC;
    case SRV_ENAMETOOLONG: return NFS3ERR_NAMETOOLONG;
    case SRV_ENOTEMPTY: return NFS3ERR_NOTEMPTY;
    default: return NFS3ERR_IO;
    }
}

/* --- attributes --------------------------------------------------------------- */

static void put_fattr(XOUT *x, const SRV_STAT *s, const char *path)
{
    put32(x, s->is_dir ? 2 : 1);                /* NF3DIR / NF3REG */
    put32(x, s->is_dir ? 0777 : 0666);
    put32(x, s->is_dir ? 2 : 1);                /* nlink */
    put32(x, 0);                                /* uid */
    put32(x, 0);                                /* gid */
    put64(x, s->size_hi, s->size_lo);
    put64(x, s->size_hi, s->size_lo);           /* used */
    put32(x, 0);                                /* rdev */
    put32(x, 0);
    put64(x, 0, 1);                             /* fsid */
    put64(x, 0, s->id ? s->id : (path[0] ? fnv(path) | 1u : 1u));
    put32(x, s->atime); put32(x, 0);
    put32(x, s->mtime); put32(x, 0);
    put32(x, s->ctime); put32(x, 0);
}

/* post_op_attr: the attributes when they can be read, else none. */
static void put_post(XOUT *x, const char *path)
{
    SRV_STAT s;

    if (srv_stat(root, path, &s) == SRV_OK) {
        put32(x, 1);
        put_fattr(x, &s, path);
    } else {
        put32(x, 0);
    }
}

/* wcc_data with no pre-operation attributes: the client then just refreshes. */
static void put_wcc(XOUT *x, const char *path)
{
    put32(x, 0);
    put_post(x, path);
}

/* sattr3: only the size is acted on (FATX has no owners or modes). */
static void get_sattr(XIN *x, u32 *set_size, u32 *size_hi, u32 *size_lo)
{
    u32 how;

    *set_size = 0;
    if (get32(x))
        get32(x);                               /* mode */
    if (get32(x))
        get32(x);                               /* uid */
    if (get32(x))
        get32(x);                               /* gid */
    if ((*set_size = get32(x)))
        get64(x, size_hi, size_lo);
    how = get32(x);                             /* atime */
    if (how == 2)
        get32(x), get32(x);
    how = get32(x);                             /* mtime */
    if (how == 2)
        get32(x), get32(x);
}

/* --- a connection's state ----------------------------------------------------- */

typedef struct {
    SRV_SOCK sock;
    u8 *in;
    u8 *out;
    SRV_FILE file;              /* the file last read or written, kept open */
    char file_path[SRV_PATH_MAX];
    u32 file_writable;
    volatile u32 busy;
} CONN;

static CONN conns[CONN_MAX];

static void close_file(CONN *c)
{
    if (c->file) {
        srv_fclose(c->file);
        c->file = 0;
    }
    c->file_path[0] = 0;
}

static s32 open_file(CONN *c, const char *path, u32 write)
{
    s32 e;

    if (c->file && str_eq(c->file_path, path) && (c->file_writable || !write))
        return SRV_OK;
    close_file(c);
    e = srv_open(root, path, SRV_READ | SRV_WRITE, &c->file);
    c->file_writable = e == SRV_OK;
    if (e != SRV_OK && !write)
        e = srv_open(root, path, SRV_READ, &c->file);
    if (e != SRV_OK) {
        c->file = 0;
        return e;
    }
    str_copy(c->file_path, path, SRV_PATH_MAX);
    return SRV_OK;
}

/* --- NFS v3 ------------------------------------------------------------------- */

static const u8 write_verf[8] = { 'x', 'e', 'd', 'r', 'n', 'f', 's', '3' };

static void nfs3(CONN *c, u32 proc, XIN *a, XOUT *r)
{
    char path[SRV_PATH_MAX], path2[SRV_PATH_MAX], dir2[SRV_PATH_MAX];
    const u8 *fh, *name;
    u32 n, m, status, hi, lo, count, i, cookie_hi, cookie_lo, set, shi, slo, dircount, max;
    u8 h[FH_SIZE];
    SRV_STAT st;
    s32 e;

    if (proc == 0)                              /* NULL */
        return;
    fh = get_opaque(a, &n, 64);
    status = a->bad ? NFS3ERR_BADHANDLE : path_of(fh, n, path);

    switch (proc) {
    case 1:                                     /* GETATTR */
        if (!status)
            status = nfs_err(srv_stat(root, path, &st));
        put32(r, status);
        if (!status)
            put_fattr(r, &st, path);
        return;
    case 2:                                     /* SETATTR */
        get_sattr(a, &set, &shi, &slo);
        if (!status && set) {
            if (c->file && str_eq(c->file_path, path))
                close_file(c);
            status = nfs_err(srv_truncate(root, path, slo, shi));
        }
        put32(r, status);
        put_wcc(r, path);
        return;
    case 3:                                     /* LOOKUP */
        name = get_opaque(a, &m, SRV_NAME_MAX + 1);
        if (!status)
            status = child(path, name, m, path2);
        if (!status)
            status = nfs_err(srv_stat(root, path2, &st));
        put32(r, status);
        if (status) {
            put_post(r, path);
            return;
        }
        handle_of(path2, h);
        put_opaque(r, h, FH_SIZE);
        put32(r, 1);
        put_fattr(r, &st, path2);
        put_post(r, path);
        return;
    case 4:                                     /* ACCESS: everything asked is granted */
        i = get32(a);
        put32(r, status);
        put_post(r, path);
        if (!status)
            put32(r, i);
        return;
    case 6:                                     /* READ */
        get64(a, &hi, &lo);
        count = get32(a);
        if (count > IO_MAX)
            count = IO_MAX;
        if (!status)
            status = nfs_err(open_file(c, path, 0));
        if (status) {
            put32(r, status);
            put_post(r, path);
            return;
        }
        {
            u32 at = r->at, got, eof;

            srv_stat(root, path, &st);
            put32(r, NFS3_OK);
            put32(r, 1);
            put_fattr(r, &st, path);
            if (r->at + 12 + count > r->size) {
                r->at = at;
                put32(r, NFS3ERR_IO);
                put32(r, 0);
                return;
            }
            e = srv_read(c->file, lo, hi, r->p + r->at + 12, count);
            if (e < 0) {
                r->at = at;
                put32(r, nfs_err(e));
                put_post(r, path);
                return;
            }
            got = (u32)e;
            eof = got < count ||
                  ((unsigned long long)hi << 32 | lo) + got >=
                  ((unsigned long long)st.size_hi << 32 | st.size_lo);
            put32(r, got);
            put32(r, eof);
            put32(r, got);
            for (i = got; i & 3; i++)
                r->p[r->at + i] = 0;
            r->at += i;
            nfs_stats.reads++;
            if ((nfs_stats.read_bytes_lo += got) < got)
                nfs_stats.read_bytes_hi++;
        }
        return;
    case 7:                                     /* WRITE */
        get64(a, &hi, &lo);
        count = get32(a);
        get32(a);                               /* stable_how */
        name = get_opaque(a, &m, IO_MAX);
        if (!status && a->bad)
            status = NFS3ERR_INVAL;
        if (!status)
            status = nfs_err(open_file(c, path, 1));
        if (!status && !c->file_writable)
            status = NFS3ERR_ACCES;
        if (!status) {
            e = srv_write(c->file, lo, hi, name, m < count ? m : count);
            status = e < 0 ? nfs_err(e) : NFS3_OK;
            if (e > 0) {
                count = (u32)e;
                nfs_stats.writes++;
                if ((nfs_stats.write_bytes_lo += count) < count)
                    nfs_stats.write_bytes_hi++;
            }
        }
        put32(r, status);
        put_wcc(r, path);
        if (!status) {
            put32(r, count);
            put32(r, 2);                        /* FILE_SYNC: written through */
            for (i = 0; i < 8; i++)
                r->p[r->at + i] = write_verf[i];
            r->at += 8;
        }
        return;
    case 8:                                     /* CREATE */
    case 9:                                     /* MKDIR */
        name = get_opaque(a, &m, SRV_NAME_MAX + 1);
        if (!status)
            status = child(path, name, m, path2);
        set = 0;
        if (proc == 8) {
            i = get32(a);                       /* createhow */
            if (i == 2)
                get32(a), get32(a);             /* the exclusive verifier */
            else
                get_sattr(a, &set, &shi, &slo);
            if (!status) {
                SRV_FILE f = 0;

                e = srv_open(root, path2, SRV_READ | SRV_WRITE | SRV_CREATE |
                             (i ? SRV_EXCL : 0) | (set && !shi && !slo ? SRV_TRUNC : 0), &f);
                if (e == SRV_EEXIST && i == 2)
                    e = SRV_OK;                 /* a retried exclusive create */
                status = nfs_err(e);
                if (f)
                    srv_fclose(f);
            }
        } else {
            get_sattr(a, &set, &shi, &slo);
            if (!status)
                status = nfs_err(srv_mkdir(root, path2));
        }
        put32(r, status);
        if (!status) {
            handle_of(path2, h);
            put32(r, 1);
            put_opaque(r, h, FH_SIZE);
            put_post(r, path2);
        }
        put_wcc(r, path);
        return;
    case 12:                                    /* REMOVE */
    case 13:                                    /* RMDIR */
        name = get_opaque(a, &m, SRV_NAME_MAX + 1);
        if (!status)
            status = child(path, name, m, path2);
        if (!status) {
            if (c->file && str_eq(c->file_path, path2))
                close_file(c);
            status = nfs_err(proc == 12 ? srv_unlink(root, path2) : srv_rmdir(root, path2));
        }
        put32(r, status);
        put_wcc(r, path);
        return;
    case 14:                                    /* RENAME */
        name = get_opaque(a, &m, SRV_NAME_MAX + 1);
        if (!status)
            status = child(path, name, m, path2);
        fh = get_opaque(a, &n, 64);
        i = a->bad ? NFS3ERR_BADHANDLE : path_of(fh, n, dir2);
        name = get_opaque(a, &m, SRV_NAME_MAX + 1);
        if (!status)
            status = i;
        if (!status) {
            char to[SRV_PATH_MAX];

            status = child(dir2, name, m, to);
            if (!status) {
                close_file(c);
                status = nfs_err(srv_rename(root, path2, to));
                if (!status)
                    handles_renamed(path2, to);
            }
        }
        put32(r, status);
        put_wcc(r, path);
        put_wcc(r, dir2);
        return;
    case 16:                                    /* READDIR */
    case 17:                                    /* READDIRPLUS */
        get64(a, &cookie_hi, &cookie_lo);
        get32(a), get32(a);                     /* cookieverf */
        dircount = get32(a);
        max = proc == 17 ? get32(a) : dircount;
        if (max > IO_MAX)
            max = IO_MAX;
        if (!status && (srv_stat(root, path, &st) != SRV_OK || !st.is_dir))
            status = NFS3ERR_NOTDIR;
        put32(r, status);
        put_post(r, path);
        if (status)
            return;
        put64(r, 0, 0);                         /* cookieverf */
        {
            u32 cookie = cookie_lo, limit = r->at + max - 64, eof = 0;
            char entry[SRV_NAME_MAX + 1];

            for (;;) {
                u32 before = r->at, was = cookie;

                e = srv_readdir(root, path, &cookie, entry, sizeof(entry), &st);
                if (e <= 0) {
                    eof = 1;
                    break;
                }
                if (child(path, (const u8 *)entry, str_len(entry), path2))
                    continue;
                put32(r, 1);
                put64(r, 0, st.id ? st.id : fnv(path2) | 1u);
                put_opaque(r, entry, str_len(entry));
                put64(r, 0, cookie);
                if (proc == 17) {
                    put32(r, 1);
                    put_fattr(r, &st, path2);
                    handle_of(path2, h);
                    put32(r, 1);
                    put_opaque(r, h, FH_SIZE);
                }
                if (r->bad || r->at > limit) {
                    r->at = before;
                    r->bad = 0;
                    cookie = was;
                    break;
                }
            }
            put32(r, 0);                        /* no more entries in this answer */
            put32(r, eof);
        }
        return;
    case 18:                                    /* FSSTAT */
        put32(r, status);
        put_post(r, path);
        if (!status) {
            u32 thi = 0, tlo = 0, fhi = 0, flo = 0;

            srv_volume(root, &tlo, &thi, &flo, &fhi);
            put64(r, thi, tlo);
            put64(r, fhi, flo);
            put64(r, fhi, flo);
            put64(r, 0, 1000000);
            put64(r, 0, 1000000);
            put64(r, 0, 1000000);
            put32(r, 0);
        }
        return;
    case 19:                                    /* FSINFO */
        put32(r, status);
        put_post(r, path);
        if (!status) {
            put32(r, IO_MAX); put32(r, IO_MAX); put32(r, 4096);
            put32(r, IO_MAX); put32(r, IO_MAX); put32(r, 4096);
            put32(r, IO_MAX);
            put64(r, 0xFFFFFFFFu, 0xFFFFFFFFu);
            put32(r, 1); put32(r, 0);           /* time_delta: one second */
            put32(r, 0x08u | 0x10u);            /* HOMOGENEOUS | CANSETTIME */
        }
        return;
    case 20:                                    /* PATHCONF */
        put32(r, status);
        put_post(r, path);
        if (!status) {
            put32(r, 1);
            put32(r, SRV_NAME_MAX);
            put32(r, 1);                        /* no_trunc */
            put32(r, 1);                        /* chown_restricted */
            put32(r, 1);                        /* case_insensitive: FATX */
            put32(r, 1);                        /* case_preserving */
        }
        return;
    case 21:                                    /* COMMIT: writes are already through */
        put32(r, status);
        put_wcc(r, path);
        if (!status) {
            for (i = 0; i < 8; i++)
                r->p[r->at + i] = write_verf[i];
            r->at += 8;
        }
        return;
    default:                                    /* READLINK, SYMLINK, MKNOD, LINK */
        put32(r, status ? status : NFS3ERR_NOTSUPP);
        put_post(r, path);
        return;
    }
}

/* --- MOUNT v3 and the portmapper ---------------------------------------------- */

static void mount3(u32 proc, XIN *a, XOUT *r)
{
    u8 h[FH_SIZE];
    u32 n;

    switch (proc) {
    case 1:                                     /* MNT: any path gives the root */
        get_opaque(a, &n, 1024);
        nfs_stats.mounts++;
        put32(r, 0);
        handle_of("", h);
        put_opaque(r, h, FH_SIZE);
        put32(r, 1);
        put32(r, 1);                            /* AUTH_SYS */
        return;
    case 2:                                     /* DUMP */
        put32(r, 0);
        return;
    case 5:                                     /* EXPORT: "/" to everyone */
        put32(r, 1);
        put_opaque(r, "/", 1);
        put32(r, 0);                            /* no groups */
        put32(r, 0);                            /* no more */
        return;
    default:                                    /* NULL, UMNT, UMNTALL */
        return;
    }
}

static void portmap(u32 proc, XIN *a, XOUT *r)
{
    u32 prog, vers;

    nfs_stats.portmap_calls++;
    if (proc == 3) {                            /* GETPORT */
        prog = get32(a);
        vers = get32(a);
        get32(a);                               /* the protocol: both are served */
        get32(a);
        put32(r, (prog == PROG_NFS && vers == 3) ||
                 (prog == PROG_MOUNT && vers >= 1 && vers <= 3) ? cfg.port : 0);
    } else if (proc == 4) {                     /* DUMP */
        static const u32 maps[][2] = { { PROG_NFS, 3 }, { PROG_MOUNT, 3 }, { PROG_MOUNT, 1 } };
        u32 i, p;

        for (i = 0; i < 3; i++)
            for (p = 6; p <= 17; p += 11) {
                put32(r, 1);
                put32(r, maps[i][0]);
                put32(r, maps[i][1]);
                put32(r, p);
                put32(r, cfg.port);
            }
        put32(r, 0);
    }
}

/* --- RPC ---------------------------------------------------------------------- */

/* One call in, its reply out; the reply's length, 0 for none. */
static u32 rpc(CONN *c, const u8 *in, u32 len, u8 *out, u32 size)
{
    XIN a = { in, len, 0, 0 };
    XOUT r = { out, size, 0, 0 };
    u32 xid, prog, vers, proc, n, accept_at;

    xid = get32(&a);
    if (get32(&a) != 0 || get32(&a) != 2)      /* a CALL of RPC version 2 */
        return 0;
    prog = get32(&a);
    vers = get32(&a);
    proc = get32(&a);
    get32(&a);
    get_opaque(&a, &n, 400);                    /* credentials, taken as given */
    get32(&a);
    get_opaque(&a, &n, 400);                    /* verifier */
    if (a.bad) {
        nfs_stats.bad_calls++;
        return 0;
    }
    nfs_stats.calls++;
    put32(&r, xid);
    put32(&r, 1);                               /* REPLY */
    put32(&r, 0);                               /* MSG_ACCEPTED */
    put32(&r, 0);                               /* verifier AUTH_NONE */
    put32(&r, 0);
    accept_at = r.at;
    put32(&r, 0);                               /* SUCCESS */
    if (prog == PROG_NFS && vers == 3 && proc <= 21) {
        nfs3(c, proc, &a, &r);
    } else if (prog == PROG_MOUNT && vers >= 1 && vers <= 3 && proc <= 5) {
        mount3(proc, &a, &r);
    } else if (prog == PROG_PORTMAP && vers == 2 && proc <= 4) {
        portmap(proc, &a, &r);
    } else if (prog == PROG_NFS || prog == PROG_MOUNT || prog == PROG_PORTMAP) {
        r.at = accept_at;
        if ((prog == PROG_NFS && vers == 3) || (prog == PROG_MOUNT && vers >= 1 && vers <= 3) ||
            (prog == PROG_PORTMAP && vers == 2)) {
            put32(&r, 3);                       /* PROC_UNAVAIL */
        } else {
            put32(&r, 2);                       /* PROG_MISMATCH */
            put32(&r, prog == PROG_MOUNT ? 1 : prog == PROG_NFS ? 3 : 2);
            put32(&r, prog == PROG_MOUNT ? 3 : prog == PROG_NFS ? 3 : 2);
        }
    } else {
        r.at = accept_at;
        put32(&r, 1);                           /* PROG_UNAVAIL */
    }
    if (a.bad && r.at == accept_at + 4) {
        r.at = accept_at;
        put32(&r, 4);                           /* GARBAGE_ARGS */
    }
    if (r.bad) {
        nfs_stats.bad_calls++;
        return 0;
    }
    return r.at;
}

/* --- transports --------------------------------------------------------------- */

static u32 allowed(u32 peer)
{
    return !cfg.allow_mask || (peer & cfg.allow_mask) == (cfg.allow & cfg.allow_mask);
}

static s32 recv_all(SRV_SOCK s, u8 *p, u32 n)
{
    u32 got = 0;
    s32 k;

    while (got < n) {
        k = srv_recv(s, p + got, n - got);
        if (k <= 0)
            return -1;
        got += (u32)k;
    }
    return 0;
}

/* One TCP connection: records (RFC 5531 record marking) in, a reply to each out. */
static void serve(void *arg)
{
    CONN *c = (CONN *)arg;
    u8 mark[4];
    u32 len, frag, last, n;

    for (;;) {
        len = 0;
        do {
            if (recv_all(c->sock, mark, 4) < 0)
                goto done;
            frag = (u32)(mark[0] & 0x7F) << 24 | (u32)mark[1] << 16 | (u32)mark[2] << 8 | mark[3];
            last = mark[0] & 0x80;
            if (len + frag > BUF_SIZE || recv_all(c->sock, c->in + len, frag) < 0)
                goto done;
            len += frag;
        } while (!last);
        n = rpc(c, c->in, len, c->out + 4, BUF_SIZE - 4);
        if (!n)
            continue;
        c->out[0] = (u8)(0x80 | n >> 24);
        c->out[1] = (u8)(n >> 16);
        c->out[2] = (u8)(n >> 8);
        c->out[3] = (u8)n;
        if (srv_send(c->sock, c->out, n + 4) < 0)
            goto done;
    }
done:
    close_file(c);
    srv_close(c->sock);
    srv_lock();
    nfs_stats.active--;
    c->busy = 0;
    srv_unlock();
}

static CONN *take_conn(void)
{
    u32 i;
    CONN *c = 0;

    srv_lock();
    for (i = 0; i < CONN_MAX; i++)
        if (!conns[i].busy) {
            c = &conns[i];
            c->busy = 1;
            break;
        }
    srv_unlock();
    if (c && !c->in) {
        c->in = (u8 *)srv_alloc(BUF_SIZE);
        c->out = (u8 *)srv_alloc(BUF_SIZE);
    }
    if (c && (!c->in || !c->out)) {
        c->busy = 0;
        return 0;
    }
    return c;
}

static void listener(void *arg)
{
    SRV_SOCK l = (SRV_SOCK)(long)arg, s;
    u32 peer;
    CONN *c;

    for (;;) {
        s = srv_accept(l, &peer);
        if (s < 0) {
            srv_sleep(100);
            continue;
        }
        if (!allowed(peer)) {
            nfs_stats.refused++;
            srv_close(s);
            continue;
        }
        c = take_conn();
        if (!c) {
            srv_close(s);
            continue;
        }
        c->sock = s;
        c->file = 0;
        c->file_path[0] = 0;
        srv_lock();
        nfs_stats.connections++;
        nfs_stats.active++;
        srv_unlock();
        if (srv_thread(serve, c)) {
            srv_close(s);
            c->busy = 0;
        }
    }
}

/* Datagrams: the portmapper and MOUNT as clients ask them over UDP. */
static void datagrams(void *arg)
{
    SRV_SOCK s = (SRV_SOCK)(long)arg;
    CONN udp_conn;                      /* one a thread: 2049 and 111 each have one */
    u8 *in = (u8 *)srv_alloc(UDP_SIZE), *out = (u8 *)srv_alloc(UDP_SIZE);
    u32 address, port, n;
    s32 got;

    udp_conn.file = 0;
    udp_conn.file_path[0] = 0;
    if (!in || !out)
        return;
    for (;;) {
        got = srv_recvfrom(s, in, UDP_SIZE, &address, &port);
        if (got <= 0)
            continue;
        if (!allowed(address)) {
            nfs_stats.refused++;
            continue;
        }
        n = rpc(&udp_conn, in, (u32)got, out, UDP_SIZE);
        close_file(&udp_conn);
        if (n)
            srv_sendto(s, out, n, address, port);
    }
}

s32 nfs_start(const NFS_CONFIG *config)
{
    SRV_SOCK s;

    if (nfs_stats.started)
        return 0;
    cfg = *config;
    if (!cfg.port)
        cfg.port = NFS_PORT_DEFAULT;
    if (!cfg.portmap_port)
        cfg.portmap_port = PORTMAP_PORT_DEFAULT;
    str_copy(root, cfg.root, SRV_PATH_MAX);
    cfg.root = root;
    if (!handles)
        handles = (HANDLE *)srv_alloc(HANDLES * sizeof(HANDLE));
    if (!handles)
        return -1;

    s = srv_listen(cfg.port, CONN_MAX);
    if (s < 0)
        return -1;                      /* the network not up yet: asked again later */
    if (srv_thread(listener, (void *)(long)s)) {
        srv_close(s);
        return -1;
    }
    s = srv_udp(cfg.port);
    if (s >= 0)
        srv_thread(datagrams, (void *)(long)s);
    if (cfg.portmap_port != 0xFFFFFFFFu) {
        s = srv_listen(cfg.portmap_port, 4);
        if (s >= 0)
            srv_thread(listener, (void *)(long)s);
        s = srv_udp(cfg.portmap_port);
        if (s >= 0)
            srv_thread(datagrams, (void *)(long)s);
    }
    nfs_stats.started = 1;
    return 0;
}
