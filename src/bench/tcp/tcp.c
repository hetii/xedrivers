/* The throughput server; see tcp.h.
 *
 * xam's socket calls all take the caller kind as their first argument (1 for a
 * title, 2 for a system app, as every xnet user passes it) and otherwise read as
 * Winsock. This console is big-endian, so a port and an address in a sockaddr are
 * already in network order with a plain assignment and nothing is byte-swapped
 * here.
 */

#include "tcp.h"

extern void *ExAllocatePoolTypeWithTag(u32 bytes, u32 tag, u32 poolType);

/* (thread, affinity mask, &previous): read off ordinal 151 on 17559. The current
 * thread is the word at PCR+0x100, the first field of the KPRCB, which is where
 * KeDelayExecutionThread itself loads it from. */
extern u32 KeSetAffinityThread(void *thread, u32 affinity, u32 *previous);

/* (thread, priority), absolute; returns the old one. Ordinal 163 on 17559. */
extern u32 KeSetPriorityThread(void *thread, u32 priority);

static inline void *current_thread(void)
{
    void *t;
#if defined(__powerpc__)
    __asm__ volatile("lwz %0, 0x100(13)" : "=r"(t));
#else
    t = 0;
#endif
    return t;
}

/* xam.xex, by ordinal: WSAStartup 1, socket 3, closesocket 4, setsockopt 7,
 * bind 11, listen 13, accept 14, recv 18, send 22, WSAGetLastError 27. */
extern int NetDll_WSAStartup(u32 caller, u16 version, void *data);
extern u32 NetDll_socket(u32 caller, int family, int type, int protocol);
extern int NetDll_closesocket(u32 caller, u32 socket);
extern int NetDll_setsockopt(u32 caller, u32 socket, int level, int option,
                             const void *value, int length);
extern int NetDll_bind(u32 caller, u32 socket, const void *address, int length);
extern int NetDll_listen(u32 caller, u32 socket, int backlog);
extern u32 NetDll_accept(u32 caller, u32 socket, void *address, int *length);
extern int NetDll_recv(u32 caller, u32 socket, void *buffer, int length, int flags);
extern int NetDll_send(u32 caller, u32 socket, const void *buffer, int length,
                       int flags);
extern int NetDll_WSAGetLastError(void);

#define CALLER_SYSAPP   2
#define AF_INET         2
#define SOCK_STREAM     1
#define IPPROTO_TCP     6
#define SOL_SOCKET      0xFFFF
#define SO_SNDBUF       0x1001
#define SO_RCVBUF       0x1002
#define SO_SNDTIMEO     0x1005
#define SO_RCVTIMEO     0x1006
#define TCP_NODELAY_OPT 0x0001
/* xnet drops connections to a socket it has not been told may be insecure.
 * ftpdll's own helper sets these two, 0x5802 then 0x5801, to 1 on every socket and
 * gives up if either fails (read off its code at 0x91806000); a SYN to a socket
 * without them is dropped silently -- connect() times out, it is not refused. */
#define SO_XNET_5802    0x5802
#define SO_XNET_5801    0x5801
#define INVALID_SOCKET  0xFFFFFFFFu

#define PORT_SOURCE  5201
#define PORT_SINK    5202
#define SLOTS        64
#define SLOT_BYTES   0x10000u       /* the most one send or recv can carry */

volatile u32 tcp_chunk = 0x8000;
/* Zero for everything by default: ftpdll sets none of these, only the two
 * insecure-socket options, and that is the baseline to tune away from. */
volatile u32 tcp_sndbuf = 0;
volatile u32 tcp_rcvbuf = 0;
volatile u32 tcp_timeout = 0;     /* ms for SO_SNDTIMEO/SO_RCVTIMEO, 0 = none */
volatile u32 tcp_cpu = 0;         /* N+1 pins each connection to hardware thread N */
/* A connection's thread waits in send() for xnet to free buffer space, and at the
 * default priority it then waits again for a running thread's quantum: 190 gaps
 * of 13 ms in a 3 s capture, 78% of the time idle. Measured: 18, 24 and anything
 * up to 17 give 4.5 / 2.5 MB/s, every time; 19, 20, 22, 28 and 31 give 22 / 27.
 * 18 and 24 are most likely xnet's own threads, whose quantum ours then shares.
 * 20 is the best of them and stays below the real-time band. 0 leaves it alone. */
volatile u32 tcp_prio = 20;
volatile u32 tcp_nodelay = 0;
volatile u32 tcp_sink_pause = 0;
/* SO_RCVBUF/SO_SNDBUF on the listening sockets, before listen(): what an accepted
 * connection inherits and what its SYN-ACK offers (window scaling) -- set them
 * before cmd 14. 0 leaves xnet's defaults. */
