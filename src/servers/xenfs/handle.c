/* File handles and NFS3 attributes. */

#include "nfsd.h"

/* === File handles ============================================================ */

/* Handles: 8 bytes, a node's index and generation. The nodes are a tree of names
 * (a parent and a name each), so a path is as long as it likes, a rename moves one
 * node and the handles below it follow, and a full table gives up a leaf no
 * client used lately rather than whatever shared its slot. */

#define NODE_NAME   64u             /* FATX names have up to 42 bytes */
#define NONE        0xFFFFFFFFu
#define DEPTH_MAX   64u

typedef struct {
    u32 parent;                     /* a node's index; the root is 0 */
    u32 gen;                        /* 0: free */
    u32 next;                       /* the hash chain, NONE at its end */
    u16 children;                   /* nodes naming this one their parent */
    u8  ref;                        /* used since the clock last passed */
    u8  dead;                       /* removed while it had children */
    u8  len;
    char name[NODE_NAME];
} NODE;

static NODE *nodes;
static u32 *buckets;
static u32 count, hand, generation = 1;

static u32 bucket_of(u32 parent, const char *name, u32 len)
{
    u32 h = 2166136261u ^ parent, i;

    for (i = 0; i < len; i++)
        h = (h ^ (u8)name[i]) * 16777619u;
    return h % count;
}

s32 handle_init(u32 wanted)
{
    u32 i;

    if (nodes)
        return 0;
    count = wanted ? wanted : HANDLES_DEFAULT;
    if (count < 16)
        count = 16;
    /* The table is 80 bytes a name from the pool, 1.3 MB by default: with four cards'
     * drivers loaded the pool had no such piece and the server never started (2026-11-11).
     * So half as many, and half again, down to 256, before giving up. */
    for (;;) {
        nodes = (NODE *)srv_alloc(count * sizeof(NODE));
        buckets = (u32 *)srv_alloc(count * sizeof(u32));
        if (nodes && buckets)
            break;
        srv_free(nodes);
        srv_free(buckets);
        nodes = 0;
        if (count <= 256)
            return -1;
        count /= 2;
    }
    for (i = 0; i < count; i++)
        buckets[i] = NONE;
    nodes[0].gen = 1;                       /* the root: never free, never hashed */
    nodes[0].next = NONE;
    return 0;
}

static u32 find(u32 parent, const char *name, u32 len)
{
    u32 i = buckets[bucket_of(parent, name, len)];

    while (i != NONE) {
        NODE *n = &nodes[i];

        if (n->parent == parent && n->len == len && !n->dead) {
            u32 k;

            for (k = 0; k < len && n->name[k] == name[k]; k++)
                ;
            if (k == len)
                return i;
        }
        i = n->next;
    }
    return NONE;
}

static void unhash(u32 i)
{
    u32 *at = &buckets[bucket_of(nodes[i].parent, nodes[i].name, nodes[i].len)];

    while (*at != NONE && *at != i)
        at = &nodes[*at].next;
    if (*at == i)
        *at = nodes[i].next;
    nodes[i].next = NONE;
}

static void rehash(u32 i)
{
    u32 b = bucket_of(nodes[i].parent, nodes[i].name, nodes[i].len);

    nodes[i].next = buckets[b];
    buckets[b] = i;
}

/* A node no longer hashed and without children given back; a dead parent left
 * childless with it. */
static void release(u32 i)
{
    while (i) {
        u32 parent = nodes[i].parent;

        nodes[i].gen = 0;
        nodes[i].dead = 0;
        nfs_stats.handles--;
        if (!parent || --nodes[parent].children || !nodes[parent].dead)
            return;
        i = parent;
    }
}

/* Gone from its name: freed now, or once its last child is. */
static void kill(u32 i)
{
    unhash(i);
    if (nodes[i].children)
        nodes[i].dead = 1;
    else
        release(i);
}

/* A free node, or a leaf no one used for a lap of the clock; NONE if none. */
static u32 take(void)
{
    u32 steps;

    for (steps = 0; steps < 2 * count; steps++) {
        u32 i = hand;

        hand = hand + 1 < count ? hand + 1 : 1;
        if (!i)
            continue;
        if (!nodes[i].gen)
            return i;
        if (nodes[i].children || nodes[i].dead)
            continue;
        if (nodes[i].ref) {
            nodes[i].ref = 0;
            continue;
        }
        unhash(i);
        release(i);
        return i;
    }
    return NONE;
}

/* The node of a path's component under `parent`, made if `make`; NONE when there
 * is none (or no room, or the name is too long). */
static u32 child_node(u32 parent, const char *name, u32 len, u32 make)
{
    u32 i = find(parent, name, len);

    if (i != NONE || !make || len > NODE_NAME)
        return i;
    nodes[parent].children++;               /* held while a node is taken */
    i = take();
    if (i == NONE) {
        nodes[parent].children--;
        return NONE;
    }
    nodes[i].parent = parent;
    if (!generation)
        generation = 1;
    nodes[i].gen = generation++;
    nodes[i].children = 0;
    nodes[i].ref = 1;
    nodes[i].dead = 0;
    nodes[i].len = (u8)len;
    mem_copy(nodes[i].name, name, len);
    rehash(i);
    nfs_stats.handles++;
    return i;
}

