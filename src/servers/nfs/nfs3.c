/* NFS v3 on files and the file system; directories are in nfs3_dir.c. */

#include "nfsd.h"

/* === Dispatch and the open file ============================================== */

/* NFS v3: the procedure table, srv.h's errors as NFS3 statuses and the open
 * files. */

u8 nfs3_write_verf[8];

/* A verifier of its own for each start: a client that sees it change sends again
 * what it wrote UNSTABLE and had not committed. */
void nfs3_init(void)
{
    u32 t = srv_now(), i;

    for (i = 0; i < 4; i++) {
        nfs3_write_verf[i] = "xenf"[i];
        nfs3_write_verf[4 + i] = (u8)(t >> (24 - 8 * i));
    }
}

static const NFS3_PROC procs[NFS3_PROC_MAX + 1] = {
    0,                  /*  0 NULL */
    nfs3_getattr,       /*  1 */
    nfs3_setattr,       /*  2 */
    nfs3_lookup,        /*  3 */
    nfs3_access,        /*  4 */
    nfs3_notsupp,       /*  5 READLINK: FATX has no links */
    nfs3_read,          /*  6 */
    nfs3_write,         /*  7 */
    nfs3_create,        /*  8 */
    nfs3_mkdir,         /*  9 */
    nfs3_notsupp,       /* 10 SYMLINK */
    nfs3_notsupp,       /* 11 MKNOD */
    nfs3_remove,        /* 12 */
    nfs3_rmdir,         /* 13 */
    nfs3_rename,        /* 14 */
    nfs3_notsupp,       /* 15 LINK */
    nfs3_readdir,       /* 16 */
    nfs3_readdirplus,   /* 17 */
    nfs3_fsstat,        /* 18 */
    nfs3_fsinfo,        /* 19 */
    nfs3_pathconf,      /* 20 */
    nfs3_commit,        /* 21 */
};

/* Every procedure but NULL starts with a file handle: read here, once. */
void nfs3_call(CONN *c, u32 proc, XIN *a, XOUT *r)
{
    NFS3_CALL q;
    const u8 *fh;
    u32 n;

    if (proc == 0 || proc > NFS3_PROC_MAX)
        return;
    q.conn = c;
    q.a = a;
    q.r = r;
    fh = xdr_get_opaque(a, &n, 64);
    q.status = a->bad ? NFS3ERR_BADHANDLE : handle_path(fh, n, q.path);
    if (q.status)
        q.path[0] = 0;
    procs[proc](&q);
}

u32 nfs3_status(s32 e)
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

/* The open files, one table for every connection: with nconnect a file's writes
 * come over several, and FATX does not reliably delete a file another handle has
 * open (some went at the last close, some stayed: 2026-11-04), so a name is
 * closed everywhere before it is removed, renamed or truncated. A call holds an
 * entry while it uses it (users); one no call used for IDLE_MS is closed by the
 * sweeper, so the server keeps nothing open behind the console's back. */

#define FILES_MAX   (CONN_MAX * 2)
#define IDLE_MS     5000u

static NFS_FILE files[FILES_MAX];

static u32 under(const char *path, const char *prefix)
{
    u32 n = str_len(prefix), k;

    for (k = 0; k < n && path[k] == prefix[k]; k++)
        ;
    return k == n && (!path[n] || path[n] == '/');
}

NFS_FILE *nfs3_file(const char *path, u32 write, s32 *e)
{
    NFS_FILE *f = 0, *old = 0;
    SRV_FILE closing = 0;
    u32 i, oldest = 0xFFFFFFFFu;

    srv_lock();
    for (i = 0; i < FILES_MAX; i++) {
        NFS_FILE *x = &files[i];

        if (x->file && !x->doomed && str_eq(x->path, path) && (x->writable || !write)) {
            x->users++;
            x->used = srv_now();
            srv_unlock();
            *e = SRV_OK;
            return x;
        }
        if (!x->file && !x->users && !f)
            f = x;
        if (x->file && !x->users && x->used < oldest) {
            oldest = x->used;
            old = x;
        }
    }
    if (!f && old) {                        /* the table full: the least used goes */
        f = old;
        closing = f->file;
        f->file = 0;
    }
    if (f)
        f->users = 1;                       /* taken while it opens */
    srv_unlock();
    if (closing)
        srv_fclose(closing);
    if (!f) {
        *e = SRV_EIO;
        return 0;
    }
    f->doomed = 0;
    *e = srv_open(nfs_cfg.root, path, SRV_READ | SRV_WRITE, &f->file);
    f->writable = *e == SRV_OK;
    if (*e != SRV_OK && !write)
        *e = srv_open(nfs_cfg.root, path, SRV_READ, &f->file);
    if (*e == SRV_OK)
        *e = srv_stat(nfs_cfg.root, path, &f->st);
    if (*e != SRV_OK) {
        if (f->file)
            srv_fclose(f->file);
        f->file = 0;
        srv_lock();
        f->users = 0;
        srv_unlock();
        return 0;
    }
    str_copy(f->path, path, SRV_PATH_MAX);
    f->used = srv_now();
    nfs_stats.opens++;
    return f;
}

