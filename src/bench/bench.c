/* The tuning tools behind src/core/bench.h: the commands only they answer, the
 * worker's tick, the standalone mode. Built when `make bench=...` names any tool;
 * each tool is compiled in only if named (XE_BENCH_<TOOL>, XE_PROF, XE_TCP).
 */

#include "bench.h"
#include "config.h"
#include "prof.h"
#if XE_BENCH_HISTORY
#include "history.h"
#endif
#if XE_BENCH_STACK
#include "stack.h"
#endif
#if XE_BENCH_DEBUG
#include "debug.h"
#endif
#if XE_TCP
#include "tcp.h"
#endif

#define CMD_BLAST       4u  /* [1] count, [2] payload bytes, [3] UDP port (stack) */
#define CMD_TCP_START  14u  /* start the throughput server (tcp, xnet mode) */
#define CMD_THREAD_PRIO 15u /* [1] thread id (xerunner console list-threads), [2] prio */
#define CMD_PEEK       16u  /* [1] address, [2] 1 = little-endian (MMIO) -> word */
#define CMD_POKE       17u  /* [1] address, [2] 1 = little-endian, [3] word */
#define CMD_CONFIG     18u  /* [1] category, [2] setting -> size, bytes in
                             * debug_config_value */
#define CMD_CRYPTO     19u  /* the kernel's SHA-1, HMAC-SHA1 and AES, checked
                             * against known answers -> a bitmap, 7 for all */
#define CMD_OMNI_ON    21u  /* register as a wireless card that records what it is
                             * asked (debug_omni_log) */
#define CMD_OMNI_OFF   22u
#define CMD_WIRED_OFF  23u  /* [1] seconds without xnet, then back */
#define CMD_WPA        20u  /* WPA2's key derivation and group-key unwrapping,
                             * likewise -> a bitmap, 3 for both */

void bench_init(void)
{
    prof_init();
}

void bench_tick(u32 tick)
{
    PROF_STAMP();
    if (!(tick % 30))
        bench_trace(".");               /* the worker is alive */
#if XE_BENCH_HISTORY
    history_tick(tick);
#endif
#if XE_BENCH_DEBUG
    debug_tick(tick);
    debug_omni_tick();
#endif
    (void)tick;
}

u32 bench_command(u32 what, volatile u32 *args)
{
    switch (what) {
#if XE_BENCH_STACK
    case CMD_BLAST:
        args[5] = stack_blast(args[1], args[2], args[3]);
        return 1;
#endif
#if XE_BENCH_DEBUG
    case CMD_THREAD_PRIO:
        args[5] = debug_thread_priority(args[1], args[2]);
        return 1;
    case CMD_PEEK:
        args[5] = debug_peek(args[1], args[2]);
        return 1;
    case CMD_CONFIG:
        args[5] = debug_config(args[1], args[2]);
        return 1;
    case CMD_CRYPTO:
        args[5] = debug_crypto();
        return 1;
    case CMD_WPA:
        args[5] = debug_wpa();
        return 1;
    case CMD_OMNI_ON:
        args[5] = debug_omni_register();
        return 1;
    case CMD_OMNI_OFF:
        args[5] = debug_omni_unregister();
        return 1;
    case CMD_WIRED_OFF:
        args[5] = debug_omni_wired_off(args[1]);
        return 1;
    case CMD_POKE:
        args[5] = debug_poke(args[1], args[2], args[3]);
        return 1;
#endif
#if XE_TCP
    case CMD_TCP_START:
        args[5] = tcp_start();
        return 1;
#endif
    default:
        (void)args;
        return 0;
    }
}

u32 bench_wired_hidden(void)
{
#if XE_BENCH_DEBUG
    return debug_omni_wired_hidden;
#else
    return 0;
#endif
}

BENCH_RX bench_mode(u32 mode)
{
#if XE_BENCH_STACK
    if (mode == XE_MODE_STANDALONE) {
        stack_ip = xe_config.ip;
        return stack_input;
    }
#endif
    (void)mode;
    return 0;
}

void bench_frame_out(const u8 *frame, u32 length)
{
#if XE_TCP
    tcp_flight_out(frame, length);
#else
    (void)frame;
    (void)length;
#endif
}

void bench_frame_in(const u8 *frame, u32 length)
{
#if XE_TCP
    tcp_flight_in(frame, length);
#else
    (void)frame;
    (void)length;
#endif
}

/* The kernel UART: transmit data at 0x7FEA1014 (the character in the top byte),
 * status at 0x7FEA1018 (0x02000000: room to send), 115200 baud as the kernel set
 * it up. Read live over the plugin's peek and checked by sending "XEDR" to
 * `xerunner flash read-uart`. */
#define UART_DATA   ((volatile u32 *)0x7FEA1014u)
#define UART_STATUS ((volatile u32 *)0x7FEA1018u)

void bench_trace(const char *mark)
{
#if XE_BENCH_DEBUG
    u32 spin;

    for (; *mark; mark++) {
        for (spin = 0; spin < 20000 && !(*UART_STATUS & 0x02000000u); spin++)
            ;
        *UART_DATA = (u32)(u8)*mark << 24;
    }
#else
    (void)mark;
#endif
}

static volatile char where = '-';

void bench_where(char step)
{
    where = step;
}

void bench_where_mark(void)
{
    char m[2];

    m[0] = where;
    m[1] = 0;
    bench_trace(m);
}
