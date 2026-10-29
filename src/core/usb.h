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
    void         *ep[8];            /* opened by the driver, closed in reverse: a
                                     * Wi-Fi and Bluetooth combo needs seven */
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

extern USB_DEV usb_devs[USB_MAX_DEVICES];

/* Who decides which devices are ours and hears of their arrival and removal --
 * net.c, for every driver at once. `match` gets an interface descriptor (or 0 when
 * only the device is known) and the device descriptor, returns nonzero to claim;
 * `attached` and `detached` run inside AddDevice/RemoveDevice and must not block. */
typedef struct {
    u32  (*match)(const u8 *interface, const u8 *device);
    void (*attached)(USB_DEV *dev);
    void (*detached)(USB_DEV *dev);
} USB_CLIENT;

#define USB_PROCESSOR 2             /* hardware thread the USB stack runs on */

int   usb_register(const USB_CLIENT *client);
void *usb_open(USB_DEV *dev, u32 type, u32 address, u32 max_packet, u32 interval);
void  usb_submit(URB *urb);

/* Thread-only: wait until `urb` is not the host controller's any more, up to
 * `timeout_ms`; 0 if it still is. A synchronous request that timed out stays
 * queued, and preparing it again (or writing its buffer) while the controller
 * holds it wrecks the controller's lists: every synchronous helper asks this
 * first, and its completion clears Busy. */
u32   usb_wait_idle(URB *urb, u32 timeout_ms);
void  usb_spin(volatile u32 *done);    /* up to 1 ms for a transfer just submitted */

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

/* Wait up to `ms`, or until usb_wake: the worker's and usb_control's wait, which a
 * dimmed dashboard does not stall. usb_wake may be called at DISPATCH_LEVEL. */
void  usb_wait(u32 ms);
void  usb_wake(void);

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
} USB_STATS;

extern volatile USB_STATS usb_stats;

#endif /* XE_USB_H */