volatile u32 tcp_listen_rcvbuf = 0;
volatile u32 tcp_listen_sndbuf = 0;
volatile u32 tcp_trace = 0;       /* the sink port's trace (4 MB), set before cmd 14 */  /* the sink stops reading while set: buffers fill */
volatile TCP_STATS tcp_stats;

/* The sink port's trace (trace(), below). */
#define TRACE_COUNT 262144u
volatile u32 *volatile sink_trace;
volatile u32 sink_trace_at;

typedef struct {
    u16 family;
    u16 port;
    u32 address;
    u8  zero[8];
} SOCKADDR_IN;

typedef struct {
    volatile u32 busy;
    u32          socket;
    u32          sink;              /* 0 source, 1 sink */
    u8          *buffer;
} SLOT;

typedef struct {
    u32 socket;
    u32 sink;
} LISTENER;

static SLOT slots[SLOTS];
static LISTENER listeners[2];
static u8 *source_bytes;            /* what the source sends, shared, read-only */
static u8 *sink_bytes;              /* where every sink reads to, thrown away */

static void sleep_ms(u32 ms)
{
    s64 interval = -((s64)ms * 10000);      /* relative, 100 ns units */
    KeDelayExecutionThread(0, 0, &interval);
}

static u32 fail(u32 step)
{
    tcp_stats.step = step;
    tcp_stats.step_error = (u32)NetDll_WSAGetLastError();
    return step;
}

static void add64(volatile u32 *lo, volatile u32 *hi, u32 n)
{
    u32 before = *lo;

    *lo = before + n;
    if (*lo < before)
        *hi = *hi + 1;
}

static int mark_insecure(u32 socket)
{
    u32 one = 1;

    if (NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_XNET_5802, &one,
                          sizeof(one)) < 0)
        return -1;
    return NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_XNET_5801,
                             &one, sizeof(one));
}

/* Buffers, Nagle and timeouts go on each accepted connection, read when it is
 * accepted, so they can be swept while the server runs. The timeouts are what
 * hand a slot back when a client vanishes without closing. */
static void tune(u32 socket)
{
    u32 value;

    if (tcp_rcvbuf) {
        value = tcp_rcvbuf;
        tcp_stats.rcvbuf_result = (u32)NetDll_setsockopt(CALLER_SYSAPP, socket,
                                                         SOL_SOCKET, SO_RCVBUF,
                                                         &value, sizeof(value));
        if ((int)tcp_stats.rcvbuf_result < 0)
            tcp_stats.rcvbuf_result = (u32)NetDll_WSAGetLastError();
    }
    if (tcp_sndbuf) {
        value = tcp_sndbuf;
        tcp_stats.sndbuf_result = (u32)NetDll_setsockopt(CALLER_SYSAPP, socket,
                                                         SOL_SOCKET, SO_SNDBUF,
                                                         &value, sizeof(value));
        if ((int)tcp_stats.sndbuf_result < 0)
            tcp_stats.sndbuf_result = (u32)NetDll_WSAGetLastError();
    }
    if (tcp_nodelay) {
        value = 1;
        NetDll_setsockopt(CALLER_SYSAPP, socket, IPPROTO_TCP, TCP_NODELAY_OPT,
                          &value, sizeof(value));
    }
    if (tcp_timeout) {
        value = tcp_timeout;
        NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_SNDTIMEO, &value,
                          sizeof(value));
        NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_RCVTIMEO, &value,
                          sizeof(value));
    }
}

static u32 clamp_chunk(u32 want)
{
    if (!want || want > SLOT_BYTES)
        return SLOT_BYTES;
    return want;
}

/* One connection, start to finish, on its own thread. */
static void pin(void)
{
    u32 cpu = tcp_cpu, prio = tcp_prio, previous;

    if (cpu && cpu <= 6)
        KeSetAffinityThread(current_thread(), 1u << (cpu - 1), &previous);
    if (prio && prio < 32)
        KeSetPriorityThread(current_thread(), prio);
}

static void serve(void *parameter)
{
    SLOT *slot = (SLOT *)parameter;
    u32 chunk = clamp_chunk(tcp_chunk);
    int moved;

    pin();

    for (;;) {
        if (slot->sink) {
            if (tcp_sink_pause) {
                sleep_ms(10);
                continue;
            }
            moved = NetDll_recv(CALLER_SYSAPP, slot->socket, slot->buffer,
                                (int)chunk, 0);
            if (moved > 0) {
                add64(&tcp_stats.recv_lo, &tcp_stats.recv_hi, (u32)moved);
                continue;
            }
            if (moved < 0) {
                tcp_stats.recv_errors++;
                tcp_stats.last_recv_error = (u32)NetDll_WSAGetLastError();
            }
            break;                  /* 0 is the far end done with us */
        }

        moved = NetDll_send(CALLER_SYSAPP, slot->socket, source_bytes, (int)chunk,
                            0);
        if (moved > 0) {
            add64(&tcp_stats.sent_lo, &tcp_stats.sent_hi, (u32)moved);
            continue;
        }
        if (moved < 0) {
            tcp_stats.send_errors++;
            tcp_stats.last_send_error = (u32)NetDll_WSAGetLastError();
        }
        break;
    }

    NetDll_closesocket(CALLER_SYSAPP, slot->socket);
    slot->socket = INVALID_SOCKET;
    tcp_stats.active--;
    mem_sync();
    slot->busy = 0;
}

