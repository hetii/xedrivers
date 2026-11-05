/* The platform a file server runs on: sockets, threads, files and time, and
 * nothing else. The servers (src/servers/<name>/) use only this, never the
 * drivers, the network layer or xnet's internals, so a server can be lifted into
 * another project with this header and an implementation of it.
 *
 * The console's implementation is srv_net.c (xam's sockets), srv_file.c (the
 * kernel's files) and srv_sys.c (threads, memory, time, settings). The host tests
 * build the same servers on a POSIX implementation of this header under the same
 * names in tests/servers/common/; nothing here is for the PC.
 *
 * Paths are the server's own: '/'-separated, relative to the root it was given,
 * never with "..". Errors are SRV_E* (negative), success 0 or a count. On the
 * console a root of "*" is every volume: "Hdd1/x", "Usb0/x"..., "" their list.
 */

#ifndef XE_SRV_H
#define XE_SRV_H

#include "kernel.h"

#define SRV_OK          0
#define SRV_ENOENT     (-2)
#define SRV_EIO        (-5)
#define SRV_EACCES     (-13)
#define SRV_EEXIST     (-17)
#define SRV_ENOTDIR    (-20)
#define SRV_EISDIR     (-21)
#define SRV_EINVAL     (-22)
#define SRV_EFBIG      (-27)       /* FATX: no file reaches 4 GiB */
#define SRV_ENOSPC     (-28)
#define SRV_ENAMETOOLONG (-36)
#define SRV_ENOTEMPTY  (-39)

#define SRV_PATH_MAX    256
#define SRV_NAME_MAX    255

/* --- sockets ----------------------------------------------------------------- */

typedef s32 SRV_SOCK;               /* < 0: none */

/* A TCP listener on `port` of every address; < 0 on failure. */
SRV_SOCK srv_listen(u32 port, u32 backlog);
/* A UDP socket bound to `port`; < 0 on failure. */
SRV_SOCK srv_udp(u32 port);
/* The next connection; its peer's IPv4 address (host order) in *peer. */
SRV_SOCK srv_accept(SRV_SOCK listener, u32 *peer);
/* A TCP connection to `address`:`port` (host order), for FTP's active mode. */
SRV_SOCK srv_connect(u32 address, u32 port);
/* Bytes read, 0 at the far end's close, < 0 on error. */
s32  srv_recv(SRV_SOCK s, void *buffer, u32 length);
/* Every byte sent, or < 0. */
s32  srv_send(SRV_SOCK s, const void *buffer, u32 length);
/* One datagram and where it came from (host order); < 0 on error. */
s32  srv_recvfrom(SRV_SOCK s, void *buffer, u32 length, u32 *address, u32 *port);
s32  srv_sendto(SRV_SOCK s, const void *buffer, u32 length, u32 address, u32 port);
void srv_close(SRV_SOCK s);
/* 1 when `s` has something to read (a listener: a connection to accept), 0 after
 * `ms` without, < 0 on error: before a call that would wait for a client that
 * may never come. */
s32  srv_wait(SRV_SOCK s, u32 ms);
/* The local address (host order) a connection arrived on: FTP's PASV answer. */
u32  srv_local_address(SRV_SOCK s);
/* The port a listener bound to (0 asked for any). */
u32  srv_local_port(SRV_SOCK s);
/* The console's own address (host order) as `peer` reaches it: a datagram socket
 * bound to every address has none of its own to tell (rpcbind's GETADDR). */
u32  srv_host_address(u32 peer);

/* --- threads ----------------------------------------------------------------- */

/* Run fn(arg) on a thread of its own, at the priority data threads need on the
 * console (>= 19: below it xnet's own threads take the quantum, see bench/tcp).
 * 0 on success. */
s32  srv_thread(void (*fn)(void *arg), void *arg);
void srv_sleep(u32 ms);

/* --- files ------------------------------------------------------------------- */

typedef struct {
    u32 is_dir;
    u32 size_lo, size_hi;
    u32 mtime;                      /* seconds since 1970 */
    u32 atime;
    u32 ctime;
    u32 id;                         /* a number stable for the path while it exists */
} SRV_STAT;

typedef void *SRV_FILE;             /* 0: none */

#define SRV_READ    1u
#define SRV_WRITE   2u
#define SRV_CREATE  4u              /* create if missing */
#define SRV_EXCL    8u              /* with CREATE: fail if it exists */
#define SRV_TRUNC   16u

/* `root` is the directory the server exports; every path is under it. */
s32  srv_open(const char *root, const char *path, u32 mode, SRV_FILE *out);
s32  srv_read(SRV_FILE f, u32 offset_lo, u32 offset_hi, void *buffer, u32 length);
s32  srv_write(SRV_FILE f, u32 offset_lo, u32 offset_hi, const void *buffer, u32 length);
void srv_fclose(SRV_FILE f);
s32  srv_stat(const char *root, const char *path, SRV_STAT *out);
s32  srv_truncate(const char *root, const char *path, u32 size_lo, u32 size_hi);
s32  srv_mkdir(const char *root, const char *path);
s32  srv_rmdir(const char *root, const char *path);
s32  srv_unlink(const char *root, const char *path);
s32  srv_rename(const char *root, const char *from, const char *to);

/* Data written so far on its way to the disk; 0 or an error. */
s32  srv_flush(SRV_FILE f);

/* A directory listing, read in order: opened, its names one at a time, closed.
 * "." and ".." are not returned. */
typedef void *SRV_DIR;              /* 0: none */
s32  srv_opendir(const char *root, const char *path, SRV_DIR *out);
/* 1 with the next name (and its attributes when `st`), 0 at the end, < 0 on error. */
s32  srv_readdir(SRV_DIR d, char *name, u32 name_size, SRV_STAT *st);
void srv_closedir(SRV_DIR d);

/* The volume under `root`: bytes in all and free. */
s32  srv_volume(const char *root, u32 *total_lo, u32 *total_hi, u32 *free_lo,
                u32 *free_hi);

/* A server that keeps files open between calls names what closes them: called
 * with a root and a path (the name, or a directory above names), by srv_unlink,
 * srv_rmdir, srv_rename, srv_truncate and a truncating srv_open before they act,
 * whichever server calls them (FATX does not reliably remove an open file). */
void srv_keeper(void (*forget)(const char *root, const char *path));
/* What the platform calls (srv_keep.c). */
void srv_forget(const char *root, const char *path);

/* --- memory ------------------------------------------------------------------ */

/* Zeroed memory; 0 if there is none. Only what is under 32 KB can be given back to
 * srv_free: larger blocks are for the server's life. */
void *srv_alloc(u32 bytes);
void  srv_free(void *p);

/* One lock for a server's shared tables: short sections only, never held across
 * a socket or file call. */
void srv_lock(void);
void srv_unlock(void);

/* --- time and settings -------------------------------------------------------- */

u32  srv_now(void);                 /* seconds since 1970 */

/* A server's setting from the plugin's ini (`<key> = value`), or `fallback`. */
u32  srv_setting_u32(const char *key, u32 fallback);
/* A string setting into `out`; 1 if found. */
u32  srv_setting(const char *key, char *out, u32 size);

#endif /* XE_SRV_H */
