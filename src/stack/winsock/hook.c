/* xam's socket exports, pointed here. Each export's address comes from the kernel
 * (XexGetProcedureAddress, by ordinal: the ordinals are xam's, kit/ordinals-xam-*.txt),
 * and its first four instructions become `lis r11,hi; addi r11,r11,lo; mtctr r11; bctr` to
 * an entry here -- the way DashLaunch hooks xam (dll/utility.c patchInJump). Every caller,
 * xam's own code included, then arrives here; nothing jumps back, xnet's bodies are not
 * run again. The entries are USB_ENTRY stubs: the callers are the console's code, which
 * keeps 64-bit values in r14-r31 that a 32-bit gcc function would only half preserve
 * (usb.c). An export whose first three instructions hold a branch that does not come back
 * (b, blr, bctr) may end before the fourth -- xam 17559's WSAGetLastError is `b` and a word
 * of padding, and four words there wrote over WSASetLastError and what followed: the
 * console never came up -- so it is left alone and counted. Those two are not needed
 * (errors.c).
 *
 * An export that already starts with such a jump was hooked before us: DashLaunch hooks
 * XNetDnsLookup to refuse Microsoft's names (`liveblock`), and calls the original through
 * a copy of its first four instructions followed by a jump back to the export's fifth.
 * That copy is found (the jump back is unmistakable) and pointed here instead: what
 * DashLaunch lets through comes to lookup.c, what it refuses stays refused.
 *
 * Done once, from DllMain: the plugin has to load before the programs that open sockets
 * (launch.ini plugin1), or their sockets are xnet's and unknown here (ws_stats.not_ours).
 */
#include "winsock.h"
#include "usb.h"

#if XE_STACK

extern s32 XexGetModuleHandle(const char *name, void **handle);
extern s32 XexGetProcedureAddress(void *handle, u32 ordinal, void **address);

/* What calls.c, select.c, options.c, errors.c, addr.c and lookup.c serve. */
#define CALLS(X)                                                                    \
    X(1,  ws_WSAStartup)       X(2,  ws_WSACleanup)       X(3,  ws_socket)           \
    X(4,  ws_closesocket)      X(5,  ws_shutdown)         X(6,  ws_ioctlsocket)      \
    X(7,  ws_setsockopt)       X(8,  ws_getsockopt)       X(9,  ws_getsockname)      \
    X(10, ws_getpeername)      X(11, ws_bind)             X(12, ws_connect)          \
    X(13, ws_listen)           X(14, ws_accept)           X(15, ws_select)           \
    X(16, ws_unsupported)      X(17, ws_unsupported)      X(18, ws_recv)             \
    X(19, ws_WSARecv)          X(20, ws_recvfrom)         X(21, ws_unsupported)      \
    X(22, ws_send)             X(23, ws_WSASend)          X(24, ws_sendto)           \
    X(25, ws_unsupported)      X(35, ws_unsupported)      X(36, ws_WSAStartupEx)     \
    X(51, ws_XNetStartup)                                                             \
    X(52, ws_XNetCleanup)      X(67, ws_XNetDnsLookup)    X(68, ws_XNetDnsRelease)   \
    X(73, ws_XNetGetTitleXnAddr) X(75, ws_XNetGetEthernetLinkStatus)                 \
    X(80, ws_XNetStartupEx)

/* One stub a function, however many ordinals lead to it. */
#define FUNCTIONS(X)                                                                \
    X(ws_WSAStartup) X(ws_WSACleanup) X(ws_socket) X(ws_closesocket) X(ws_shutdown)   \
    X(ws_ioctlsocket) X(ws_setsockopt) X(ws_getsockopt) X(ws_getsockname)             \
    X(ws_getpeername) X(ws_bind) X(ws_connect) X(ws_listen) X(ws_accept) X(ws_select) \
    X(ws_unsupported) X(ws_recv) X(ws_WSARecv) X(ws_recvfrom) X(ws_send)              \
    X(ws_WSASend) X(ws_sendto)                                                         \
    X(ws_WSAStartupEx) X(ws_XNetStartup) X(ws_XNetCleanup) X(ws_XNetDnsLookup)        \
    X(ws_XNetDnsRelease) X(ws_XNetGetTitleXnAddr) X(ws_XNetGetEthernetLinkStatus)     \
    X(ws_XNetStartupEx)

