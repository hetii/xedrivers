/* The tuning tools behind src/core/bench.h: the commands only they answer, the
 * worker's tick, the standalone mode. Built when `make bench=...` names any tool;
 * each tool is compiled in only if named (XE_BENCH_<TOOL>, XE_PROF, XE_TCP).
 */

#include "bench.h"
#include "config.h"
#include "prof.h"
#if XE_BENCH_TRACE
#include "trace.h"
#endif
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
#define CMD_VOICE      26u  /* debug_voice: a microphone request as a title makes it */
#define CMD_MEMORY     25u  /* debug_memory: statistics, and what can be taken */
#define CMD_CONFIG     18u  /* [1] category, [2] setting -> size, bytes in
                             * debug_config_value */
#define CMD_CRYPTO     19u  /* the kernel's SHA-1, HMAC-SHA1 and AES, checked
                             * against known answers -> a bitmap, 7 for all */
#define CMD_OMNI_ON    21u  /* register as a wireless card that records what it is
                             * asked (debug_omni_log) */
#define CMD_OMNI_OFF   22u
#define CMD_PCI        24u  /* [1] bus<<16|device<<8|function, [2] register,
                             * [3] 0 read / 1 write debug_pci_value -> dword */
#define CMD_WIRED_OFF  23u  /* [1] seconds without xnet, then back */
#define CMD_WPA        20u  /* WPA2's key derivation and group-key unwrapping,
                             * likewise -> a bitmap, 3 for both */

void bench_init(void)
{
    prof_init();
}

void bench_tick(u32 tick)
{
    (void)tick;
    PROF_STAMP();
#if XE_BENCH_HISTORY
    history_tick(tick);
#endif
#if XE_BENCH_TRACE
    trace_tick(tick);
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
#if XE_AUDIO
    case CMD_VOICE:
        args[5] = debug_voice(args[1]);
        return 1;
#endif
    case CMD_PEEK:
        args[5] = debug_peek(args[1], args[2]);
        return 1;
    case CMD_PCI:
        args[5] = debug_pci(args[1], args[2], args[3]);
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
    case CMD_MEMORY:
        args[5] = debug_memory();
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

void bench_event(u32 code, u32 a, u32 b, u32 c)
{
#if XE_BENCH_TRACE
    trace_event(code, a, b, c);
#else
    (void)code; (void)a; (void)b; (void)c;
#endif
}

#if XE_BENCH_DEBUG
/* Set to 1 over XBDM and every frame of the console's sound becomes a triangle wave of
 * (c + 1) kHz at -12 dB in channel c (48 kHz: periods 48, 24, 16, 12, 9, 8 samples, all of
 * which divide 144). On the far side, which channel went where shows by frequency, and a
 * sample lost or repeated breaks the 144-sample period. */
volatile u32 audio_test_tone;
static u32 tone_phase;
#endif

void bench_audio_frame(float *frame)
{
#if XE_BENCH_DEBUG
    u32 c, i;

    if (!audio_test_tone)
        return;
    /* 2: the two front channels only (1 kHz left, 2 kHz right), as tools' tri.wav on a PC */
    for (c = 0; c < 6; c++) {
        u32 period = 48u / (c + 1);

        for (i = 0; i < 256; i++) {
            float x = (float)((tone_phase + i) % period) / (float)period;

            frame[c * 256 + i] = audio_test_tone == 2 && c >= 2 ? 0.0f :
                                 0.25f * (x < 0.5f ? 4.0f * x - 1.0f : 3.0f - 4.0f * x);
        }
    }
    tone_phase += 256;
#else
    (void)frame;
#endif
}

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