void nfs3_file_done(NFS_FILE *f)
{
    SRV_FILE closing = 0;

    srv_lock();
    if (!--f->users && f->doomed) {
        closing = f->file;
        f->file = 0;
        f->doomed = 0;
    }
    srv_unlock();
    if (closing)
        srv_fclose(closing);
}

/* Every entry for `path`, or under it, closed; waits (briefly) for calls using one. */
void nfs3_forget(const char *path)
{
    SRV_FILE closing[FILES_MAX];
    u32 i, n, busy, tries;

    for (tries = 0; tries < 200; tries++) {
        n = busy = 0;
        srv_lock();
        for (i = 0; i < FILES_MAX; i++) {
            NFS_FILE *x = &files[i];

            if (!x->file || !under(x->path, path))
                continue;
            if (x->users) {
                x->doomed = 1;
                busy++;
            } else {
                closing[n++] = x->file;
                x->file = 0;
            }
        }
        srv_unlock();
        for (i = 0; i < n; i++)
            srv_fclose(closing[i]);
        if (!busy)
            return;
        srv_sleep(10);
    }
}

/* Files no call used for IDLE_MS closed, once a second. */
static void sweeper(void *arg)
{
    SRV_FILE closing[FILES_MAX];
    u32 i, n, now;

    (void)arg;
    for (;;) {
        srv_sleep(1000);
        now = srv_now();
        n = 0;
        srv_lock();
        for (i = 0; i < FILES_MAX; i++) {
            NFS_FILE *x = &files[i];

            if (x->file && !x->users && now - x->used >= IDLE_MS / 1000) {
                closing[n++] = x->file;
                x->file = 0;
            }
        }
        srv_unlock();
        for (i = 0; i < n; i++)
            srv_fclose(closing[i]);
    }
}

/* Another server (or this one) about to remove, rename or truncate a name. */
static void keeper(const char *root, const char *path)
{
    if (str_eq(root, nfs_cfg.root))
        nfs3_forget(path);
}

s32 nfs3_files_start(void)
{
    srv_keeper(keeper);
    return srv_thread(sweeper, 0);
}

void nfs3_release(CONN *c)
{
    if (c->list.dir) {
        srv_closedir(c->list.dir);
        c->list.dir = 0;
    }
    c->list.path[0] = 0;
}

/* === Files =================================================================== */

/* NFS v3 on one file: GETATTR, SETATTR, LOOKUP, ACCESS, READ, WRITE, COMMIT. */

void nfs3_getattr(NFS3_CALL *q)
{
    SRV_STAT st;

    if (!q->status)
        q->status = nfs3_status(srv_stat(nfs_cfg.root, q->path, &st));
    xdr_put32(q->r, q->status);
    if (!q->status)
        attr_put(q->r, &st, q->path);
}

/* Only the size can be set: a truncate or an extension. */
void nfs3_setattr(NFS3_CALL *q)
{
    u32 set, hi = 0, lo = 0;

    attr_get_set(q->a, &set, &hi, &lo);
    if (!q->status && set) {
        nfs3_forget(q->path);
        q->status = nfs3_status(srv_truncate(nfs_cfg.root, q->path, lo, hi));
    }
    xdr_put32(q->r, q->status);
    attr_put_wcc(q->r, q->path);
}

