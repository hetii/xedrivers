/* NFS v3 on files and the file system; directories are in nfs3_dir.c. */

#include "nfsd.h"

/* === Dispatch and the open file ============================================== */

/* NFS v3: the procedure table, srv.h's errors as NFS3 statuses and the file a
 * connection keeps open. */

const u8 nfs3_write_verf[8] = { 'x', 'e', 'd', 'r', 'n', 'f', 's', '3' };

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

void nfs3_close(NFS_OPEN *o)
{
    if (o->file) {
        srv_fclose(o->file);
        o->file = 0;
    }
    o->path[0] = 0;
}

/* Read-write when it can be (a later WRITE then reuses it), read-only otherwise. */
s32 nfs3_open(CONN *c, const char *path, u32 write)
{
    NFS_OPEN *o = &c->open;
    s32 e;

    if (o->file && str_eq(o->path, path) && (o->writable || !write))
        return SRV_OK;
    nfs3_close(o);
    e = srv_open(nfs_cfg.root, path, SRV_READ | SRV_WRITE, &o->file);
    o->writable = e == SRV_OK;
    if (e != SRV_OK && !write)
        e = srv_open(nfs_cfg.root, path, SRV_READ, &o->file);
    if (e != SRV_OK) {
        o->file = 0;
        return e;
    }
    str_copy(o->path, path, SRV_PATH_MAX);
    return SRV_OK;
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
        if (q->conn->open.file && str_eq(q->conn->open.path, q->path))
            nfs3_close(&q->conn->open);
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
    xdr_put32(q->r, q->status);
    if (q->status) {
        attr_put_post(q->r, q->path);
        return;
    }
    handle_of(path, fh);
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

/* The data is read straight into the reply, after the attributes and counts. */
void nfs3_read(NFS3_CALL *q)
{
    XOUT *r = q->r;
    u32 hi, lo, count, at, got, eof, i;
    SRV_STAT st;
    s32 e;

    xdr_get64(q->a, &hi, &lo);
    count = xdr_get32(q->a);
    if (count > IO_MAX)
        count = IO_MAX;
    if (!q->status)
        q->status = nfs3_status(nfs3_open(q->conn, q->path, 0));
    if (q->status) {
        xdr_put32(r, q->status);
        attr_put_post(r, q->path);
        return;
    }
    at = r->at;
    srv_stat(nfs_cfg.root, q->path, &st);
    xdr_put32(r, NFS3_OK);
    xdr_put32(r, 1);
    attr_put(r, &st, q->path);
    if (r->at + 12 + count > r->size) {
        r->at = at;
        xdr_put32(r, NFS3ERR_IO);
        xdr_put32(r, 0);
        return;
    }
    e = srv_read(q->conn->open.file, lo, hi, r->p + r->at + 12, count);
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

/* Written through: the reply says FILE_SYNC, so the client needs no COMMIT. */
void nfs3_write(NFS3_CALL *q)
{
    u32 hi, lo, count, n;
    const u8 *data;
    s32 e;

    xdr_get64(q->a, &hi, &lo);
    count = xdr_get32(q->a);
    xdr_get32(q->a);                                /* stable_how */
    data = xdr_get_opaque(q->a, &n, IO_MAX);
    if (!q->status && q->a->bad)
        q->status = NFS3ERR_INVAL;
    if (!q->status)
        q->status = nfs3_status(nfs3_open(q->conn, q->path, 1));
    if (!q->status && !q->conn->open.writable)
        q->status = NFS3ERR_ACCES;
    if (!q->status) {
        e = srv_write(q->conn->open.file, lo, hi, data, n < count ? n : count);
        q->status = e < 0 ? nfs3_status(e) : NFS3_OK;
        if (e > 0) {
            count = (u32)e;
            nfs_stats.writes++;
            if ((nfs_stats.write_bytes_lo += count) < count)
                nfs_stats.write_bytes_hi++;
        }
    }
    xdr_put32(q->r, q->status);
    attr_put_wcc(q->r, q->path);
    if (!q->status) {
        xdr_put32(q->r, count);
        xdr_put32(q->r, 2);                         /* FILE_SYNC */
        xdr_put_fixed(q->r, nfs3_write_verf, 8);
    }
}

/* Writes are already through: nothing to do but answer. */
void nfs3_commit(NFS3_CALL *q)
{
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
