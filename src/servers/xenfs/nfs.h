/* An NFSv3 server over TCP (RFC 1813), with the MOUNT v3 protocol and a portmapper
 * (RFC 1833, v2) beside it, on the platform of src/servers/common/srv.h only.
 *
 * NFS and MOUNT share one TCP port (NFS_PORT_DEFAULT, 2049): RPC names the program
 * in every call. The portmapper answers on 111, TCP and UDP, and points both
 * programs there. Authentication is AUTH_SYS taken as given (a LAN server, as NFSv3
 * is); `nfs_allow` may restrict the clients by address. File handles are
 * 8 bytes, an index into a table of paths and a generation, so a handle from
 * before a restart reads as stale rather than as another file.
 *
 * Linux mounts it with `mount -t nfs -o vers=3,proto=tcp,nolock host:/ dir`, and
 * `nconnect=N` opens N connections, each served by a thread of its own.
 */

#ifndef XE_NFS_H
#define XE_NFS_H

#include "srv.h"

#define NFS_PORT_DEFAULT     2049u
#define PORTMAP_PORT_DEFAULT 111u

typedef struct {
    const char *root;       /* the directory exported as "/" */
    u32 port;               /* NFS and MOUNT; 0 for NFS_PORT_DEFAULT */
    u32 portmap_port;       /* 0 for PORTMAP_PORT_DEFAULT; ~0u for no portmapper */
    u32 allow;              /* a client address (host order) and */
    u32 allow_mask;         /* its mask; 0 / 0 lets anyone in */
    u32 handles;            /* names held at once; 0 for the default (16384) */
} NFS_CONFIG;

/* Counters, read by tools on the host. */
typedef struct {
    u32 started;
    u32 connections;
    u32 active;
    u32 calls;
    u32 bad_calls;
    u32 refused;            /* clients outside nfs_allow */
    u32 reads;
    u32 read_bytes_lo, read_bytes_hi;
    u32 writes;
    u32 write_bytes_lo, write_bytes_hi;
    u32 stale;              /* handles not in the table (any more) */
    u32 handles;            /* names in the table */
    u32 handles_full;       /* a handle refused: no leaf to give up */
    u32 portmap_calls;
    u32 mounts;
    u32 commits;
    u32 opens;              /* files opened for READ, WRITE, COMMIT */
    u32 listings;           /* directories opened for READDIR */
    u32 no_thread;          /* a connection closed: its thread did not start */
    u32 no_slot;            /* a connection closed: CONN_MAX at work */
} NFS_STATS;

extern volatile NFS_STATS nfs_stats;

/* Start the listeners on threads of their own; 0 on success. The config is copied. */
s32 nfs_start(const NFS_CONFIG *config);

#endif /* XE_NFS_H */
