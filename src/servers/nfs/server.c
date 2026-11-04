/* The server: its start, the sockets, RPC and XDR; see nfsd.h. RFC 1813 (NFS v3,
 * MOUNT v3), RFC 5531 (RPC, record marking), RFC 4506 (XDR). */

#include "nfsd.h"

/* === Start =================================================================== */

volatile NFS_STATS nfs_stats;
NFS_CONFIG nfs_cfg;

static char root[SRV_PATH_MAX];

s32 nfs_start(const NFS_CONFIG *config)
{
    if (nfs_stats.started)
        return 0;
    nfs_cfg = *config;
    if (!nfs_cfg.port)
        nfs_cfg.port = NFS_PORT_DEFAULT;
    if (!nfs_cfg.portmap_port)
        nfs_cfg.portmap_port = PORTMAP_PORT_DEFAULT;
    str_copy(root, config->root, SRV_PATH_MAX);
    nfs_cfg.root = root;
    if (handle_init())
        return -1;
    if (transport_start(nfs_cfg.port, CONN_MAX))
        return -1;                      /* the network not up yet: asked again later */
    if (nfs_cfg.portmap_port != 0xFFFFFFFFu)
        transport_start(nfs_cfg.portmap_port, 4);
    nfs_stats.started = 1;
    return 0;
}

/* === Transport =============================================================== */

/* TCP carries records (RFC 5531 section 11): a
 * 4-byte mark, its top bit "last fragment", before each fragment. */

#define BUF_SIZE    (IO_MAX + 4096u)    /* a WRITE's data and its header */
#define UDP_SIZE    8192u

static CONN conns[CONN_MAX];

