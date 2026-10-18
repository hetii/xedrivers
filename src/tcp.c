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
#define TCP_NODELAY_OPT 0x0001
#define INVALID_SOCKET  0xFFFFFFFFu

#define PORT_SOURCE  5201
#define PORT_SINK    5202
#define SLOTS        4
#define SLOT_BYTES   0x10000u       /* the most one send or recv can carry */

volatile u32 tcp_chunk = 0x8000;
volatile u32 tcp_sndbuf = 0x40000;
volatile u32 tcp_rcvbuf = 0x40000;
volatile u32 tcp_nodelay = 1;
volatile TCP_STATS tcp_stats;

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

static u32 clamp_chunk(u32 want)
{
    if (!want || want > SLOT_BYTES)
        return SLOT_BYTES;
    return want;
}

/* One connection, start to finish, on its own thread. */
static void serve(void *parameter)
{
    SLOT *slot = (SLOT *)parameter;
    u32 chunk = clamp_chunk(tcp_chunk);
    int moved;

    for (;;) {
        if (slot->sink) {
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

    if (tcp_rcvbuf) {
        u32 value = tcp_rcvbuf;
        NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_RCVBUF, &value,
                          sizeof(value));
    }
    if (tcp_sndbuf) {
        u32 value = tcp_sndbuf;
        NetDll_setsockopt(CALLER_SYSAPP, socket, SOL_SOCKET, SO_SNDBUF, &value,
                          sizeof(value));
    }

    if (tcp_nodelay) {
        u32 value = 1;
        NetDll_setsockopt(CALLER_SYSAPP, socket, IPPROTO_TCP, TCP_NODELAY_OPT,
                          &value, sizeof(value));
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

        if (accepted == INVALID_SOCKET) {
            sleep_ms(100);
            continue;
        }
        tcp_stats.accepts++;

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
        if (ExCreateThread(&thread, 0, 0, 0, (void *)serve, slot, 2) < 0) {
            tcp_stats.active--;
            NetDll_closesocket(CALLER_SYSAPP, accepted);
            slot->busy = 0;
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

    for (i = 0; i < SLOTS; i++) {
        slots[i].buffer = (u8 *)ExAllocatePoolTypeWithTag(SLOT_BYTES, 0x50435458u,
                                                          2);
        slots[i].socket = INVALID_SOCKET;
        if (!slots[i].buffer)
            return fail(TCP_ERR_POOL);
    }
    source_bytes = slots[0].buffer;     /* the source only ever reads it */
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
