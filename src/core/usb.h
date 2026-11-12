/* The USB devices we own -- up to USB_MAX_DEVICES, each for whichever driver
 * matches it -- and transfers on them.
 *
 * The kernel's USB stack expects UsbdQueueAsyncTransfer on hardware thread 2 at
 * DISPATCH_LEVEL (CUsbEnet: the checked kernel asserts it; the retail one merely
 * does not lock against other processors). usb_submit() therefore queues a request
 * directly when already there -- completion routines are -- and otherwise hands it
 * to a DPC targeted at that processor.
 */

#ifndef XE_USB_H
#define XE_USB_H

#include "kernel.h"

#define USB_TYPE_CONTROL   0
#define USB_TYPE_ISOCH     1
#define USB_TYPE_BULK      2
#define USB_TYPE_INTERRUPT 3

#define USB_MAX_DEVICES 4

/* One device we own. Everything that was the single device's is per device now:
 * the kernel's handles, the endpoints closed on removal, the control transfer the
 * bring-up waits for. `index` is the slot, `where` the node's controller/port byte
 * (bits 0-1 controller, 2-4 port), for telling sockets apart. */
typedef struct USB_DEV {
    void         *device;           /* the kernel's device node, while we own it */
    void         *control;          /* default endpoint */
    void         *ep[12];           /* opened by the driver, closed in reverse: a
                                     * Wi-Fi and Bluetooth combo needs nine with voice */
    u32           ep_count;
    volatile u32  active;           /* cleared the moment removal starts */
    volatile u32  generation;       /* grows with every AddDevice on this slot */
    u32           index;
    u32           where;
    void         *client;           /* the client's per-device state (net.c's) */
    URB           control_urb;
    volatile u32  control_done;
    volatile u32  control_status;
    URB           close_urb;
} USB_DEV;

/* A call into Microsoft's code from ours, with a frame that has the home area it may store
 * its arguments in (SP+0x10 to +0x4F, kit/kcall.S): `routine` gets up to seven words, and its
 * return comes back. See usb.c for the freeze that taught it. */
#if defined(__powerpc__)
u32 usb_framed_call(void *routine, ...);
#endif

/* Controllers 0 and 1 are EHCI, 2 and 3 their USB 1.1 companions (OHCI). */
#define USB_ON_COMPANION(dev) (((dev)->where & 3u) >= 2u)

extern USB_DEV usb_devs[USB_MAX_DEVICES];

/* Who decides which devices are ours and hears of their arrival and removal --
 * net.c, for every driver at once. `match` gets an interface descriptor (or 0 when
 * only the device is known) and the device descriptor, returns nonzero to claim;
 * `attached` and `detached` run inside AddDevice/RemoveDevice and must not block. */
typedef struct {
    u32  (*match)(const u8 *interface, const u8 *device);
    void (*attached)(USB_DEV *dev);
    void (*detached)(USB_DEV *dev);
    /* The device's last endpoint is closed and the kernel told: nothing of it completes
     * any more, and what was taken for it may go. DISPATCH_LEVEL; 0: not wanted. */
    void (*gone)(USB_DEV *dev);
} USB_CLIENT;

/* The entry for a routine the kernel calls at DISPATCH_LEVEL: why, in usb.c. `load` puts
 * the routine's address in r12; the arguments go through as they came. */