void nfs3_lookup(NFS3_CALL *q)
{
    char path[SRV_PATH_MAX];
    const u8 *name;
    u8 fh[FH_SIZE];
    SRV_STAT st;
    u32 n;

    name = xdr_get_opaque(q->a, &n, SRV_NAME_MAX + 1);
    if (!q->status)
        q->status = handle_child(q->path, name, n, path);
    if (!q->status)
        q->status = nfs3_status(srv_stat(nfs_cfg.root, path, &st));
    if (!q->status)
        q->status = handle_of(path, fh);
    xdr_put32(q->r, q->status);
    if (q->status) {
        attr_put_post(q->r, q->path);
        return;
    }
    xdr_put_opaque(q->r, fh, FH_SIZE);
    xdr_put32(q->r, 1);
    attr_put(q->r, &st, path);
    attr_put_post(q->r, q->path);
}

/* Everything asked is granted: FATX has no permissions to check. */
void nfs3_access(NFS3_CALL *q)
{
    u32 asked = xdr_get32(q->a);

    xdr_put32(q->r, q->status);
    attr_put_post(q->r, q->path);
    if (!q->status)
        xdr_put32(q->r, asked);
}

/* The data is read straight into the reply, after the attributes and counts;
 * the attributes are those the open file was found with (and has been written to
 * since), not another call to the disk for every 64 KB. */
void nfs3_read(NFS3_CALL *q)
{
    XOUT *r = q->r;
    u32 hi, lo, count, at, got, eof, i;
    NFS_FILE *f = 0;
    SRV_STAT st;
    s32 e = SRV_OK;

    xdr_get64(q->a, &hi, &lo);
    count = xdr_get32(q->a);
    if (count > IO_MAX)
        count = IO_MAX;
    if (!q->status) {
        f = nfs3_file(q->path, 0, &e);
        q->status = nfs3_status(e);
    }
    if (q->status) {
        xdr_put32(r, q->status);
        attr_put_post(r, q->path);
        return;
    }
    srv_lock();
    st = f->st;
    srv_unlock();
    at = r->at;
    xdr_put32(r, NFS3_OK);
    xdr_put32(r, 1);
    attr_put(r, &st, q->path);
    if (r->at + 12 + count > r->size) {
        nfs3_file_done(f);
        r->at = at;
        xdr_put32(r, NFS3ERR_IO);
        xdr_put32(r, 0);
        return;
    }
    e = srv_read(f->file, lo, hi, r->p + r->at + 12, count);
    nfs3_file_done(f);
    if (e < 0) {
        r->at = at;
        xdr_put32(r, nfs3_status(e));
        attr_put_post(r, q->path);
        return;
    }
    got = (u32)e;
    eof = got < count ||
          ((u64)hi << 32 | lo) + got >= ((u64)st.size_hi << 32 | st.size_lo);
    xdr_put32(r, got);
    xdr_put32(r, eof);
    xdr_put32(r, got);                              /* the opaque's length */
    for (i = got; i & 3; i++)
        r->p[r->at + i] = 0;
    r->at += i;
    nfs_stats.reads++;
    if ((nfs_stats.read_bytes_lo += got) < got)
        nfs_stats.read_bytes_hi++;
}

/* UNSTABLE as the client asks it (COMMIT flushes later), else flushed before the
 * answer. The attributes after are the open file's, its size moved by the write. */
void nfs3_write(NFS3_CALL *q)
{
    u32 hi, lo, count, n, stable;
    NFS_FILE *f = 0;
    const u8 *data;
    SRV_STAT st;
    u64 end;
    s32 e = SRV_OK;

    xdr_get64(q->a, &hi, &lo);
    count = xdr_get32(q->a);
    stable = xdr_get32(q->a);
    data = xdr_get_opaque(q->a, &n, IO_MAX);
    if (!q->status && q->a->bad)
        q->status = NFS3ERR_INVAL;
    if (!q->status) {
        f = nfs3_file(q->path, 1, &e);
        q->status = nfs3_status(e);
    }
    if (!q->status && !f->writable) {
        nfs3_file_done(f);
        q->status = NFS3ERR_ACCES;
    }
    if (q->status) {
        xdr_put32(q->r, q->status);
        attr_put_wcc(q->r, q->path);
        return;
    }
    e = srv_write(f->file, lo, hi, data, n < count ? n : count);
    if (e >= 0 && stable && srv_flush(f->file) != SRV_OK)
        e = SRV_EIO;
    srv_lock();
    if (e > 0) {
        count = (u32)e;
        end = ((u64)hi << 32 | lo) + count;
        if (end > ((u64)f->st.size_hi << 32 | f->st.size_lo)) {
            f->st.size_lo = (u32)end;
            f->st.size_hi = (u32)(end >> 32);
        }
        f->st.mtime = f->st.ctime = srv_now();
        nfs_stats.writes++;
        if ((nfs_stats.write_bytes_lo += count) < count)
            nfs_stats.write_bytes_hi++;
    }
    st = f->st;
    srv_unlock();
    nfs3_file_done(f);
    q->status = e < 0 ? nfs3_status(e) : NFS3_OK;
    xdr_put32(q->r, q->status);
    if (q->status) {
        attr_put_wcc(q->r, q->path);
        return;
    }
    xdr_put32(q->r, 0);                             /* no pre-operation attributes */
    xdr_put32(q->r, 1);
    attr_put(q->r, &st, q->path);
    xdr_put32(q->r, count);
    xdr_put32(q->r, stable ? 2 : 0);                /* FILE_SYNC, or UNSTABLE */
    xdr_put_fixed(q->r, nfs3_write_verf, 8);
}