/* The node of a whole path ("" the root). */
static u32 path_node(const char *path, u32 make)
{
    u32 cur = 0, len;

    while (*path) {
        for (len = 0; path[len] && path[len] != '/'; len++)
            ;
        cur = child_node(cur, path, len, make);
        if (cur == NONE)
            return NONE;
        nodes[cur].ref = 1;
        path += len;
        if (*path == '/')
            path++;
    }
    return cur;
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

u32 handle_of(const char *path, u8 *fh)
{
    u32 i;

    srv_lock();
    i = path_node(path, 1);
    put_be32(fh, i == NONE ? 0 : i);
    put_be32(fh + 4, i == NONE || !i ? 0 : nodes[i].gen);
    srv_unlock();
    if (i == NONE) {
        nfs_stats.handles_full++;
        return NFS3ERR_SERVERFAULT;
    }
    return NFS3_OK;
}

u32 handle_path(const u8 *fh, u32 len, char *out)
{
    u32 chain[DEPTH_MAX], depth = 0, i, gen, at = 0, k, status = NFS3_OK;

    if (len != FH_SIZE)
        return NFS3ERR_BADHANDLE;
    i = get_be32(fh);
    gen = get_be32(fh + 4);
    out[0] = 0;
    if (!i)
        return gen ? NFS3ERR_STALE : NFS3_OK;
    srv_lock();
    if (i >= count || !nodes[i].gen || nodes[i].gen != gen || nodes[i].dead) {
        status = NFS3ERR_STALE;
    } else {
        nodes[i].ref = 1;
        for (; i && depth < DEPTH_MAX; i = nodes[i].parent) {
            if (nodes[i].dead)
                break;
            chain[depth++] = i;
        }
        if (i)                              /* a dead ancestor, or too deep */
            status = NFS3ERR_STALE;
        while (!status && depth--) {
            NODE *n = &nodes[chain[depth]];

            if (at + (at ? 1 : 0) + n->len + 1 > SRV_PATH_MAX) {
                status = NFS3ERR_NAMETOOLONG;
                break;
            }
            if (at)
                out[at++] = '/';
            for (k = 0; k < n->len; k++)
                out[at++] = n->name[k];
            out[at] = 0;
        }
    }
    srv_unlock();
    if (status) {
        out[0] = 0;
        nfs_stats.stale++;
    }
    return status;
}

/* The last component of a path: its start, *len its length; *dir_len what is before. */
static const char *last_name(const char *path, u32 *len, u32 *dir_len)
{
    u32 n = str_len(path), i = n;

    while (i && path[i - 1] != '/')
        i--;
    *len = n - i;
    *dir_len = i ? i - 1 : 0;
    return path + i;
}

void handle_renamed(const char *from, const char *to)
{
    char dir[SRV_PATH_MAX];
    const char *name;
    u32 node, target, parent, len, dir_len;

    srv_lock();
    node = path_node(from, 0);
    if (node == NONE || !node)
        goto done;
    target = path_node(to, 0);
    if (target != NONE && target && target != node)
        kill(target);                       /* replaced: its handles go stale */
    name = last_name(to, &len, &dir_len);
    str_copy(dir, to, dir_len + 1 < SRV_PATH_MAX ? dir_len + 1 : SRV_PATH_MAX);
    parent = path_node(dir, 1);
    if (parent == NONE || len > NODE_NAME) {
        kill(node);
        goto done;
    }
    unhash(node);
    if (nodes[node].parent != parent) {
        u32 old = nodes[node].parent;

        nodes[parent].children++;
        nodes[node].parent = parent;
        if (old && !--nodes[old].children && nodes[old].dead)
            release(old);
    }
    nodes[node].len = (u8)len;
    mem_copy(nodes[node].name, name, len);
    rehash(node);
done:
    srv_unlock();
}

void handle_removed(const char *path)
{
    u32 node;

    srv_lock();
    node = path_node(path, 0);
    if (node != NONE && node)
        kill(node);
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

/* A path's node, its index and generation, is the file's number: unique while it
 * lives (no 32-bit hash to collide on a disk of millions of files) and the same
 * across a rename, as a client expects of a file whose handle it holds. */
void attr_fileid(const char *path, u32 *hi, u32 *lo)
{
    u32 i;

    srv_lock();
    i = path_node(path, 1);
    *hi = i == NONE ? 0xFFFFFFFFu : i;
    *lo = i == NONE ? str_hash(path) : i ? nodes[i].gen : 1;
    srv_unlock();
}

void attr_put(XOUT *x, const SRV_STAT *s, const char *path)
{
    u32 hi, lo;

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
    attr_fileid(path, &hi, &lo);
    xdr_put64(x, hi, lo);
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
