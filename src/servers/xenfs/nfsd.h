/* The server's internals, shared by its files; users of the server have nfs.h.
 *   nfs.c       start, sockets (TCP records, datagrams), RPC, XDR
 *   portmap.c   the portmapper and rpcbind, MOUNT
 *   handle.c    file handles and attributes
 *   nfs3.c      NFS v3 on files and the file system; nfs3_dir.c on directories */

#ifndef XE_NFSD_H
#define XE_NFSD_H

#include "nfs.h"
#include "str.h"

#define PROG_PORTMAP    100000u     /* portmapper v2, rpcbind v3 and v4 */
#define PROG_NFS        100003u
#define PROG_MOUNT      100005u

#define IO_MAX          65536u      /* the largest READ or WRITE: rtmax / wtmax */
/* TCP connections served at once: nconnect's 16 and, beside them, the client's calls
 * to the portmapper before each (with 16, nconnect=16 got 13 and hung on the rest). */
#define CONN_MAX        24

extern NFS_CONFIG nfs_cfg;          /* as nfs_start was given it, defaults filled in */

/* An open file, shared by every connection (nfs3.c), with its attributes as found
 * and written since (READ and WRITE answer with them, not a stat of their own). */
typedef struct {
    SRV_FILE file;                  /* 0: a free entry */
    char path[SRV_PATH_MAX];
    u32 writable;
    u32 users;                      /* calls using it now */
    u32 doomed;                     /* closed by the last of them */
    u32 used;                       /* srv_now() of its last use */
    SRV_STAT st;
} NFS_FILE;

/* The directory a connection is listing, kept open while the listing goes on:
 * `pos` entries answered, which is the cookie the next READDIR asks for if it goes
 * on; an entry read that did not fit in the last answer is held for the next. */
typedef struct {
    SRV_DIR dir;
    char path[SRV_PATH_MAX];
    u32 pos;
    u32 held;
    char held_name[SRV_NAME_MAX + 1];
    SRV_STAT held_st;
} NFS_LIST;

/* One client's transport: a TCP connection, or the socket a datagram came in on. */
typedef struct {
    SRV_SOCK sock;
    u32 udp;
    u32 peer;                       /* its address (host order) */
    u8 *in;
    u8 *out;
    NFS_LIST list;
    volatile u32 busy;
} CONN;

/* === XDR ===================================================================== */

/* XDR (RFC 4506): a call's arguments read, a reply written, in place. A read past
 * the end or a write past the buffer sets `bad` and does nothing more, so a
 * procedure reads and writes straight on and checks once. */

typedef struct {
    const u8 *p;
    u32 len, at, bad;
} XIN;

typedef struct {
    u8 *p;
    u32 size, at, bad;
} XOUT;

u32  xdr_get32(XIN *x);
void xdr_get64(XIN *x, u32 *hi, u32 *lo);
/* An opaque or a string, left in the input: its bytes, *n its length (<= max). */
const u8 *xdr_get_opaque(XIN *x, u32 *n, u32 max);

void xdr_put32(XOUT *x, u32 v);
void xdr_put64(XOUT *x, u32 hi, u32 lo);
void xdr_put_opaque(XOUT *x, const void *data, u32 n);
void xdr_put_string(XOUT *x, const char *s);
/* n bytes as they are (a fixed-size opaque such as a verifier). */
void xdr_put_fixed(XOUT *x, const void *data, u32 n);

/* === RPC ===================================================================== */

/* ONC RPC (RFC 5531): a call taken apart, handed to its program, the reply's
 * header around what the program wrote. */

/* One call in, its reply out; the reply's length, 0 for none. */
u32 rpc_call(CONN *c, const u8 *in, u32 len, u8 *out, u32 size);

/* === Transport =============================================================== */

/* The sockets: TCP with record marking, a thread a connection, and datagrams. */

/* A TCP listener on `port` and a UDP socket beside it, each on its own thread;
 * 0 when the listener is up (the UDP one is a convenience, for MOUNT and the
 * portmapper). */
s32 transport_start(u32 port, u32 backlog);

/* === MOUNT =================================================================== */

/* MOUNT v1-v3 (RFC 1813 appendix I): "/" exported to everyone. */

#define MOUNT_PROC_MAX 5u

void mount_call(u32 proc, XIN *a, XOUT *r);

/* === Portmapper and rpcbind ================================================== */

/* The portmapper v2 (RFC 1833) and rpcbind v3/v4 (RFC 1833): where NFS and MOUNT
 * are, over TCP and UDP. Nothing registers: the table is what this server is. */

/* The highest procedure a version knows; 0 for a version not served. */
u32  portmap_proc_max(u32 vers);
/* 1 when the procedure is served (else PROC_UNAVAIL). */
u32  portmap_call(CONN *c, u32 vers, u32 proc, XIN *a, XOUT *r);

/* === File handles ============================================================ */

/* File handles: 8 bytes, a node's index and a generation. The nodes are a tree
 * of names, so a handle from before a restart, or of a name removed since, reads
 * as stale rather than as another file. Index 0 is the root's. */