static SLOT *take_slot(void)
{
    u32 i;

    for (i = 0; i < SLOTS; i++) {
        if (!__sync_lock_test_and_set(&slots[i].busy, 1))
            return &slots[i];
    }
    return 0;
}

static u32 listener(u32 port, u32 *out)
{
    SOCKADDR_IN address;
    u32 socket = NetDll_socket(CALLER_SYSAPP, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    u32 i;

    if (socket == INVALID_SOCKET)
        return fail(TCP_ERR_SOCKET);
    if (mark_insecure(socket) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, socket);
        return fail(TCP_ERR_INSECURE);
    }

    if (tcp_listen_rcvbuf) {
        i = tcp_listen_rcvbuf;
        NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_RCVBUF, &i, sizeof(i));
    }
    if (tcp_listen_sndbuf) {
        i = tcp_listen_sndbuf;
        NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_SNDBUF, &i, sizeof(i));
    }
    address.family = AF_INET;
    address.port = (u16)port;           /* big-endian machine: already network */
    address.address = 0;                /* every interface */
    for (i = 0; i < sizeof(address.zero); i++)
        address.zero[i] = 0;

    if (NetDll_bind(CALLER_SYSAPP, socket, &address, sizeof(address)) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, socket);
        return fail(TCP_ERR_BIND);
    }
    if (NetDll_listen(CALLER_SYSAPP, socket, SLOTS) < 0) {
        NetDll_closesocket(CALLER_SYSAPP, socket);
        return fail(TCP_ERR_LISTEN);
    }
    *out = socket;
    return TCP_OK;
}

/* Accept for one port forever, handing each connection to a slot's thread. */
static void accept_loop(void *parameter)
{
    LISTENER *listening = (LISTENER *)parameter;

    for (;;) {
        u32 accepted = NetDll_accept(CALLER_SYSAPP, listening->socket, 0, 0);
        SLOT *slot;
        void *thread = 0;
        int status;

        if (accepted == INVALID_SOCKET) {
            sleep_ms(100);
            continue;
        }
        tcp_stats.accepts++;
        mark_insecure(accepted);
        tune(accepted);

        slot = take_slot();
        if (!slot) {
            tcp_stats.refused++;
            NetDll_closesocket(CALLER_SYSAPP, accepted);
            continue;
        }
        slot->socket = accepted;
        slot->sink = listening->sink;
        tcp_stats.active++;
        mem_sync();
        status = ExCreateThread(&thread, 0, 0, 0, (void *)serve, slot, 2);
        if (status < 0) {
            tcp_stats.active--;
            tcp_stats.thread_failures++;
            tcp_stats.thread_status = (u32)status;
            NetDll_closesocket(CALLER_SYSAPP, accepted);
            slot->busy = 0;
        } else {
            /* the handle only: kept, every connection's thread object and stack
             * stayed after it ended, and after ~80 the next thread failed */
            NtClose(thread);
        }
    }
}

u32 tcp_start(void)
{
    static u32 wsa_data[128];           /* WSADATA, larger than it needs */
    u32 source = 0, sink = 0, i;
    void *thread = 0;

    if (tcp_stats.started)
        return TCP_OK;

    /* two buffers for every slot: the sinks throw what they read into one away,
     * the sources only read the other, so SLOTS connections need no SLOTS of them */
    source_bytes = (u8 *)ExAllocatePoolTypeWithTag(SLOT_BYTES, 0x50435458u, 2);
    sink_bytes = (u8 *)ExAllocatePoolTypeWithTag(SLOT_BYTES, 0x50435458u, 2);
    if (!source_bytes || !sink_bytes)
        return fail(TCP_ERR_POOL);
    for (i = 0; i < SLOTS; i++) {
        slots[i].buffer = sink_bytes;
        slots[i].socket = INVALID_SOCKET;
    }
    if (tcp_trace)                      /* 4 MB: only when asked, memory is short */
        sink_trace = (volatile u32 *)MmAllocatePhysicalMemory(2, TRACE_COUNT * 16, 4);
    for (i = 0; i < SLOT_BYTES; i++)
        source_bytes[i] = (u8)i;

    if (NetDll_WSAStartup(CALLER_SYSAPP, 2, wsa_data) < 0)
        return fail(TCP_ERR_STARTUP);

    if (listener(PORT_SOURCE, &source))
        return tcp_stats.step;
    if (listener(PORT_SINK, &sink))
        return tcp_stats.step;
    listeners[0].socket = source;
    listeners[0].sink = 0;
    listeners[1].socket = sink;
    listeners[1].sink = 1;
    mem_sync();

    for (i = 0; i < 2; i++) {
        if (ExCreateThread(&thread, 0, 0, 0, (void *)accept_loop, &listeners[i],
                           2) < 0)
            return fail(TCP_ERR_THREAD);
    }

    tcp_stats.started = 1;
    return TCP_OK;
}

