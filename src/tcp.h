/* A throughput server inside the plugin, on the console's own network stack.
 *
 * Two ports, both served out of RAM with no disk anywhere:
 *   5201  source  every accepted connection is sent `tcp_chunk` bytes at a time
 *                 out of one RAM buffer, until the far end closes
 *   5202  sink    every accepted connection is read and thrown away until EOF
 * One thread an accepted connection, so several streams can run at once.
 *
 * Why: ftpdll and XBDM both add a file server and a disk to every measurement,
 * and the two disagree by 7 to 15 percent over the same card. This is the same
 * path with neither -- xnet, TCP and the driver and nothing else -- which is what
 * CUsbEnet's author measured his 32 MB/s to a "local speed test socket" with.
 *
 * The socket calls are xam's NetDll_* exports, imported by ordinal like every
 * kernel call: tools/build_plugin.py packs the second import library. Ordinals
 * from `xerunner alfred ordinals --module xam.xex`, identical at 20871 and 21256.
 */

#ifndef XE_TCP_H
#define XE_TCP_H

#include "kernel.h"

/* All four are read when a connection starts, so a change applies to the next
 * one; xectl.py set tcp_chunk 65536 and so on. */
extern volatile u32 tcp_chunk;      /* bytes per send/recv call */
extern volatile u32 tcp_sndbuf;     /* SO_SNDBUF, 0 leaves xnet's own */
extern volatile u32 tcp_rcvbuf;     /* SO_RCVBUF, 0 leaves xnet's own */
extern volatile u32 tcp_nodelay;    /* TCP_NODELAY on the source's sends */

/* Start the listeners. Thread-only -- it allocates and blocks. Returns 0, or the
 * step that failed; calling it again after success does nothing. */
u32 tcp_start(void);

enum {
    TCP_OK = 0,
    TCP_ERR_POOL = 1,
    TCP_ERR_STARTUP,
    TCP_ERR_SOCKET,
    TCP_ERR_BIND,
    TCP_ERR_LISTEN,
    TCP_ERR_THREAD,
};

typedef struct {
    u32 started;
    u32 step;               /* the step that failed, TCP_ERR_* */
    u32 step_error;         /* what WSAGetLastError said about it */
    u32 accepts;
    u32 active;
    u32 refused;            /* accepted with no free slot */
    u32 sent_lo, sent_hi;
    u32 recv_lo, recv_hi;
    u32 send_errors;
    u32 recv_errors;
    u32 last_send_error;
    u32 last_recv_error;
} TCP_STATS;

extern volatile TCP_STATS tcp_stats;

#endif /* XE_TCP_H */
