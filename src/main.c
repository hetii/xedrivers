/* xedrivers: a Realtek RTL8156 USB 2.5G Ethernet adapter as the Xbox 360's NIC.
 *
 * DllMain registers the USB driver object and starts the worker; nothing else, as
 * allocating from the pool there hangs the boot (xerunner: dllmain-pool-allocation@1).
 * The worker does everything that has to wait:
 *
 *   - the card was enumerated at boot, before this plugin existed, so the kernel
 *     will not offer it to us until it re-enumerates: find it on the root hubs and
 *     reset it (xerunner: plugin-load-vs-enumeration@1)
 *   - on every AddDevice, bring it up (net.c)
 *   - take orders from the debug channel
 *
 * The debug channel is one command word the worker polls, written from outside
 * with XBDM `setmem` (xerunner: plugin-debug-channel-f3@1); `xe_state`, `usb_stats`,
 * `net_stats` and `stack_stats` are read back the same way. Their addresses come
 * from the linked ELF (build/plugin.elf), fixed because the image is linked at a
 * fixed base with no relocations (xerunner: plugin-debug-channel-f1@1).
 */

#include "kernel.h"
#include "usb.h"
#include "net.h"
#include "stack.h"
#include "xnet.h"

#define XE_VERSION 0x00010000u

#define CMD_PING     1u     /* answers 'PING' */
#define CMD_RESET    2u     /* UsbdResetDevice on the attached card */
#define CMD_CLAIM    3u     /* find the card on the hubs and reset it */
#define CMD_BLAST    4u     /* [1] count, [2] payload bytes, [3] UDP port */
#define CMD_MODE     5u     /* [1] 0 = drop, 1 = standalone stack, 2 = xnet */
#define CMD_RESTART  6u     /* run the bring-up again */

#define MODE_DROP       0u
#define MODE_STANDALONE 1u
#define MODE_XNET       2u

/* Standalone by default: the first boot brings the card up and answers on
 * 10.36.0.2 without touching xam, so the USB path is proven before the xnet hooks
 * are armed. `CMD_MODE 2` switches to xnet; -DXE_DEFAULT_MODE=2 bakes it in. */
#ifndef XE_DEFAULT_MODE
#define XE_DEFAULT_MODE MODE_STANDALONE
#endif

typedef struct {
    u32 magic;              /* 'XEAI' once DllMain ran */
    u32 version;
    u32 worker_ticks;
    u32 claim_attempts;
    u32 claim_found;
    u32 start_result;
    u32 start_count;
    u32 mode;
    u32 xnet_status;
} XE_STATE;

volatile XE_STATE xe_state = { 0, XE_VERSION };

/*   [0] request, cleared by the worker   [1..3] arguments
 *   [4] ack counter                      [5] result   [6] the request answered */
volatile u32 xe_cmd[8] = { 0 };

static u32 seen_generation;

static void no_frames(const u8 *frame, u32 length)
{
    (void)frame;
    (void)length;
}

static u32 set_mode(u32 mode)
{
    if (mode == MODE_XNET) {
        xe_state.xnet_status = xnet_enable();
        if (xe_state.xnet_status) {
            net_rx = no_frames;
            xe_state.mode = MODE_DROP;
            return xe_state.xnet_status;
        }
        xe_state.mode = MODE_XNET;
        return 0;
    }

    xnet_disable();
    net_link_cb = 0;
    net_rx = mode == MODE_STANDALONE ? stack_input : no_frames;
    xe_state.mode = mode == MODE_STANDALONE ? MODE_STANDALONE : MODE_DROP;
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
    case CMD_BLAST:
        return stack_blast(xe_cmd[1], xe_cmd[2], xe_cmd[3]);
    case CMD_MODE:
        return set_mode(xe_cmd[1]);
    case CMD_RESTART:
        seen_generation = 0;
        return 0;
    default:
        return 0xFFFFFFFFu;
    }
}

static void worker(void *parameter)
{
    u32 tick, what;

    (void)parameter;
    set_mode(XE_DEFAULT_MODE == MODE_XNET ? MODE_DROP : XE_DEFAULT_MODE);

    for (tick = 0;; tick++) {
        xe_state.worker_ticks = tick;

        /* Ticks are 20 ms: the first claim one second in, then every five while
         * nothing has been offered, six tries in all. */
        if (!usb.active && xe_state.claim_attempts < 6 &&
            (tick == 50 || (tick > 50 && tick % 250 == 0))) {
            xe_state.claim_attempts++;
            if (usb_claim_present())
                xe_state.claim_found++;
        }

        if (usb.active && usb.generation != seen_generation) {
            seen_generation = usb.generation;
            xe_state.start_count++;
            xe_state.start_result = net_start();
            if (!xe_state.start_result && XE_DEFAULT_MODE == MODE_XNET &&
                xe_state.mode == MODE_DROP && !xe_state.xnet_status)
                set_mode(MODE_XNET);
        }

        what = xe_cmd[0];
        if (what) {
            xe_cmd[0] = 0;
            xe_cmd[5] = run(what);
            xe_cmd[6] = what;
            xe_cmd[4] = xe_cmd[4] + 1;
        }
        usb_sleep(20);
    }
}

static void attached(void)
{
    /* The worker notices the new generation and brings the card up. */
}

static void detached(void)
{
    net_detached();
}

static const USB_DRIVER rtl8156 = { 0x0BDA, 0x8156, attached, detached };

int DllMain(void *instance, u32 reason, void *reserved)
{
    void *thread = 0;

    (void)instance;
    (void)reserved;
    if (reason != 1)                            /* DLL_PROCESS_ATTACH */
        return 1;

    xe_state.magic = 0x58454149u;               /* 'XEAI' */
    usb_register(&rtl8156);
    ExCreateThread(&thread, 0, 0, 0, (void *)worker, 0, 2);
    return 1;
}
