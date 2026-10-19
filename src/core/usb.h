/* One USB device, claimed for whichever driver matches it, and transfers on it.
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

typedef struct {
    void         *device;           /* the kernel's device node, while we own it */
    void         *control;          /* default endpoint */
    void         *ep[4];            /* opened by the driver, closed in reverse */
    u32           ep_count;
    volatile u32  active;           /* cleared the moment removal starts */
    volatile u32  generation;       /* grows with every AddDevice */
} USB_DEV;

extern USB_DEV usb;

/* Who decides which devices are ours and hears of their arrival and removal --
 * net.c, for every driver at once. `match` gets an interface descriptor (or 0 when
 * only the device is known) and the device descriptor, returns nonzero to claim;
 * `attached` and `detached` run inside AddDevice/RemoveDevice and must not block. */
typedef struct {
    u32  (*match)(const u8 *interface, const u8 *device);
    void (*attached)(void);
    void (*detached)(void);
} USB_CLIENT;

int   usb_register(const USB_CLIENT *client);
void *usb_open(u32 type, u32 address, u32 max_packet, u32 interval);
void  usb_submit(URB *urb);

/* Fill a request for the default endpoint or for `endpoint`. */
void  usb_prepare(URB *urb, void *endpoint, URB_COMPLETION done, void *buffer,
                  u32 length, const u8 *setup);

/* Thread-only: one control transfer, waited for. Returns the completion status, or
 * STATUS_UNSUCCESSFUL on timeout; *actual gets the bytes moved. `buffer` must be
 * DMA memory (from net_dma_alloc). */
u32   usb_control(const u8 setup[8], void *buffer, u32 length, u32 *actual,
                  u32 timeout_ms);

/* Find our device among those already enumerated and reset it so the kernel offers
 * it again -- boot enumeration is over before plugins load. 1: ours found and
 * reset; 2: not recognised, but devices no driver had were reset once each so
 * Match sees their real descriptors; 0: nothing to do.
 * xerunner: plugin-load-vs-enumeration@1, xerunner: usb-driver-object-f19@1 */
u32   usb_claim_present(void);

void  usb_sleep(u32 ms);

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
} USB_STATS;

extern volatile USB_STATS usb_stats;

#endif /* XE_USB_H */
