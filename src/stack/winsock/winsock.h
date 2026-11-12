/* xnet's socket interface, served by the own stack (make stack=1).
 *
 * Every program on the console reaches the network through xam's NetDll_* exports, in
 * xnet's flavour of Winsock. With stack=1 the start of each of those that touches a socket
 * is patched to jump here (hook.c), for every caller alike -- the dashboard, XBDM, ftpdll,
 * Aurora, our own servers, xam itself: all of it goes through lwIP, and xnet sees no frame.
 * What touches no socket stays xam's: inet_addr, __WSAFDIsSet, the WSA event wrappers,
 * XNetRandom, and WSAGetLastError / WSASetLastError, which read and write a word of the
 * calling thread that errors.c writes too.
 *
 *   hook.c      finding the exports and patching them; the entries the patches jump to
 *   calls.c     the socket calls themselves
 *   handles.c   a socket's Winsock handle and lwIP descriptor, and the caller that made it
 *   select.c    select(): Winsock's sets of handles to lwIP's bitmaps and back
 *   options.c   setsockopt / getsockopt: Winsock's numbers and layouts to lwIP's
 *   errors.c    lwIP's errno, per thread, and the thread's Winsock error as xam keeps it
 *   addr.c      start-up and clean-up, the console's address and link (XNetGetTitleXnAddr)
 *   lookup.c    XNetDnsLookup on lwIP's DNS
 *
 *   overlap.c   overlapped sends and receives (an OVERLAPPED and its event), WSARecvFrom,
 *               WSASendTo, WSAGetOverlappedResult, WSACancelOverlappedIO, WSAEventSelect
 *
 * Not served: xnet's secure transport for games (VDP sockets, XNetConnect, keys, QoS) -- a
 * game's network play does not work with stack=1.
 */
#ifndef XE_WINSOCK_H
#define XE_WINSOCK_H

#include "kernel.h"

/* --- what Winsock on the console looks like ----------------------------------- */

#define WS_INVALID_SOCKET   0xFFFFFFFFu
#define WS_ERROR            (-1)

#define WS_AF_INET          2
#define WS_SOCK_STREAM      1
#define WS_SOCK_DGRAM       2
#define WS_IPPROTO_IP       0
#define WS_IPPROTO_TCP      6
#define WS_IPPROTO_UDP      17

#define WS_SOL_SOCKET       0xFFFF

#define WS_MSG_OOB          0x1
#define WS_MSG_PEEK         0x2
#define WS_MSG_DONTROUTE    0x4
#define WS_MSG_WAITALL      0x8

#define WS_FD_SETSIZE       64

typedef struct {
    u16 family;
    u16 port;                   /* network order, which is this machine's */
    u32 address;
    u8  zero[8];
} WS_SOCKADDR_IN;

typedef struct {
    u32 count;
    u32 handle[WS_FD_SETSIZE];
} WS_FD_SET;

typedef struct {
    s32 sec, usec;
} WS_TIMEVAL;

typedef struct {
    u32 length;
    u8 *buffer;
} WS_BUF;

/* XNADDR, as XNetGetTitleXnAddr fills it. */
typedef struct {
    u32 ina;
    u32 ina_online;
    u16 port_online;
    u8  enet[6];
    u8  online[20];
} WS_XNADDR;

/* XNDNS, as XNetDnsLookup hands it out. */
typedef struct {
    s32 status;                 /* WSAEINPROGRESS, 0, or the reason it failed */
    u32 count;
    u32 address[8];
} WS_XNDNS;

/* --- Winsock's error codes ------------------------------------------------------ */

