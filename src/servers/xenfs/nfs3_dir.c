/* NFS v3 on names in a directory: CREATE, MKDIR, REMOVE, RMDIR, RENAME, READDIR,
 * READDIRPLUS. */

#include "nfsd.h"

#define CREATE_GUARDED   1u
#define CREATE_EXCLUSIVE 2u

/* The reply both creates share: the new object's handle and attributes, then the
 * directory's wcc_data. */
static void put_created(NFS3_CALL *q, const char *path)
{
    u8 fh[FH_SIZE];

    xdr_put32(q->r, q->status);
    if (!q->status) {
        if (handle_of(path, fh) == NFS3_OK) {
            xdr_put32(q->r, 1);
            xdr_put_opaque(q->r, fh, FH_SIZE);
        } else {
            xdr_put32(q->r, 0);                     /* the client LOOKUPs it */
        }
        attr_put_post(q->r, path);
    }
    attr_put_wcc(q->r, q->path);
}

void nfs3_create(NFS3_CALL *q)
{
    char path[SRV_PATH_MAX];
    const u8 *name;
    u32 n, how, set = 0, hi = 0, lo = 0;
    SRV_FILE f = 0;
    s32 e;

    name = xdr_get_opaque(q->a, &n, SRV_NAME_MAX + 1);
    if (!q->status)
        q->status = handle_child(q->path, name, n, path);
    how = xdr_get32(q->a);
    if (how == CREATE_EXCLUSIVE)
        xdr_get32(q->a), xdr_get32(q->a);           /* the verifier */
    else
        attr_get_set(q->a, &set, &hi, &lo);
    if (!q->status) {
        if (set && !hi && !lo)
            nfs3_forget(path);                      /* truncated by the create */
        e = srv_open(nfs_cfg.root, path, SRV_READ | SRV_WRITE | SRV_CREATE |
                     (how ? SRV_EXCL : 0) | (set && !hi && !lo ? SRV_TRUNC : 0), &f);
        if (e == SRV_EEXIST && how == CREATE_EXCLUSIVE)
            e = SRV_OK;                             /* a retried exclusive create */
        q->status = nfs3_status(e);
        if (f)
            srv_fclose(f);
    }
    put_created(q, path);
}

void nfs3_mkdir(NFS3_CALL *q)
{
    char path[SRV_PATH_MAX];
    const u8 *name;
    u32 n, set, hi, lo;

    name = xdr_get_opaque(q->a, &n, SRV_NAME_MAX + 1);
    if (!q->status)
        q->status = handle_child(q->path, name, n, path);
    attr_get_set(q->a, &set, &hi, &lo);
    if (!q->status)
        q->status = nfs3_status(srv_mkdir(nfs_cfg.root, path));
    put_created(q, path);
}

static void remove_name(NFS3_CALL *q, u32 dir)
{
    char path[SRV_PATH_MAX];
    const u8 *name;
    u32 n;

    name = xdr_get_opaque(q->a, &n, SRV_NAME_MAX + 1);
    if (!q->status)
        q->status = handle_child(q->path, name, n, path);
    if (!q->status) {
        nfs3_forget(path);
        q->status = nfs3_status(dir ? srv_rmdir(nfs_cfg.root, path)
                                    : srv_unlink(nfs_cfg.root, path));
        if (!q->status)
            handle_removed(path);
    }
    xdr_put32(q->r, q->status);
    attr_put_wcc(q->r, q->path);
}

void nfs3_remove(NFS3_CALL *q)
{
    remove_name(q, 0);
}

void nfs3_rmdir(NFS3_CALL *q)
{
    remove_name(q, 1);
}

/* The second directory comes as a handle of its own; held handles follow. */
void nfs3_rename(NFS3_CALL *q)
{
    char from[SRV_PATH_MAX], to_dir[SRV_PATH_MAX], to[SRV_PATH_MAX];
    const u8 *name, *fh;
    u32 n, to_status;

    name = xdr_get_opaque(q->a, &n, SRV_NAME_MAX + 1);
    if (!q->status)
        q->status = handle_child(q->path, name, n, from);
    fh = xdr_get_opaque(q->a, &n, 64);
    to_status = q->a->bad ? NFS3ERR_BADHANDLE : handle_path(fh, n, to_dir);
    if (to_status)
        to_dir[0] = 0;
    name = xdr_get_opaque(q->a, &n, SRV_NAME_MAX + 1);
    if (!q->status)
        q->status = to_status;
    if (!q->status)
        q->status = handle_child(to_dir, name, n, to);
    if (!q->status) {
        nfs3_forget(from);
        nfs3_forget(to);
        q->status = nfs3_status(srv_rename(nfs_cfg.root, from, to));
        if (!q->status)
            handle_renamed(from, to);
    }
    xdr_put32(q->r, q->status);
    attr_put_wcc(q->r, q->path);
    attr_put_wcc(q->r, to_dir);
}