#if defined(__powerpc__)
#define USB_STUB(name, load)                                                        \
    __asm__(".section .text\n"                                                       \
            ".machine ppc64\n"                                                       \
            ".globl " name "\n"                                                      \
            name ":\n"                                                               \
            "    mflr    0\n"                                                        \
            "    stw     0, 4(1)\n"                                                  \
            "    stwu    1, -176(1)\n"                                               \
            "    std 14, 16(1)\n  std 15, 24(1)\n  std 16, 32(1)\n  std 17, 40(1)\n"  \
            "    std 18, 48(1)\n  std 19, 56(1)\n  std 20, 64(1)\n  std 21, 72(1)\n"  \
            "    std 22, 80(1)\n  std 23, 88(1)\n  std 24, 96(1)\n  std 25, 104(1)\n" \
            "    std 26, 112(1)\n std 27, 120(1)\n std 28, 128(1)\n std 29, 136(1)\n" \
            "    std 30, 144(1)\n std 31, 152(1)\n"                                  \
            "    " load "\n"                                                         \
            "    mtctr   12\n"                                                       \
            "    bctrl\n"                                                            \
            "    ld 14, 16(1)\n   ld 15, 24(1)\n   ld 16, 32(1)\n   ld 17, 40(1)\n"   \
            "    ld 18, 48(1)\n   ld 19, 56(1)\n   ld 20, 64(1)\n   ld 21, 72(1)\n"   \
            "    ld 22, 80(1)\n   ld 23, 88(1)\n   ld 24, 96(1)\n   ld 25, 104(1)\n"  \
            "    ld 26, 112(1)\n  ld 27, 120(1)\n  ld 28, 128(1)\n  ld 29, 136(1)\n"  \
            "    ld 30, 144(1)\n  ld 31, 152(1)\n"                                   \
            "    addi    1, 1, 176\n"                                                \
            "    lwz     0, 4(1)\n"                                                  \
            "    mtlr    0\n"                                                        \
            "    blr\n"                                                              \
            ".machine any\n")
#endif

/* A function of ours the kernel or a module calls, entered through such a stub: defines
 * `entry`, which calls `fn` (global, marked USB_ENTERED). Give the kernel `entry`, never `fn`. */
#if defined(__powerpc__)
#define USB_ENTERED __attribute__((used))
#define USB_ENTRY(entry, fn) USB_STUB(#entry, "lis 12, " #fn "@ha\n    addi 12, 12, " #fn "@l")
#else
#define USB_ENTERED
#endif

/* A DPC of ours, entered so that the thread it interrupts keeps its registers: see usb.c. */
#if defined(__powerpc__)
void usb_dpc_init(KDPC *dpc, PKDEFERRED_ROUTINE routine, void *context);
#else
static inline void usb_dpc_init(KDPC *dpc, PKDEFERRED_ROUTINE routine, void *context)
{
    KeInitializeDpc(dpc, routine, context);
}
#endif

#define USB_PROCESSOR 2             /* hardware thread the USB stack runs on */

int   usb_register(const USB_CLIENT *client);
void *usb_open(USB_DEV *dev, u32 type, u32 address, u32 max_packet, u32 interval);
void  usb_submit(URB *urb);

/* Queue an isochronous request on `dev`, its packets' lengths in `lengths`, which stays
 * ours; from any thread, like usb_submit. */
void  usb_isoch(USB_DEV *dev, ISOCH *request, u16 *lengths);

/* Thread-only: wait until `urb` is not the host controller's any more, up to
 * `timeout_ms`; 0 if it still is. A synchronous request that timed out stays
 * queued, and preparing it again (or writing its buffer) while the controller
 * holds it wrecks the controller's lists: every synchronous helper asks this
 * first, and its completion clears Busy. */
u32   usb_wait_idle(URB *urb, u32 timeout_ms);
void  usb_spin(volatile u32 *done);
void  usb_cancel(URB *urb);            /* the controller gives it back, cancelled */

/* A driver's ring of receive requests, watched (usb_rx_watch). Under heavy
 * sending this console's host controller can stop asking for a device's bulk IN
 * requests, the device holding data and no error ever coming; taking them back
 * (usb_cancel) and queuing them again -- which a ring that queues a request again
 * on any completion does by itself -- starts it again. Only if that does nothing
 * is the device to be reset. Ticks are the driver's own. */
typedef struct {
    URB *urbs;              /* the ring, `count` of them */
    u32 count;
    u32 kick_ticks;         /* ticks without progress before taking them back, 0 never */
    u32 reset_ticks;        /* and before a reset is due */
    u32 seen;               /* the progress count last time */
    u32 quiet;              /* ticks without progress */
    u32 kicked;             /* taken back since the last progress */
    volatile u32 *kicks;    /* the driver's counters, or 0 */
    volatile u32 *kick_ok;
} USB_RX_WATCH;

