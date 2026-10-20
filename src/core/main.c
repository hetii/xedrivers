/* xedrivers: a USB Ethernet adapter as the Xbox 360's NIC -- whichever driver in
 * src/drivers/ takes it (the Realtek RTL8156 USB 2.5G first among them).
 *
 * DllMain registers the USB driver object and starts the worker; nothing else, as
 * allocating from the pool there hangs the boot (xerunner: dllmain-pool-allocation@1).
 * The worker does everything that has to wait:
 *
 *   - the card was enumerated at boot, before this plugin existed, so the kernel
 *     will not offer it to us until it re-enumerates: find it on the hubs and
 *     reset it (xerunner: plugin-load-vs-enumeration@1)
 *   - on every AddDevice, bring it up with its driver (net.c) and apply xe_config
 *   - take orders from the debug channel
 *
 * The debug channel is one command word the worker polls, written from outside
 * with XBDM `setmem` (xerunner: plugin-debug-channel-f3@1); `xe_state`, `xe_config`,
 * `usb_stats`, `net_stats`, `stack_stats`, `xnet_stats` and each driver's own
 * stats are read back the same
 * way. Their addresses come from the linked ELF (build/plugin.elf), fixed because
 * the image is linked at a fixed base with no relocations
 * (xerunner: plugin-debug-channel-f1@1). tests/xectl.py wraps all of it.
 */

#include "kernel.h"
#include "config.h"
#include "usb.h"
#include "net.h"
#include "xnet.h"
#include "bench.h"

#define XE_VERSION 0x00020000u

#define CMD_PING       1u   /* answers 'PING' */
#define CMD_RESET      2u   /* UsbdResetDevice on the attached card */
#define CMD_CLAIM      3u   /* find the card on the hubs and reset it */
#define CMD_MODE       5u   /* [1] XE_MODE_* */
#define CMD_RESTART    6u   /* run the bring-up again */
/* 4 and 14..17 belong to the bench (src/bench/bench.c, `make bench=...`), 7..11 to
 * the driver of the card (rtl8156: advertise, registers, PHY); any number the core
 * does not know is offered to the bench, then to the driver. */
#define CMD_XNET_IFACE 13u  /* [1] interface id given to xnet's receive callback */
typedef struct {
    u32 magic;              /* 'XEDR' once DllMain ran */
    u32 version;
    u32 worker_ticks;
    u32 claim_attempts;
    u32 claim_found;
    u32 start_result;
    u32 start_count;
    u32 mode;               /* the mode actually in force */
    u32 xnet_status;
} XE_STATE;

volatile XE_STATE xe_state = { 0, XE_VERSION };
volatile XE_CONFIG xe_config = { 0x58434647u, XE_MODE, XE_ADVERTISE, XE_IP,
                                   XE_TRANSPORT };

/*   [0] request, cleared by the worker   [1..3] arguments
 *   [4] ack counter   [5] result   [6] the request answered   [7] status */
volatile u32 xe_cmd[8] = { 0 };

static u32 seen_generation;

static void no_frames(const u8 *frame, u32 length)
{
    (void)frame;
    (void)length;
}

/* Switch who consumes frames. xnet needs a card that is up; until it is, the mode
 * is remembered in xe_config and applied by the next bring-up. */
static u32 set_mode(u32 mode)
{
    NET_RX receiver;

    xe_config.mode = mode;
    if (mode == XE_MODE_XNET) {
        if (!net_stats.ready) {
            net_rx = no_frames;
            xe_state.mode = XE_MODE_DROP;
            return STATUS_UNSUCCESSFUL;
        }
        xe_state.xnet_status = xnet_enable();
        if (xe_state.xnet_status) {
            net_rx = no_frames;
            xe_state.mode = XE_MODE_DROP;
            return xe_state.xnet_status;
        }
        xe_state.mode = XE_MODE_XNET;
        return 0;
    }

    xnet_disable();
    net_link_cb = 0;
    receiver = bench_mode(mode);
    net_rx = receiver ? receiver : no_frames;
    xe_state.mode = receiver ? mode : XE_MODE_DROP;
    return 0;
}

static u32 run(u32 what)
{
    switch (what) {
    case CMD_PING:
        return 0x50494E47u;
    case CMD_RESET:
        return usb.active ? (u32)UsbdResetDevice(usb.device) : STATUS_UNSUCCESSFUL;
    case CMD_CLAIM:
        return usb_claim_present();
    case CMD_MODE:
        return set_mode(xe_cmd[1]);
    case CMD_RESTART:
        seen_generation = 0;
        return 0;
    case CMD_XNET_IFACE:
        xnet_rx_iface = xe_cmd[1];
        return 0;
    default:
        if (bench_command(what, xe_cmd) || net_command(what, xe_cmd))
            return xe_cmd[5];
        return 0xFFFFFFFFu;
    }
}

static void bring_up(void)
{
    xe_state.start_count++;
    xe_state.start_result = net_start();
    if (xe_state.start_result)
        return;
    set_mode(xe_config.mode);
}

static void worker(void *parameter)
{
    u32 tick, what;

    (void)parameter;
    bench_init();
    set_mode(xe_config.mode == XE_MODE_XNET ? XE_MODE_DROP : xe_config.mode);
    xe_config.mode = XE_MODE;

    for (tick = 0;; tick++) {
        xe_state.worker_ticks = tick;
        bench_tick(tick);

        /* Ticks are 20 ms: the first claim one second in, then every five while
         * nothing has been offered, six tries in all. */
        if (!usb.active && xe_state.claim_attempts < 6 &&
            (tick == 50 || (tick > 50 && tick % 250 == 0))) {
            xe_state.claim_attempts++;
            if (usb_claim_present() == 1)
                xe_state.claim_found++;
        }

        if (usb.active && usb.generation != seen_generation) {
            seen_generation = usb.generation;
            bring_up();
        }

        what = xe_cmd[0];
        if (what) {
            xe_cmd[0] = 0;
            xe_cmd[7] = 0;
            xe_cmd[5] = run(what);
            xe_cmd[6] = what;
            xe_cmd[4] = xe_cmd[4] + 1;
        }
        usb_sleep(20);
    }
}

int DllMain(void *instance, u32 reason, void *reserved)
{
    void *thread = 0;

    (void)instance;
    (void)reserved;
    if (reason != 1)                            /* DLL_PROCESS_ATTACH */
        return 1;

    xe_state.magic = 0x58454452u;               /* 'XEDR' */
    net_register();
    ExCreateThread(&thread, 0, 0, 0, (void *)worker, 0, 2);
    return 1;
}