/* What was written UNSTABLE goes to the disk: one handle of a file flushes the
 * file, whichever connection wrote it. */
void nfs3_commit(NFS3_CALL *q)
{
    NFS_FILE *f;
    s32 e = SRV_OK;

    if (!q->status) {
        f = nfs3_file(q->path, 1, &e);
        if (f) {
            e = srv_flush(f->file);
            nfs3_file_done(f);
        }
        q->status = nfs3_status(e);
        nfs_stats.commits++;
    }
    xdr_put32(q->r, q->status);
    attr_put_wcc(q->r, q->path);
    if (!q->status)
        xdr_put_fixed(q->r, nfs3_write_verf, 8);
}

/* === The file system ========================================================= */

/* NFS v3 on the file system: FSSTAT, FSINFO, PATHCONF, and the answer to what
 * FATX has not (links, devices). */

#define FSF3_HOMOGENEOUS 0x08u
#define FSF3_CANSETTIME  0x10u

void nfs3_fsstat(NFS3_CALL *q)
{
    u32 thi = 0, tlo = 0, fhi = 0, flo = 0;

    xdr_put32(q->r, q->status);
    attr_put_post(q->r, q->path);
    if (q->status)
        return;
    srv_volume(nfs_cfg.root, &tlo, &thi, &flo, &fhi);
    xdr_put64(q->r, thi, tlo);                      /* bytes */
    xdr_put64(q->r, fhi, flo);                      /* free */
    xdr_put64(q->r, fhi, flo);                      /* free to this user */
    xdr_put64(q->r, 0, 1000000);                    /* files: FATX does not count */
    xdr_put64(q->r, 0, 1000000);
    xdr_put64(q->r, 0, 1000000);
    xdr_put32(q->r, 0);                             /* invarsec */
}

void nfs3_fsinfo(NFS3_CALL *q)
{
    xdr_put32(q->r, q->status);
    attr_put_post(q->r, q->path);
    if (q->status)
        return;
    xdr_put32(q->r, IO_MAX);                        /* rtmax, rtpref, rtmult */
    xdr_put32(q->r, IO_MAX);
    xdr_put32(q->r, 4096);
    xdr_put32(q->r, IO_MAX);                        /* wtmax, wtpref, wtmult */
    xdr_put32(q->r, IO_MAX);
    xdr_put32(q->r, 4096);
    xdr_put32(q->r, IO_MAX);                        /* dtpref */
    xdr_put64(q->r, 0xFFFFFFFFu, 0xFFFFFFFFu);      /* maxfilesize */
    xdr_put32(q->r, 1);                             /* time_delta: one second */
    xdr_put32(q->r, 0);
    xdr_put32(q->r, FSF3_HOMOGENEOUS | FSF3_CANSETTIME);
}

void nfs3_pathconf(NFS3_CALL *q)
{
    xdr_put32(q->r, q->status);
    attr_put_post(q->r, q->path);
    if (q->status)
        return;
    xdr_put32(q->r, 1);                             /* linkmax */
    xdr_put32(q->r, SRV_NAME_MAX);
    xdr_put32(q->r, 1);                             /* no_trunc */
    xdr_put32(q->r, 1);                             /* chown_restricted */
    xdr_put32(q->r, 1);                             /* case_insensitive: FATX */
    xdr_put32(q->r, 1);                             /* case_preserving */
}

void nfs3_notsupp(NFS3_CALL *q)
{
    xdr_put32(q->r, q->status ? q->status : NFS3ERR_NOTSUPP);
    attr_put_post(q->r, q->path);
}