void  usb_rx_watch_init(USB_RX_WATCH *w, URB *urbs, u32 count, u32 kick_ticks,
                        u32 reset_ticks, volatile u32 *kicks, volatile u32 *kick_ok);

/* Once a tick: `watched` 0 when no traffic is to be expected (counters start
 * over); `progress` the driver's count of receive completions, which should
 * not move when the ring is not working (all parked, say). 1: reset the device. */
u32   usb_rx_watch(USB_RX_WATCH *w, u32 watched, u32 progress);    /* up to 1 ms for a transfer just submitted */

/* Fill a request for `dev`'s default endpoint or for `endpoint`. */
void  usb_prepare(URB *urb, USB_DEV *dev, void *endpoint, URB_COMPLETION done,
                  void *buffer, u32 length, const u8 *setup);

/* Thread-only: one control transfer on `dev`, waited for. Returns the completion
 * status, or STATUS_UNSUCCESSFUL on timeout; *actual gets the bytes moved.
 * `buffer` must be DMA memory. */
u32   usb_control(USB_DEV *dev, const u8 setup[8], void *buffer, u32 length,
                  u32 *actual, u32 timeout_ms);

/* Have the devices that enumerated before the plugin loaded offered again: every
 * node no driver has is reset, once each, so the kernel enumerates it again and
 * Match sees its real descriptor (boot enumeration is over before plugins load).
 * Returns how many were reset.
 * xerunner: plugin-load-vs-enumeration@1, xerunner: usb-driver-object-f19@1 */
u32   usb_claim_present(void);

void  usb_sleep(u32 ms);

/* 1: usb_sleep and usb_wait spin on the time base instead of waiting (the USB processor
 * during a bring-up, main.c; see usb.c). */
void  usb_spin_waits(u32 on);

/* Wait up to `ms`, or until usb_wake: the worker's and usb_control's wait, which a
 * dimmed dashboard does not stall. usb_wake may be called at DISPATCH_LEVEL. */
void  usb_wait(u32 ms);
void  usb_wake(void);

/* Have usb_wake set `event` too: another thread's wait that the dim must not stall (a
 * synchronization event, initialised; it stays registered). */
void  usb_wake_also(KEVENT *event);

/* usb_wake from any IRQL, above DISPATCH_LEVEL too: through a DPC. */
void  usb_wake_soon(void);

typedef struct {
    u32 match_calls;
    u32 add_calls;
    u32 remove_calls;
    u32 close_passes;
    u32 submit_direct;
    u32 submit_dpc;
    u32 dpc_runs;
    u32 last_open_status;
    u32 scan_found;
    u32 reset_status;
    u32 unbound_resets;     /* nodes no driver had, reset to be offered again */
    u32 no_slot;            /* devices refused: USB_MAX_DEVICES already owned */
    u32 control_late;       /* control requests refused: the last one still out */
    u32 cancels;            /* requests taken back (usb_cancel) */
    u32 nak_reload_set;     /* high-speed bulk endpoints given NAK_RELOAD (usb_open) */
    u32 nak_reload_skipped; /* bulk endpoints whose handle did not read as an EHCI QH */
    u32 spin_bring_ups;     /* bring-ups whose waits spun (usb_spin_waits) */
    u32 bring_ups_dimmed;   /* bring-ups left on the worker's processor (main.c, pulse) */
    u32 qtd_guard;          /* 1: the kernel's free transfer descriptors are counted (usb.c) */
    u32 held;               /* requests that waited for descriptors */
    u32 held_now;
    u32 held_most;
} USB_STATS;

extern volatile USB_STATS usb_stats;

/* The worker's, every tick: requests that wait for the kernel's transfer descriptors are
 * looked at again even if nothing of ours completes. */
void usb_tick(void);

#endif /* XE_USB_H */
