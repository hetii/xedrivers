/* xedrivers: a Realtek RTL8156 USB 2.5G Ethernet adapter as the Xbox 360's NIC.
 *
 * DllMain registers the USB driver object and starts the worker; nothing else, as
 * allocating from the pool there hangs the boot (xerunner: dllmain-pool-allocation@1).
 * The worker does everything that has to wait:
 *
 *   - the card was enumerated at boot, before this plugin existed, so the kernel
 *     will not offer it to us until it re-enumerates: find it on the root hubs and
 *     reset it (xerunner: plugin-load-vs-enumeration@1)
 *   - on every AddDevice, bring it up (net.c) and apply xe_config
 *   - take orders from the debug channel
 *
 * The debug channel is one command word the worker polls, written from outside
 * with XBDM `setmem` (xerunner: plugin-debug-channel-f3@1); `xe_state`, `xe_config`,
 * `usb_stats`, `net_stats`, `stack_stats` and `xnet_stats` are read back the same
 * way. Their addresses come from the linked ELF (build/plugin.elf), fixed because
 * the image is linked at a fixed base with no relocations
 * (xerunner: plugin-debug-channel-f1@1). tests/xectl.py wraps all of it.
 */

#include "kernel.h"
#include "config.h"
#include "usb.h"
#include "net.h"
#include "rtl.h"
#include "stack.h"
#include "xnet.h"

#define XE_VERSION 0x00020000u

#define CMD_PING       1u   /* answers 'PING' */
#define CMD_RESET      2u   /* UsbdResetDevice on the attached card */
#define CMD_CLAIM      3u   /* find the card on the hubs and reset it */
#define CMD_BLAST      4u   /* [1] count, [2] payload bytes, [3] UDP port */
#define CMD_MODE       5u   /* [1] XE_MODE_* */
#define CMD_RESTART    6u   /* run the bring-up again */
#define CMD_ADVERTISE  7u   /* [1] RTL_ADV_* bits; renegotiates now */
#define CMD_REG_READ   8u   /* [1] mcu, [2] address -> [5] value, [7] status */
#define CMD_REG_WRITE  9u   /* [1] mcu, [2] address, [3] word */
#define CMD_PHY_READ  10u   /* [1] OCP address -> [5] value, [7] status */
#define CMD_PHY_WRITE 11u   /* [1] OCP address, [2] value */

typedef struct {
    u32 magic;              /* 'XEAI' once DllMain ran */
    u32 version;
    u32 worker_ticks;
    u32 claim_attempts;
    u32 claim_found;
    u32 start_result;
    u32 start_count;
    u32 mode;               /* the mode actually in force */
    u32 xnet_status;
    u32 advertise_status;   /* of the last advertisement write */
    u32 chip_version;       /* PLA_TCR0 >> 16, e.g. 0x7710 for an RTL8156B */
} XE_STATE;

volatile XE_STATE xe_state = { 0, XE_VERSION };
volatile XE_CONFIG xe_config = { 0x58434647u, XE_MODE, XE_ADVERTISE, XE_IP };

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
    stack_ip = xe_config.ip;
    net_rx = mode == XE_MODE_STANDALONE ? stack_input : no_frames;
    xe_state.mode = mode == XE_MODE_STANDALONE ? XE_MODE_STANDALONE : XE_MODE_DROP;
    return 0;
}

static u32 advertise(u32 bits)
{
    xe_config.advertise = bits;
    if (!net_stats.ready)
        return STATUS_UNSUCCESSFUL;
    xe_state.advertise_status = bits ? rtl_set_advertise(bits) : STATUS_SUCCESS;
    return xe_state.advertise_status;
}

static u32 run(u32 what)
{
    u32 value = 0;

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
    case CMD_ADVERTISE:
        return advertise(xe_cmd[1]);
    case CMD_REG_READ:
        xe_cmd[7] = rtl_read_word(xe_cmd[1], xe_cmd[2], &value);
        return value;
    case CMD_REG_WRITE:
        return rtl_write_word(xe_cmd[1], xe_cmd[2], xe_cmd[3]);
    case CMD_PHY_READ:
        xe_cmd[7] = rtl_phy_read(xe_cmd[1], &value);
        return value;
    case CMD_PHY_WRITE:
        return rtl_phy_write(xe_cmd[1], xe_cmd[2]);
    default:
        return 0xFFFFFFFFu;
    }
}

static void bring_up(void)
{
    u32 tcr0 = 0;

    xe_state.start_count++;
    rtl_forget_page();
    xe_state.start_result = net_start();
    if (xe_state.start_result)
        return;

    if (NT_SUCCESS(rtl_read_dword(RTL_PLA, RTL_PLA_TCR0, &tcr0)))
        xe_state.chip_version = tcr0 >> 16;
    if (xe_config.advertise)
        advertise(xe_config.advertise);
    set_mode(xe_config.mode);
}

static void worker(void *parameter)
{
    u32 tick, what;

    (void)parameter;
    set_mode(xe_config.mode == XE_MODE_XNET ? XE_MODE_DROP : xe_config.mode);
    xe_config.mode = XE_MODE;

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