#define WSAEINTR            10004
#define WSAEBADF            10009
#define WSAEFAULT           10014
#define WSAEINVAL           10022
#define WSAEMFILE           10024
#define WSAEWOULDBLOCK      10035
#define WSAEINPROGRESS      10036
#define WSAEALREADY         10037
#define WSAENOTSOCK         10038
#define WSAEDESTADDRREQ     10039
#define WSAEMSGSIZE         10040
#define WSAEPROTOTYPE       10041
#define WSAENOPROTOOPT      10042
#define WSAEPROTONOSUPPORT  10043
#define WSAEOPNOTSUPP       10045
#define WSAEAFNOSUPPORT     10047
#define WSAEADDRINUSE       10048
#define WSAEADDRNOTAVAIL    10049
#define WSAENETDOWN         10050
#define WSAENETUNREACH      10051
#define WSAECONNABORTED     10053
#define WSAECONNRESET       10054
#define WSAENOBUFS          10055
#define WSAEISCONN          10056
#define WSAENOTCONN         10057
#define WSAESHUTDOWN        10058
#define WSAETIMEDOUT        10060
#define WSAECONNREFUSED     10061
#define WSAEHOSTUNREACH     10065
#define WSANOTINITIALISED   10093
#define WSAHOST_NOT_FOUND   11001

/* --- the layer's own ------------------------------------------------------------- */

typedef struct {
    u32 hooked;                 /* exports patched */
    u32 chained;                /* exports hooked before us (DashLaunch's DNS): their way on is ours */
    u32 refused;                /* exports left alone: too short to patch, or not found */
    u32 refused_ordinal;        /* the last of those */
    u32 calls;                  /* socket calls served */
    u32 sockets, sockets_most;  /* open now, at most */
    u32 not_ours;               /* a handle that is no socket of ours (made before the hooks) */
    u32 unsupported;            /* calls answered WSAEOPNOTSUPP */
    u32 waited;                 /* calls that waited for the stack to come up */
    u32 lookups, lookups_failed;
    u32 failed;                 /* errors set; the last WS_RECENT of them below */
    u32 recent[8][3];           /* where (the code that set it), the Winsock code, calls then */
    u32 option_refused;         /* the last setsockopt refused: level << 16 | option */
    u32 not_ours_from, not_ours_handle;     /* the last of those: which call, which handle */
    u32 by_caller[16][2];       /* calls by where in this layer they came from: address, count */
    u32 option_failed;          /* the last option lwIP refused: level << 16 | option */
    u32 select_ready[3];        /* handles select found ready: read, write, except */
    u32 select_poll, select_forever, select_timed;  /* by time-out: zero, none, some */
    u32 binds;
    u32 bound[8];               /* the last ports bound: TCP 1 << 16 | port, UDP 2 << 16 */
} WS_STATS;

#define WS_RECENT   8u

extern volatile WS_STATS ws_stats;

/* hook.c: patch xam. DllMain, before any other plugin opens a socket. */
void ws_hook(void);

/* handles.c */
s32  ws_fd(u32 handle);                         /* lwIP's descriptor, or -1 (WSAENOTSOCK set) */
u32  ws_handle(s32 fd, u32 caller);             /* a new socket's handle */
void ws_forget(s32 fd);
void ws_close_caller(u32 caller);               /* WSACleanup: every socket of that caller */

/* errors.c: the failure of the lwIP call just made, or a code of our own; WS_ERROR. */
s32  ws_fail_errno(void);
s32  ws_fail(s32 wsa);
void ws_set_error(s32 wsa);

/* errors.c: lwIP's errno of this thread (cc.h makes `errno` this), and WSAGetLastError. */
int *ipstack_errno(void);
s32  ws_WSAGetLastError(void);

/* calls.c: 0 when the stack is up, else WSAENETDOWN set (it waits a while first). */
u32  ws_ready(void);

/* overlap.c */
s32  ws_overlapped_recv(u32 s, s32 fd, WS_BUF *bufs, u32 count, u32 *received, void *ov);
s32  ws_overlapped_send(u32 s, s32 fd, const WS_BUF *bufs, u32 count, u32 *sent, void *ov);
void ws_write_blocked(u32 s);
void ws_overlap_forget(u32 s);

/* addr.c */
void ws_to_lwip(const WS_SOCKADDR_IN *from, void *to);
void ws_from_lwip(const void *from, WS_SOCKADDR_IN *to);

#endif /* XE_WINSOCK_H */