#define STUB(fn) USB_ENTRY(fn##_entry, fn); void fn##_entry(void);
FUNCTIONS(STUB)

static const struct {
    u32 ordinal;
    void (*entry)(void);
} calls[] = {
#define ROW(ordinal, fn) { ordinal, fn##_entry },
    CALLS(ROW)
};

static u32 ends(u32 w)
{
    return w == 0x4E800020u || w == 0x4E800420u ||             /* blr, bctr */
           ((w >> 26) == 18 && !(w & 3));                       /* b */
}

/* Where a `lis r11; addi r11; mtctr r11; bctr` at `at` leads, or 0. */
static u32 jumps_to(const volatile u32 *at)
{
    if ((at[0] & 0xFFFF0000u) != 0x3D600000u || (at[1] & 0xFFFF0000u) != 0x396B0000u ||
        at[2] != 0x7D6903A6u || at[3] != 0x4E800420u)
        return 0;
    return ((at[0] & 0xFFFFu) << 16) + (u32)(s32)(s16)(at[1] & 0xFFFFu);
}

/* The hooker's copy of an export's first four instructions, as DashLaunch builds it
 * (dll/utility.c hookFunctionStart): `lis r11; addi r11` to the export's fifth instruction,
 * `mtctr r11`, the four instructions, `bctr` -- somewhere near the hooker's own code. */
static volatile u32 *copy_of(u32 export, u32 near)
{
    u32 from = (near & ~0xFFFu) - 0x40000u, to = from + 0x80000u, a, back = export + 16;

    for (a = from; a < to; a += 4) {
        volatile u32 *p = (volatile u32 *)a;

        if ((a & 0xFFFu) == 0 && (!MmIsAddressValid((void *)a) ||
                                  !MmIsAddressValid((void *)(a + 0xFFF + 32)))) {
            a += 0xFFC;
            continue;
        }
        if ((p[0] & 0xFFFF0000u) == 0x3D600000u && (p[1] & 0xFFFF0000u) == 0x396B0000u &&
            p[2] == 0x7D6903A6u && p[7] == 0x4E800420u &&
            ((p[0] & 0xFFFFu) << 16) + (u32)(s32)(s16)(p[1] & 0xFFFFu) == back)
            return p;
    }
    return 0;
}

static void jump(volatile u32 *at, u32 to)
{
    at[0] = 0x3D600000u | (((to + 0x8000u) >> 16) & 0xFFFFu);  /* lis   r11,hi    */
    at[1] = 0x396B0000u | (to & 0xFFFFu);                      /* addi  r11,r11,lo */
    at[2] = 0x7D6903A6u;                                       /* mtctr r11       */
    at[3] = 0x4E800420u;                                       /* bctr            */
    KeSweepIcacheRange((void *)at, 16);
}

void ws_hook(void)
{
    void *xam = 0, *address;
    u32 i, k, hooker;

    /* Microsoft's code, called with a frame that has its home area (usb.h) */
    if ((s32)usb_framed_call((void *)XexGetModuleHandle, (u32)"xam.xex", (u32)&xam) < 0 || !xam)
        return;
    for (i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        volatile u32 *at;

        address = 0;
        if ((s32)usb_framed_call((void *)XexGetProcedureAddress, (u32)xam, calls[i].ordinal,
                                 (u32)&address) < 0 || !address ||
            !MmIsAddressValid(address) || !MmIsAddressValid((u8 *)address + 15)) {
            ws_stats.refused++;
            ws_stats.refused_ordinal = calls[i].ordinal;
            continue;
        }
        at = (volatile u32 *)address;
        hooker = jumps_to(at);
        if (hooker) {
            volatile u32 *copy = copy_of((u32)address, hooker);

            if (!copy) {
                ws_stats.refused++;
                ws_stats.refused_ordinal = calls[i].ordinal;
                continue;
            }
            jump(copy, (u32)calls[i].entry);    /* the hooker's way on is ours */
            ws_stats.chained++;
            continue;
        }
        for (k = 0; k < 3; k++)
            if (ends(at[k]))
                break;
        if (k < 3) {
            ws_stats.refused++;
            ws_stats.refused_ordinal = calls[i].ordinal;
            continue;
        }
        jump(at, (u32)calls[i].entry);
        ws_stats.hooked++;
    }
}

#endif /* XE_STACK */