/* --- bytes in flight on the send port ---------------------------------------
 *
 * Seen from the console, which a capture on the PC cannot do: the highest
 * sequence number the console has sent on PORT_SOURCE against the last ACK it
 * received. One connection at a time is what this is for. */

static volatile u32 flight_top, flight_ack, flight_valid;

static u32 be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

/* The TCP header of an Ethernet IPv4 frame, its payload length in *payload. */
static const u8 *tcp_of(const u8 *f, u32 length, u32 *payload)
{
    u32 ihl, total, off;

    if (length < 54 || f[12] != 0x08 || f[13] != 0x00 || f[23] != 6)
        return 0;
    ihl = (u32)(f[14] & 15) * 4;
    total = (u32)f[16] << 8 | f[17];
    if (ihl < 20 || 14 + ihl + 20 > length)
        return 0;
    off = (u32)(f[14 + ihl + 12] >> 4) * 4;
    *payload = total > ihl + off ? total - ihl - off : 0;
    return f + 14 + ihl;
}

/* Every segment xnet is handed on PORT_SINK and every ACK it sends there, for a
 * capture on the PC to be held against: { time base, sequence or ack number,
 * client port << 16 | payload length (bit 15: what the console sent), the frame's
 * length }. 4 MB from tcp_start, sink_trace pointing at it, sink_trace_at the
 * next entry (it wraps). */

static void trace(u32 number, u32 port, u32 word, u32 length)
{
    volatile u32 *e;
    u32 at, tb;

    if (!sink_trace)
        return;
    at = __sync_fetch_and_add(&sink_trace_at, 1) & (TRACE_COUNT - 1);
#if defined(__powerpc__)
    __asm__ volatile("mftb %0" : "=r"(tb));
#else
    tb = 0;
#endif
    e = sink_trace + at * 4;
    e[0] = tb;
    e[1] = number;
    e[2] = port << 16 | word;
    e[3] = length;
}

void tcp_flight_out(const u8 *frame, u32 length)
{
    u32 payload, top, flight, kb;
    const u8 *t = tcp_of(frame, length, &payload);

    if (t && ((u32)t[0] << 8 | t[1]) == PORT_SINK)
        trace(be32(t + 8), (u32)t[2] << 8 | t[3], 0x8000u | (t[13] & 0x3Fu), length);
    if (!t || ((u32)t[0] << 8 | t[1]) != PORT_SOURCE || !payload)
        return;
    top = be32(t + 4) + payload;
    if (flight_valid && (s32)(top - flight_top) <= 0) {
        tcp_stats.flight_resent++;
        return;
    }
    if (!flight_valid) {
        flight_ack = be32(t + 4);
        flight_valid = 1;
    }
    flight_top = top;
    flight = top - flight_ack;
    if (flight > 0x1000000)
        return;
    kb = flight >> 10;
    tcp_stats.flight_samples++;
    tcp_stats.flight_kb_sum += kb;
    if (flight > tcp_stats.flight_max)
        tcp_stats.flight_max = flight;
    tcp_stats.flight_hist[kb >= 112 ? 7 : kb / 16]++;
}

void tcp_flight_in(const u8 *frame, u32 length)
{
    u32 payload, ack;
    const u8 *t = tcp_of(frame, length, &payload);

    if (t && ((u32)t[2] << 8 | t[3]) == PORT_SINK)
        trace(be32(t + 4), (u32)t[0] << 8 | t[1], payload & 0x7FFFu, length);
    if (!t || ((u32)t[2] << 8 | t[3]) != PORT_SOURCE || !(t[13] & 0x10))
        return;
    if (t[13] & 0x02) {                     /* a new connection's SYN */
        flight_valid = 0;
        return;
    }
    ack = be32(t + 8);
    if (flight_valid && (s32)(ack - flight_ack) > 0)
        flight_ack = ack;
}
