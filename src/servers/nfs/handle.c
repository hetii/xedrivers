/* File handles and NFS3 attributes. */

#include "nfsd.h"

/* === File handles ============================================================ */

/* Handles: 8 bytes, an index into a table of paths and a generation (nfsd.h). */

typedef struct {
    char path[SRV_PATH_MAX];
    u32 gen;
    u32 used;
} HANDLE;

static HANDLE *handles;
static u32 generation = 1;

s32 handle_init(void)
{
    if (!handles)
        handles = (HANDLE *)srv_alloc(HANDLES * sizeof(HANDLE));
    return handles ? 0 : -1;
}

static void put_be32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}

static u32 get_be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* An open-addressed table on the path's hash, 16 probes; when all are taken the
 * home slot is reused and the handles to its old path turn stale. */
void handle_of(const char *path, u8 *fh)
{
    u32 i, h, slot = 0, n;

    srv_lock();
    if (path[0]) {
        h = str_hash(path) % (HANDLES - 1);
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
    put_be32(fh, slot);
    put_be32(fh + 4, slot ? handles[slot].gen : 0);
    srv_unlock();
}

u32 handle_path(const u8 *fh, u32 len, char *out)
{
    u32 slot, gen, status = NFS3_OK;

    if (len != FH_SIZE)
        return NFS3ERR_BADHANDLE;
    slot = get_be32(fh);
    gen = get_be32(fh + 4);
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

void handle_renamed(const char *from, const char *to)
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

u32 handle_child(const char *dir, const u8 *name, u32 n, char *out)
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
    mem_copy(out + d, name, n);
    out[d + n] = 0;
    return NFS3_OK;
}

/* === Attributes ============================================================== */

u32 attr_fileid(const SRV_STAT *s, const char *path)
{
    return s->id ? s->id : (path[0] ? str_hash(path) | 1u : 1u);
}

void attr_put(XOUT *x, const SRV_STAT *s, const char *path)
{
    xdr_put32(x, s->is_dir ? 2 : 1);                /* NF3DIR / NF3REG */
    xdr_put32(x, s->is_dir ? 0777 : 0666);
    xdr_put32(x, s->is_dir ? 2 : 1);                /* nlink */
    xdr_put32(x, 0);                                /* uid */
    xdr_put32(x, 0);                                /* gid */
    xdr_put64(x, s->size_hi, s->size_lo);
    xdr_put64(x, s->size_hi, s->size_lo);           /* used */
    xdr_put32(x, 0);                                /* rdev */
    xdr_put32(x, 0);
    xdr_put64(x, 0, 1);                             /* fsid */
    xdr_put64(x, 0, attr_fileid(s, path));
    xdr_put32(x, s->atime);
    xdr_put32(x, 0);
    xdr_put32(x, s->mtime);
    xdr_put32(x, 0);
    xdr_put32(x, s->ctime);
    xdr_put32(x, 0);
}

void attr_put_post(XOUT *x, const char *path)
{
    SRV_STAT s;

    if (srv_stat(nfs_cfg.root, path, &s) == SRV_OK) {
        xdr_put32(x, 1);
        attr_put(x, &s, path);
    } else {
        xdr_put32(x, 0);
    }
}

void attr_put_wcc(XOUT *x, const char *path)
{
    xdr_put32(x, 0);
    attr_put_post(x, path);
}

void attr_get_set(XIN *x, u32 *set_size, u32 *size_hi, u32 *size_lo)
{
    u32 how;

    if (xdr_get32(x))
        xdr_get32(x);                               /* mode */
    if (xdr_get32(x))
        xdr_get32(x);                               /* uid */
    if (xdr_get32(x))
        xdr_get32(x);                               /* gid */
    if ((*set_size = xdr_get32(x)))
        xdr_get64(x, size_hi, size_lo);
    how = xdr_get32(x);                             /* atime: SET_TO_CLIENT_TIME */
    if (how == 2)
        xdr_get32(x), xdr_get32(x);
    how = xdr_get32(x);                             /* mtime */
    if (how == 2)
        xdr_get32(x), xdr_get32(x);
}