static u32 allowed(u32 peer)
{
    return !nfs_cfg.allow_mask ||
           (peer & nfs_cfg.allow_mask) == (nfs_cfg.allow & nfs_cfg.allow_mask);
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

/* One record, its fragments joined; its length, or < 0 at the end. */
static s32 recv_record(CONN *c)
{
    u8 mark[4];
    u32 len = 0, frag, last;

    do {
        if (recv_all(c->sock, mark, 4) < 0)
            return -1;
        frag = (u32)(mark[0] & 0x7F) << 24 | (u32)mark[1] << 16 | (u32)mark[2] << 8 | mark[3];
        last = mark[0] & 0x80;
        if (len + frag > BUF_SIZE || recv_all(c->sock, c->in + len, frag) < 0)
            return -1;
        len += frag;
    } while (!last);
    return (s32)len;
}

/* One TCP connection, on a thread of its own: a reply to each record. */
static void serve(void *arg)
{
    CONN *c = (CONN *)arg;
    s32 len;
    u32 n;

    while ((len = recv_record(c)) >= 0) {
        n = rpc_call(c, c->in, (u32)len, c->out + 4, BUF_SIZE - 4);
        if (!n)
            continue;
        c->out[0] = (u8)(0x80 | n >> 24);
        c->out[1] = (u8)(n >> 16);
        c->out[2] = (u8)(n >> 8);
        c->out[3] = (u8)n;
        if (srv_send(c->sock, c->out, n + 4) < 0)
            break;
    }
    nfs3_close(&c->open);
    srv_close(c->sock);
    srv_lock();
    nfs_stats.active--;
    c->busy = 0;
    srv_unlock();
}

/* A free connection slot, its buffers allocated on first use and kept. */
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
        c->udp = 0;
        c->peer = peer;
        c->open.file = 0;
        c->open.path[0] = 0;
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

/* Datagrams, one at a time on the socket's thread; no file stays open. */
static void datagrams(void *arg)
{
    CONN c;
    u32 port, n;
    s32 got;

    c.sock = (SRV_SOCK)(long)arg;
    c.udp = 1;
    c.open.file = 0;
    c.open.path[0] = 0;
    c.in = (u8 *)srv_alloc(UDP_SIZE);
    c.out = (u8 *)srv_alloc(UDP_SIZE);
    if (!c.in || !c.out)
        return;
    for (;;) {
        got = srv_recvfrom(c.sock, c.in, UDP_SIZE, &c.peer, &port);
        if (got <= 0)
            continue;
        if (!allowed(c.peer)) {
            nfs_stats.refused++;
            continue;
        }
        n = rpc_call(&c, c.in, (u32)got, c.out, UDP_SIZE);
        nfs3_close(&c.open);
        if (n)
            srv_sendto(c.sock, c.out, n, c.peer, port);
    }
}

s32 transport_start(u32 port, u32 backlog)
{
    SRV_SOCK s = srv_listen(port, backlog);

    if (s < 0)
        return -1;
    if (srv_thread(listener, (void *)(long)s)) {
        srv_close(s);
        return -1;
    }
    s = srv_udp(port);
    if (s >= 0)
        srv_thread(datagrams, (void *)(long)s);
    return 0;
}

/* === RPC ===================================================================== */

/* Credentials are taken as given (AUTH_SYS on a LAN, as
 * NFSv3 is) and every reply carries AUTH_NONE. */

#define MSG_CALL        0u
#define MSG_REPLY       1u
#define RPC_VERSION     2u
#define MSG_ACCEPTED    0u
#define SUCCESS         0u
#define PROG_UNAVAIL    1u
#define PROG_MISMATCH   2u
#define PROC_UNAVAIL    3u
#define GARBAGE_ARGS    4u

/* The versions of a program served, low to high; 0 for a program not served. */
static u32 versions(u32 prog, u32 *low, u32 *high)
{
    switch (prog) {
    case PROG_NFS: *low = 3; *high = 3; return 1;
    case PROG_MOUNT: *low = 1; *high = 3; return 1;
    case PROG_PORTMAP: *low = 2; *high = 4; return 1;
    default: return 0;
    }
}

/* The program's answer after SUCCESS; 0 when the procedure is not served. */
static u32 dispatch(CONN *c, u32 prog, u32 vers, u32 proc, XIN *a, XOUT *r)
{
    switch (prog) {
    case PROG_NFS:
        if (proc > NFS3_PROC_MAX)
            return 0;
        nfs3_call(c, proc, a, r);
        return 1;
    case PROG_MOUNT:
        if (proc > MOUNT_PROC_MAX)
            return 0;
        mount_call(proc, a, r);
        return 1;
    default:
        return proc <= portmap_proc_max(vers) && portmap_call(c, vers, proc, a, r);
    }
}

u32 rpc_call(CONN *c, const u8 *in, u32 len, u8 *out, u32 size)
{
    XIN a = { in, len, 0, 0 };
    XOUT r = { out, size, 0, 0 };
    u32 xid, prog, vers, proc, n, accept_at, low, high;

    xid = xdr_get32(&a);
    if (xdr_get32(&a) != MSG_CALL || xdr_get32(&a) != RPC_VERSION)
        return 0;
    prog = xdr_get32(&a);
    vers = xdr_get32(&a);
    proc = xdr_get32(&a);
    xdr_get32(&a);
    xdr_get_opaque(&a, &n, 400);                /* credentials */
    xdr_get32(&a);
    xdr_get_opaque(&a, &n, 400);                /* verifier */
    if (a.bad) {
        nfs_stats.bad_calls++;
        return 0;
    }
    nfs_stats.calls++;

    xdr_put32(&r, xid);
    xdr_put32(&r, MSG_REPLY);
    xdr_put32(&r, MSG_ACCEPTED);
    xdr_put32(&r, 0);                           /* verifier: AUTH_NONE, empty */
    xdr_put32(&r, 0);
    accept_at = r.at;

    if (!versions(prog, &low, &high)) {
        xdr_put32(&r, PROG_UNAVAIL);
    } else if (vers < low || vers > high) {
        xdr_put32(&r, PROG_MISMATCH);
        xdr_put32(&r, low);
        xdr_put32(&r, high);
    } else {
        xdr_put32(&r, SUCCESS);
        if (!dispatch(c, prog, vers, proc, &a, &r)) {
            r.at = accept_at;
            xdr_put32(&r, PROC_UNAVAIL);
        } else if (a.bad) {
            r.at = accept_at;
            r.bad = 0;
            xdr_put32(&r, GARBAGE_ARGS);
        }
    }
    if (r.bad) {
        nfs_stats.bad_calls++;
        return 0;
    }
    return r.at;
}

/* === XDR ===================================================================== */

u32 xdr_get32(XIN *x)
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

void xdr_get64(XIN *x, u32 *hi, u32 *lo)
{
    *hi = xdr_get32(x);
    *lo = xdr_get32(x);
}

const u8 *xdr_get_opaque(XIN *x, u32 *n, u32 max)
{
    const u8 *p;

    *n = xdr_get32(x);
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

void xdr_put32(XOUT *x, u32 v)
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

void xdr_put64(XOUT *x, u32 hi, u32 lo)
{
    xdr_put32(x, hi);
    xdr_put32(x, lo);
}

void xdr_put_fixed(XOUT *x, const void *data, u32 n)
{
    u32 i, padded = (n + 3) & ~3u;

    if (x->at + padded > x->size) {
        x->bad = 1;
        return;
    }
    mem_copy(x->p + x->at, data, n);
    for (i = n; i < padded; i++)
        x->p[x->at + i] = 0;
    x->at += padded;
}

void xdr_put_opaque(XOUT *x, const void *data, u32 n)
{
    xdr_put32(x, n);
    xdr_put_fixed(x, data, n);
}

void xdr_put_string(XOUT *x, const char *s)
{
    xdr_put_opaque(x, s, str_len(s));
}