/* The listing's next entry: the one held back, or the directory's next. */
static s32 next_entry(NFS_LIST *l, char *name, SRV_STAT *st)
{
    if (l->held) {
        l->held = 0;
        str_copy(name, l->held_name, SRV_NAME_MAX + 1);
        *st = l->held_st;
        return 1;
    }
    return srv_readdir(l->dir, name, SRV_NAME_MAX + 1, st);
}

/* The listing at `cookie`: the open one when it is there already (a listing read
 * on, as clients read one), else the directory opened again and read up to it. */
static u32 seek_listing(CONN *c, const char *path, u32 cookie)
{
    NFS_LIST *l = &c->list;
    char name[SRV_NAME_MAX + 1];
    SRV_STAT st;

    if (l->dir && str_eq(l->path, path) && l->pos == cookie)
        return NFS3_OK;
    if (l->dir)
        srv_closedir(l->dir);
    l->dir = 0;
    l->held = 0;
    l->pos = 0;
    str_copy(l->path, path, SRV_PATH_MAX);
    if (srv_opendir(nfs_cfg.root, path, &l->dir) != SRV_OK) {
        l->dir = 0;
        return NFS3ERR_NOTDIR;
    }
    nfs_stats.listings++;
    while (l->pos < cookie && next_entry(l, name, &st) > 0)
        l->pos++;
    return NFS3_OK;
}

/* Entries from the cookie on, as many as fit in what the client allows; a cookie
 * is an entry's place in the listing. */
static void list(NFS3_CALL *q, u32 plus)
{
    char path[SRV_PATH_MAX], entry[SRV_NAME_MAX + 1];
    NFS_LIST *l = &q->conn->list;
    XOUT *r = q->r;
    u32 cookie_hi, cookie_lo, max, limit, eof = 0, before, hi, lo;
    u8 fh[FH_SIZE];
    SRV_STAT st;
    s32 e;

    xdr_get64(q->a, &cookie_hi, &cookie_lo);
    xdr_get32(q->a), xdr_get32(q->a);               /* cookieverf */
    max = xdr_get32(q->a);                          /* dircount, or count below */
    if (plus)
        max = xdr_get32(q->a);
    if (max > IO_MAX)
        max = IO_MAX;
    if (!q->status && (srv_stat(nfs_cfg.root, q->path, &st) != SRV_OK || !st.is_dir))
        q->status = NFS3ERR_NOTDIR;
    if (!q->status)
        q->status = seek_listing(q->conn, q->path, cookie_lo);
    xdr_put32(r, q->status);
    attr_put_post(r, q->path);
    if (q->status)
        return;
    xdr_put64(r, 0, 0);                             /* cookieverf */
    limit = r->at + max - 64;
    for (;;) {
        before = r->at;
        e = next_entry(l, entry, &st);
        if (e <= 0) {
            eof = 1;
            break;
        }
        if (handle_child(q->path, (const u8 *)entry, str_len(entry), path)) {
            l->pos++;                               /* a name not served: passed */
            continue;
        }
        attr_fileid(path, &hi, &lo);
        xdr_put32(r, 1);
        xdr_put64(r, hi, lo);
        xdr_put_string(r, entry);
        xdr_put64(r, 0, l->pos + 1);
        if (plus) {
            xdr_put32(r, 1);
            attr_put(r, &st, path);
            if (handle_of(path, fh) == NFS3_OK) {
                xdr_put32(r, 1);
                xdr_put_opaque(r, fh, FH_SIZE);
            } else {
                xdr_put32(r, 0);                    /* the client LOOKUPs it */
            }
        }
        if (r->bad || r->at > limit) {              /* did not fit: held for next */
            r->at = before;
            r->bad = 0;
            l->held = 1;
            str_copy(l->held_name, entry, sizeof(l->held_name));
            l->held_st = st;
            break;
        }
        l->pos++;
    }
    xdr_put32(r, 0);                                /* no more entries here */
    xdr_put32(r, eof);
}

void nfs3_readdir(NFS3_CALL *q)
{
    list(q, 0);
}

void nfs3_readdirplus(NFS3_CALL *q)
{
    list(q, 1);
}