#define FH_SIZE         8u
#define HANDLES_DEFAULT 4096u       /* names a client may hold handles to (84 B each: 344 KB;
                                     * 16384 took 1.4 MB of a system memory that runs out) */

/* The table of `count` nodes (0: the default); 0 on success. Called again it keeps
 * what it has. */
s32  handle_init(u32 count);
/* The handle of `path` ("" the root), made if new; an NFS3 status. */
u32  handle_of(const char *path, u8 *fh);
/* The path of a handle into `out`; an NFS3 status. */
u32  handle_path(const u8 *fh, u32 len, char *out);
/* After a rename: every held path under `from` now lives under `to`. */
void handle_renamed(const char *from, const char *to);
/* After a remove: handles of `path` go stale. */
void handle_removed(const char *path);
/* `dir` + "/" + `name` into `out`, "." and ".." resolved; an NFS3 status. */
u32  handle_child(const char *dir, const u8 *name, u32 n, char *out);

/* === Attributes ============================================================== */

/* NFS3 attributes: fattr3 from srv.h's SRV_STAT, post_op_attr, wcc_data, sattr3. */

/* The number a file is known by (fileid, 64 bits): its handle node's. */
void attr_fileid(const char *path, u32 *hi, u32 *lo);
void attr_put(XOUT *x, const SRV_STAT *s, const char *path);
/* post_op_attr: the attributes when they can be read, else none. */
void attr_put_post(XOUT *x, const char *path);
/* wcc_data with no pre-operation attributes: the client then just refreshes. */
void attr_put_wcc(XOUT *x, const char *path);
/* sattr3: only the size is acted on (FATX has no owners or modes). */
void attr_get_set(XIN *x, u32 *set_size, u32 *size_hi, u32 *size_lo);

/* === NFS v3 ================================================================== */

/* NFS v3 (RFC 1813): the procedures, in three files by what they act on --
 * nfs3.c (attributes, lookup, read, write, the file system) and nfs3_dir.c
 * (names in a directory: create, remove, rename, list). */

#define NFS3_PROC_MAX 21u

#define NFS3_OK             0u
#define NFS3ERR_PERM        1u
#define NFS3ERR_NOENT       2u
#define NFS3ERR_IO          5u
#define NFS3ERR_ACCES       13u
#define NFS3ERR_EXIST       17u
#define NFS3ERR_NOTDIR      20u
#define NFS3ERR_ISDIR       21u
#define NFS3ERR_INVAL       22u
#define NFS3ERR_FBIG        27u
#define NFS3ERR_NOSPC       28u
#define NFS3ERR_NAMETOOLONG 63u
#define NFS3ERR_NOTEMPTY    66u
#define NFS3ERR_STALE       70u
#define NFS3ERR_BADHANDLE   10001u
#define NFS3ERR_NOTSUPP     10004u
#define NFS3ERR_SERVERFAULT 10006u

/* A call as a procedure sees it: the arguments after the first file handle, the
 * handle's path (or the status it read as), the reply after the accept status. */
typedef struct {
    CONN *conn;
    XIN *a;
    XOUT *r;
    char path[SRV_PATH_MAX];
    u32 status;
} NFS3_CALL;

typedef void (*NFS3_PROC)(NFS3_CALL *q);

void nfs3_call(CONN *c, u32 proc, XIN *a, XOUT *r);

/* srv.h's error as an NFS3 status. */
u32  nfs3_status(s32 e);

/* The open file of `path` (opened for writing if asked), held until
 * nfs3_file_done; 0 with the error in *e. */
NFS_FILE *nfs3_file(const char *path, u32 write, s32 *e);
void nfs3_file_done(NFS_FILE *f);
/* Every open file of `path` or under it closed: before it is removed, renamed
 * or truncated. */
void nfs3_forget(const char *path);
/* The sweeper that closes idle files; 0 on success. */
s32  nfs3_files_start(void);
/* What a connection holds open (its listing), at its end. */
void nfs3_release(CONN *c);

/* The verifier of WRITE and COMMIT: one for the server's life (writes go through). */
extern u8 nfs3_write_verf[8];
void nfs3_init(void);

/* nfs3.c: files */
void nfs3_getattr(NFS3_CALL *q);
void nfs3_setattr(NFS3_CALL *q);
void nfs3_lookup(NFS3_CALL *q);
void nfs3_access(NFS3_CALL *q);
void nfs3_read(NFS3_CALL *q);
void nfs3_write(NFS3_CALL *q);
void nfs3_commit(NFS3_CALL *q);

/* nfs3_dir.c */
void nfs3_create(NFS3_CALL *q);
void nfs3_mkdir(NFS3_CALL *q);
void nfs3_remove(NFS3_CALL *q);
void nfs3_rmdir(NFS3_CALL *q);
void nfs3_rename(NFS3_CALL *q);
void nfs3_readdir(NFS3_CALL *q);
void nfs3_readdirplus(NFS3_CALL *q);

/* nfs3.c: the file system */
void nfs3_fsstat(NFS3_CALL *q);
void nfs3_fsinfo(NFS3_CALL *q);
void nfs3_pathconf(NFS3_CALL *q);
void nfs3_notsupp(NFS3_CALL *q);

#endif /* XE_NFSD_H */
