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
 * (xerunner: plugin-debug-channel-f1@1). tools/xectl.py wraps all of it.
 */

#include "kernel.h"
#include "config.h"
#include "usb.h"
#include "file.h"
#include "net.h"
#include "bench.h"

#define XE_VERSION 0x00020000u

#define CMD_PING       1u   /* answers 'PING' */
#define CMD_RESET      2u   /* UsbdResetDevice on the attached card */
#define CMD_CLAIM      3u   /* find the card on the hubs and reset it */
#define CMD_RESTART    6u   /* run the bring-up again */
/* 5 and 13 belong to the network layer (src/net/net.c), 4 and 14..17 to the bench
 * (src/bench/bench.c, `make bench=...`), 7..11 to the driver of the card (rtl8156:
 * advertise, registers, PHY); any number the core does not know is offered to the
 * bench, then to the layer, which asks the driver. */
typedef struct {
    u32 magic;              /* 'XEDR' once DllMain ran */
    u32 version;
    u32 worker_ticks;
    u32 claim_attempts;
    u32 claim_found;
    u32 build_commit;       /* the first 8 hex digits of the commit built from */
    u32 build_dirty;        /* 1: tracked files differed from that commit */
} XE_STATE;

#ifndef XE_BUILD_COMMIT
#define XE_BUILD_COMMIT 0u
#define XE_BUILD_DIRTY  1u
#endif
volatile XE_STATE xe_state = { 0, XE_VERSION, 0, 0, 0, XE_BUILD_COMMIT, XE_BUILD_DIRTY };
volatile XE_CONFIG xe_config = { 0x58434647u, XE_MODE, XE_ADVERTISE, XE_IP,
                                   XE_TRANSPORT };

/*   [0] request, cleared by the worker   [1..3] arguments
 *   [4] ack counter   [5] result   [6] the request answered   [7] status */
volatile u32 xe_cmd[8] = { 0 };

static u32 seen_generation[USB_MAX_DEVICES];

/* Command 2: UsbdResetDevice on slot [1] (0 when only one device is owned). */
static u32 reset_device(u32 slot)
{
    USB_DEV *dev = slot < USB_MAX_DEVICES ? &usb_devs[slot] : 0;

    return dev && dev->active ? (u32)UsbdResetDevice(dev->device)
                              : STATUS_UNSUCCESSFUL;
}

static u32 run(u32 what)
{
    u32 i;

    switch (what) {
    case CMD_PING:
        return 0x50494E47u;
    case CMD_RESET:
        return reset_device(xe_cmd[1]);
    case CMD_CLAIM:
        return usb_claim_present();
    case CMD_RESTART:
        for (i = 0; i < USB_MAX_DEVICES; i++)
            seen_generation[i] = 0;
        return 0;
    default:
        if (bench_command(what, xe_cmd) || net_command(what, xe_cmd))
            return xe_cmd[5];
        return 0xFFFFFFFFu;
    }
}

/* The worker runs on hardware thread WORKER_PROCESSOR, not where ExCreateThread
 * put it -- hardware thread 2, the USB processor. Once the dashboard dims the
 * screen after about ten idle minutes, a thread waiting there is no longer woken
 * until a pad is touched: not by its timeout, not by its event being set (measured:
 * the worker stopped at the same tick on every idle boot, with
 * KeDelayExecutionThread and with KeWaitForSingleObject alike, while frames kept
 * flowing; on thread 5 it went on past the dim). Why the dashboard does that to
 * thread 2 is not known. USB requests from here go to thread 2 by DPC
 * (usb_submit), as they always did from a thread. */
#define WORKER_PROCESSOR 5

/* (thread, affinity mask, &previous): ordinal 151; the current thread is the word
 * at PCR+0x100. */
extern u32 KeSetAffinityThread(void *thread, u32 affinity, u32 *previous);

static void run_on(u32 processor)
{
    void *self;
    u32 previous;

#if defined(__powerpc__)
    __asm__ volatile("lwz %0, 0x100(13)" : "=r"(self));
#else
    self = 0;
#endif
    KeSetAffinityThread(self, 1u << processor, &previous);
}

static void away_from_the_usb_processor(void)
{
    run_on(WORKER_PROCESSOR);
}

/* Whether any device is owned: the boot claims stop once one is (the others that
 * enumerated before us are reset by the same claim, see usb_claim_present). */
static u32 owned(void)
{
    u32 i;

    for (i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devs[i].active)
            return 1;
    return 0;
}

static void worker(void *parameter)
{
    u32 tick, what, i;

    (void)parameter;
    away_from_the_usb_processor();
    bench_init();
    net_learn_console_mac();            /* before any device is brought up */
    net_layer_init();

    for (tick = 0;; tick++) {
        xe_state.worker_ticks = tick;
        bench_tick(tick);

        /* Ticks are 20 ms: the first claim one second in, then every five while
         * nothing has been offered, six tries in all. */
        if (!owned() && xe_state.claim_attempts < 6 &&
            (tick == 50 || (tick > 50 && tick % 250 == 0))) {
            xe_state.claim_attempts++;
            if (usb_claim_present())
                xe_state.claim_found++;
        }

        for (i = 0; i < USB_MAX_DEVICES; i++) {
            USB_DEV *dev = &usb_devs[i];

            if (dev->active && dev->generation != seen_generation[i]) {
                seen_generation[i] = dev->generation;
                /* A bring-up is thousands of synchronous transfers in a row (the
                 * firmware, the efuse map): from thread 5 each is a DPC sent to
                 * thread 2 and a wake sent back, and about one boot in twenty
                 * both threads stopped there for good, every other one running
                 * on -- crossed interprocessor requests, as it looks (thread 5
                 * was last seen inside KeInsertQueueDpc). On the USB processor
                 * both stay local; the dim (above) does not come into a
                 * bring-up's seconds. The scan's channel changes later are the
                 * same kind of burst: usb_spin keeps the wake from being sent.
                 * 2026-10-29: 0 stops in 60 boots, from 5 in about 100. */
                run_on(USB_PROCESSOR);
                net_bring_up(dev);
                away_from_the_usb_processor();
            }
        }
        net_tick();

        what = xe_cmd[0];
        if (what) {
            xe_cmd[0] = 0;
            xe_cmd[7] = 0;
            xe_cmd[5] = run(what);
            xe_cmd[6] = what;
            xe_cmd[4] = xe_cmd[4] + 1;
        }
        usb_wait(20);
    }
}

int DllMain(void *instance, u32 reason, void *reserved)
{
    void *thread = 0;

    (void)reserved;
    if (reason != 1)                            /* DLL_PROCESS_ATTACH */
        return 1;

    xe_state.magic = 0x58454452u;               /* 'XEDR' */
    file_set_plugin(instance);
    net_register();
    ExCreateThread(&thread, 0, 0, 0, (void *)worker, 0, 2);
    return 1;
}
