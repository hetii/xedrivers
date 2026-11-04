/* What clients ask before NFS: the portmapper v2 and rpcbind v3/v4 (RFC 1833),
 * and MOUNT (RFC 1813 appendix I). */

#include "nfsd.h"

/* === Portmapper and rpcbind ================================================== */

/* rpcinfo and libtirpc ask rpcbind
 * v4 and v3 first (GETADDR of the portmapper itself) and give up when told only
 * v2 is here, so all three are answered. */

#define IPPROTO_TCP 6u
#define IPPROTO_UDP 17u

typedef struct {
    u32 prog, vers;
} ENTRY;

static const ENTRY entries[] = {
    { PROG_PORTMAP, 4 }, { PROG_PORTMAP, 3 }, { PROG_PORTMAP, 2 },
    { PROG_NFS, 3 },
    { PROG_MOUNT, 3 }, { PROG_MOUNT, 2 }, { PROG_MOUNT, 1 },
};

#define ENTRIES (sizeof(entries) / sizeof(entries[0]))

/* The port a program is served on; 0 if it is not. */
static u32 port_of(u32 prog, u32 vers)
{
    u32 i;

    for (i = 0; i < ENTRIES; i++)
        if (entries[i].prog == prog && entries[i].vers == vers)
            return prog != PROG_PORTMAP ? nfs_cfg.port :
                   nfs_cfg.portmap_port != 0xFFFFFFFFu ? nfs_cfg.portmap_port : 0;
    return 0;
}

/* --- v2: ports ---------------------------------------------------------------- */

static u32 v2(u32 proc, XIN *a, XOUT *r)
{
    u32 prog, vers, i, p;

    switch (proc) {
    case 0:                                     /* NULL */
        return 1;
    case 1:                                     /* SET, UNSET: nothing registers */
    case 2:
        xdr_put32(r, 0);
        return 1;
    case 3:                                     /* GETPORT: both protocols served */
        prog = xdr_get32(a);
        vers = xdr_get32(a);
        xdr_get32(a);
        xdr_get32(a);
        xdr_put32(r, port_of(prog, vers));
        return 1;
    case 4:                                     /* DUMP */
        for (i = 0; i < ENTRIES; i++)
            for (p = IPPROTO_TCP; p <= IPPROTO_UDP; p += IPPROTO_UDP - IPPROTO_TCP) {
                if (!port_of(entries[i].prog, entries[i].vers))
                    continue;
                xdr_put32(r, 1);
                xdr_put32(r, entries[i].prog);
                xdr_put32(r, entries[i].vers);
                xdr_put32(r, p);
                xdr_put32(r, port_of(entries[i].prog, entries[i].vers));
            }
        xdr_put32(r, 0);
        return 1;
    default:                                    /* CALLIT */
        return 0;
    }
}

/* --- v3 and v4: universal addresses -------------------------------------------- */

static char *put_decimal(char *p, u32 v)
{
    char digits[10];
    u32 n = 0;

    do
        digits[n++] = (char)('0' + v % 10);
    while ((v /= 10));
    while (n)
        *p++ = digits[--n];
    return p;
}

/* "h1.h2.h3.h4.p1.p2" (RFC 5665): the address and port as six decimal bytes. */
static void uaddr(char *out, u32 address, u32 port)
{
    u32 i;
    char *p = out;

    for (i = 0; i < 4; i++) {
        p = put_decimal(p, (address >> (24 - 8 * i)) & 0xFF);
        *p++ = '.';
    }
    p = put_decimal(p, port >> 8 & 0xFF);
    *p++ = '.';
    p = put_decimal(p, port & 0xFF);
    *p = 0;
}

/* The address a client reaches this server at, as it reached it. */
static u32 local_address(CONN *c)
{
    return c->udp ? srv_host_address(c->peer) : srv_local_address(c->sock);
}

static u32 v3(CONN *c, u32 vers, u32 proc, XIN *a, XOUT *r)
{
    char netid[8], address[32];
    const u8 *s;
    u32 prog, pvers, n, i, k, port;
    static const char *const netids[] = { "tcp", "udp" };

    switch (proc) {
    case 0:                                     /* NULL */
        return 1;
    case 1:                                     /* SET, UNSET */
    case 2:
        xdr_put32(r, 0);
        return 1;
    case 3:                                     /* GETADDR */
    case 9:                                     /* GETVERSADDR (v4) */
        if (proc == 9 && vers < 4)
            return 0;
        prog = xdr_get32(a);
        pvers = xdr_get32(a);
        s = xdr_get_opaque(a, &n, 64);          /* r_netid */
        for (i = 0; i < n && i + 1 < sizeof(netid); i++)
            netid[i] = (char)s[i];
        netid[i] = 0;
        xdr_get_opaque(a, &n, 128);             /* r_addr */
        xdr_get_opaque(a, &n, 128);             /* r_owner */
        port = (str_eq(netid, "tcp") || str_eq(netid, "udp")) ? port_of(prog, pvers) : 0;
        if (port) {
            uaddr(address, local_address(c), port);
            xdr_put_string(r, address);
        } else {
            xdr_put_string(r, "");              /* not registered */
        }
        return 1;
    case 4:                                     /* DUMP */
        for (i = 0; i < ENTRIES; i++) {
            port = port_of(entries[i].prog, entries[i].vers);
            if (!port)
                continue;
            uaddr(address, local_address(c), port);
            for (k = 0; k < 2; k++) {
                xdr_put32(r, 1);
                xdr_put32(r, entries[i].prog);
                xdr_put32(r, entries[i].vers);
                xdr_put_string(r, netids[k]);
                xdr_put_string(r, address);
                xdr_put_string(r, "superuser");
            }
        }
        xdr_put32(r, 0);
        return 1;
    case 6:                                     /* GETTIME */
        xdr_put32(r, srv_now());
        return 1;
    default:                                    /* CALLIT, the address conversions */
        return 0;
    }
}

u32 portmap_proc_max(u32 vers)
{
    return vers == 2 ? 5 : vers == 3 ? 8 : vers == 4 ? 12 : 0;
}

u32 portmap_call(CONN *c, u32 vers, u32 proc, XIN *a, XOUT *r)
{
    nfs_stats.portmap_calls++;
    return vers == 2 ? v2(proc, a, r) : v3(c, vers, proc, a, r);
}

/* === MOUNT =================================================================== */

#define AUTH_SYS 1u

void mount_call(u32 proc, XIN *a, XOUT *r)
{
    u8 fh[FH_SIZE];
    u32 n;

    switch (proc) {
    case 1:                                     /* MNT: any path gives the root */
        xdr_get_opaque(a, &n, 1024);
        nfs_stats.mounts++;
        xdr_put32(r, 0);                        /* MNT3_OK */
        handle_of("", fh);
        xdr_put_opaque(r, fh, FH_SIZE);
        xdr_put32(r, 1);
        xdr_put32(r, AUTH_SYS);
        return;
    case 2:                                     /* DUMP: no list kept */
        xdr_put32(r, 0);
        return;
    case 5:                                     /* EXPORT: "/" to everyone */
        xdr_put32(r, 1);
        xdr_put_string(r, "/");
        xdr_put32(r, 0);                        /* no groups */
        xdr_put32(r, 0);                        /* no more */
        return;
    default:                                    /* NULL, UMNT, UMNTALL */
        return;
    }
}
