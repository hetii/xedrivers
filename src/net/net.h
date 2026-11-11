/* The network layer: one Ethernet frame pipe, whichever driver (driver.h) is
 * behind it, and who consumes it. The core calls the three entry points below;
 * consumers -- xnet.c, the bench's stack -- use the second part; drivers report
 * into the third.
 */

#ifndef XE_NET_H
#define XE_NET_H

#include "kernel.h"
#include "usb.h"

struct XE_DRIVER;

#define ETH_ALEN      6
#define ETH_HLEN      14
#define ETH_FRAME_MAX 1518

/* --- for the core --------------------------------------------------------------- */

/* Once, from the worker before its loop: the boot mode, xnet only once a card is up. */
void net_layer_init(void);

/* `dev` was offered (a new USB generation on its slot): bring it up, apply the
 * mode, choose the uplink again. */
void net_bring_up(USB_DEV *dev);

/* Every worker tick: each device's driver tick, while that device is up. */
void net_tick(void);

/* A host command the core does not know: the mode (5), xnet's interface id (13),
 * else the driver's. 1 if handled, the result in args[5]. */
u32  net_command(u32 what, volatile u32 *args);

/* The USB client the core registers: match against every driver, bring-up on
 * arrival, net_detached on removal. */
void net_register(void);

/* Read by tools/xectl.py. */
typedef struct {
    u32 start_result;
    u32 start_count;
    u32 mode;               /* the mode actually in force */
    u32 xnet_status;
    u32 console_mac_status;         /* of ExGetXConfigSetting */
    u32 console_mac_needed;         /* the size it says the setting has */
    u32 console_mac_read[2];        /* what it gave, bytes in order */
    u32 console_mac_from_config;    /* 1 if that was taken as the address */
} NET_STATE;

extern volatile NET_STATE net_state;

/* --- the devices --------------------------------------------------------------- */

#define NET_MAX USB_MAX_DEVICES

/* What every driver reports the same way; a driver's own detail is in its own
 * stats (cdc_stats, rtl_stats). One a device slot; read by tools/xectl.py. */
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
    u32 rx_not_uplink;      /* frames from a device that was not the uplink */
    u32 where;              /* the USB node's controller/port byte */
} NET_STATS;

/* A device taken out of the uplink choice by a consumer of its own -- a wireless
 * card the dashboard drives (src/wireless/omni.c) is not a cable for xnet. Its
 * frames, link changes and freed transmit slots go there instead. Called as
 * net_rx, net_link_cb and net_tx_idle are. */
typedef struct {
    void *context;
    void (*rx)(void *context, const u8 *frame, u32 length);
    void (*link)(void *context, u32 up);
    void (*tx_idle)(void *context);
} NET_CLAIM;

/* One network device, a USB slot's. */
typedef struct NET_DEV {
    USB_DEV *usb;
    const struct XE_DRIVER *drv;
    void *priv;             /* the driver's state for this device */
    volatile NET_STATS *st;
    u8 mac[ETH_ALEN];
    u32 link_down_seen;
    u32 up_order;           /* when its link last came up, for keeping the uplink */
    u32 stalled_ticks;      /* its link taken down by the transmit stall watch: ticks since */
    u8 *rings;
    u32 rings_size;
    u32 rings_used;
    const NET_CLAIM *claim; /* 0 while it is a candidate for the uplink */
} NET_DEV;

extern NET_DEV net_devs[NET_MAX];
extern volatile NET_STATS net_stats[NET_MAX];

/* The uplink: index + 1 into net_devs of the device xnet is given, 0 for none
 * (the onboard port, then). Chosen again on every change of a device's link or
 * readiness: a phone that tethers before a USB card, a faster link before a slower
 * one, the uplink in force before an equal one, the back sockets before the front
 * and a lower port first. */
extern volatile u32 net_active;

/* Nonzero for the moment between two uplinks, when xnet is to see no link at
 * all, not even the onboard port's (see choose() in net.c). */
extern volatile u32 net_switching;

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

extern u8 net_mac[ETH_ALEN];        /* the uplink's own address */

/* The console's own address, as xnet sends from it: known once xnet has sent a
 * frame (xnet.c). */
extern u8 net_console_mac[ETH_ALEN];
extern volatile u32 net_console_mac_known;

/* The address a wireless card is to join on. The console's goes to one card only -- the first
 * to ask, until it leaves: two stations on one address at one access point push each other
 * off, round and round (2026-11-09, an RTL8822BU and an AIC8800 in the console at once). The
 * others join on their own, and the uplink is the one on the console's (net.c, better). */
const u8 *net_join_mac(struct NET_DEV *nd);
extern volatile u32 net_console_mac_handovers;  /* times it was taken from a card without link */

/* Ask the console for its own address, before any device is brought up: a card
 * that has to carry it (a Wi-Fi station associates on it) cannot wait for xnet to
 * send its first frame. Thread-only; what it found is in net_state. */
void net_learn_console_mac(void);

/* Queue one frame on the uplink. Any IRQL up to DISPATCH. 1 if queued, 0 if
 * dropped. */
u32  net_tx(const u8 *frame, u32 length);

/* Whether the next transmit slot is free: a hint for a sender deciding to try. */
u32  net_tx_space(void);

u32  net_link_up(void);
u32  net_speed_mbps(void);

/* --- for drivers -------------------------------------------------------------- */

/* One received frame, up to the consumer if `nd` is the uplink. Any IRQL up to
 * DISPATCH. */
void net_frame_in(NET_DEV *nd, const u8 *frame, u32 length);

/* Link state as the device reports it; a reported down also stops a frame's
 * arrival from being taken as link up (a device that never reports does). */
void net_link_report(NET_DEV *nd, u32 up);
void net_speed_report(NET_DEV *nd, u32 mbps);

/* A transmit completed and its slot is free: wakes net_tx_idle if `nd` is the
 * uplink. */
void net_tx_done(NET_DEV *nd);

/* Take `nd` for `claim` (see NET_CLAIM), or give it back with 0. Until the next
 * bring-up, which gives every device back. Thread-only. */
void net_claim(NET_DEV *nd, const NET_CLAIM *claim);

/* Queue one frame on this device rather than the uplink, and whether it has a free
 * slot: for its claimant. As net_tx and net_tx_space. */
u32  net_dev_tx(NET_DEV *nd, const u8 *frame, u32 length);

/* A frame is about to be handed to xnet, by the uplink or the second way: what the
 * acknowledgement thinning wants to know of it (acks.h). DISPATCH. */
void net_rx_note(const u8 *frame, u32 length);
u32  net_dev_tx_space(NET_DEV *nd);

/* DMA memory. The scratch block is allocated once and never moves -- for control
 * and register buffers a driver keeps across bring-ups; each caller takes its
 * share once. A device's ring block is carved again on every bring-up:
 * net_dma_begin with the total the driver needs (the block is allocated at the
 * first call and must not grow later), then net_dma_alloc per buffer,
 * 2048-aligned so a frame buffer never crosses a 4 KB page. Thread-only; 0 when
 * out of memory. */
void *net_dma_scratch(u32 bytes);
u32   net_dma_begin(NET_DEV *nd, u32 total);
void *net_dma_alloc(NET_DEV *nd, u32 bytes);

void net_add64(volatile u32 *lo, volatile u32 *hi, u32 n);

#endif /* XE_NET_H */
