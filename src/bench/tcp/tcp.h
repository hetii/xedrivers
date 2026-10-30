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
 * kernel call: kit/plugin.py packs the second import library from
 * kit/ordinals-xam-21256.0.txt. Ordinals
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
extern volatile u32 tcp_timeout;    /* SO_SNDTIMEO/SO_RCVTIMEO in ms, 0 = none */
extern volatile u32 tcp_cpu;        /* N+1 pins a connection to hardware thread N */
extern volatile u32 tcp_prio;       /* absolute thread priority, 0 = leave it */
extern volatile u32 tcp_sink_pause; /* the sink stops reading while set */
extern volatile u32 tcp_listen_rcvbuf;  /* SO_RCVBUF on the listeners, before cmd 14 */
extern volatile u32 tcp_listen_sndbuf;  /* SO_SNDBUF on the listeners, before cmd 14 */
extern volatile u32 tcp_trace;          /* allocate the sink port's trace, before cmd 14 */

/* Start the listeners. Thread-only -- it allocates and blocks. Returns 0, or the
 * step that failed; calling it again after success does nothing. */
u32 tcp_start(void);

/* Every frame xnet sends and receives (bench_frame_out/in): the send port's
 * bytes in flight. Any IRQL. */
void tcp_flight_out(const u8 *frame, u32 length);
void tcp_flight_in(const u8 *frame, u32 length);

enum {
    TCP_OK = 0,
    TCP_ERR_POOL = 1,
    TCP_ERR_STARTUP,
    TCP_ERR_SOCKET,
    TCP_ERR_BIND,
    TCP_ERR_LISTEN,
    TCP_ERR_THREAD,
    TCP_ERR_INSECURE,       /* xnet refused the insecure-socket options */
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
    u32 sndbuf_result;      /* 0, or WSAGetLastError() if setsockopt failed */
    u32 rcvbuf_result;
    /* What the console has sent on PORT_SOURCE and not yet seen acknowledged,
     * sampled at every data segment it sends (tcp_flight_out). */
    u32 flight_samples;
    u32 flight_kb_sum;          /* of the samples, in KB */
    u32 flight_max;             /* bytes */
    u32 flight_hist[8];         /* samples by 16 KB: 0-16, 16-32, ... 112 and up */
    u32 flight_resent;          /* data segments at or below the highest sent */
    u32 thread_failures;        /* accepted, but no thread could be made for it */
    u32 thread_status;          /* what ExCreateThread said the last time */
} TCP_STATS;

extern volatile TCP_STATS tcp_stats;

#endif /* XE_TCP_H */
