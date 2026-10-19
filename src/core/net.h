/* The network device as the rest of the plugin sees it: one Ethernet frame pipe,
 * whichever driver (driver.h) is behind it. Consumers -- xnet.c, stack.c -- use
 * the first half; drivers report into the second.
 */

#ifndef XE_NET_H
#define XE_NET_H

#include "kernel.h"

#define ETH_ALEN      6
#define ETH_HLEN      14
#define ETH_FRAME_MAX 1518

/* --- for consumers ------------------------------------------------------------ */

/* Where received frames go. Called at DISPATCH_LEVEL on the USB processor (or on
 * the processor a driver hands receive to); the frame is only valid for the
 * duration of the call. */
typedef void (*NET_RX)(const u8 *frame, u32 length);
extern volatile NET_RX net_rx;

/* Called when the link goes up or down, same context as net_rx. */
typedef void (*NET_LINK)(u32 up);
extern volatile NET_LINK net_link_cb;

/* Called at the end of every transmit completion, once a slot has been freed --
 * for a sender that wants to keep the ring full without a thread waiting on it
 * (stack.c's generator, xnet.c's queue). */
typedef void (*NET_TX_IDLE)(void);
extern volatile NET_TX_IDLE net_tx_idle;

extern u8 net_mac[ETH_ALEN];        /* the card's own address, set by its driver */

/* Bring the attached device up with its driver. Thread-only. Returns 0 on success
 * or the step that failed. */
u32  net_start(void);
void net_detached(void);

/* Queue one frame. Any IRQL up to DISPATCH. Returns 1 if queued, 0 if dropped. */
u32  net_tx(const u8 *frame, u32 length);

/* Whether the next transmit slot is free: a hint for a sender deciding to try. */
u32  net_tx_space(void);

u32  net_link_up(void);
u32  net_speed_mbps(void);

/* A host command the core does not know, handed to the driver. 1 if handled. */
u32  net_command(u32 what, volatile u32 *args);

/* The USB client the core registers: match against every driver, bring-up on
 * arrival, net_detached on removal. */
void net_register(void);

/* --- for drivers -------------------------------------------------------------- */

/* One received frame, up to the consumer. Any IRQL up to DISPATCH. */
void net_frame_in(const u8 *frame, u32 length);

/* Link state as the device reports it; a reported down also stops a frame's
 * arrival from being taken as link up (a device that never reports does). */
void net_link_report(u32 up);
void net_speed_report(u32 mbps);

/* A transmit completed and its slot is free: wakes net_tx_idle. */
void net_tx_done(void);

/* DMA memory. The scratch block is allocated once and never moves -- for control
 * and register buffers a driver keeps across bring-ups; each caller takes its
 * share once. The ring block is carved again on every bring-up: net_dma_begin
 * with the total the driver needs (the block is allocated at the first call and
 * must not grow later), then net_dma_alloc per buffer, 2048-aligned so a frame
 * buffer never crosses a 4 KB page. Thread-only; 0 when out of memory. */
void *net_dma_scratch(u32 bytes);
u32   net_dma_begin(u32 total);
void *net_dma_alloc(u32 bytes);

/* What every driver reports the same way; a driver's own detail is in its own
 * stats (cdc_stats, rtl_stats). Read by tests/xectl.py. */
typedef struct {
    u32 driver;             /* index into xe_drivers + 1, 0 for none */
    u32 step;               /* last bring-up step reached, the driver's numbering */
    u32 step_status;        /* status of the step that failed */
    u32 ready;
    u32 link;
    u32 speed_mbps;
    u32 rx_frames;
    u32 rx_bytes;
    u32 rx_bytes_hi;
    u32 rx_errors;
    u32 rx_last_error;
    u32 rx_short;
    u32 rx_stopped;
    u32 tx_frames;
    u32 tx_bytes;
    u32 tx_bytes_hi;
    u32 tx_drops;
    u32 tx_errors;
    u32 tx_last_error;
    u32 tx_inflight;
} NET_STATS;

extern volatile NET_STATS net_stats;

void net_add64(volatile u32 *lo, volatile u32 *hi, u32 n);

#endif /* XE_NET_H */
