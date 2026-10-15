/* The RTL8156 as an Ethernet frame pipe: bring-up, receive ring, transmit slots,
 * link state. Who consumes the frames is decided by net_rx (stack.c or xnet.c). */

#ifndef XE_NET_H
#define XE_NET_H

#include "kernel.h"

#define ETH_ALEN      6
#define ETH_HLEN      14
#define ETH_FRAME_MAX 1518

/* Where received frames go. Called at DISPATCH_LEVEL on the USB processor; the
 * frame is only valid for the duration of the call. */
typedef void (*NET_RX)(const u8 *frame, u32 length);
extern volatile NET_RX net_rx;

/* Called when the link goes up or down, same context as net_rx. */
typedef void (*NET_LINK)(u32 up);
extern volatile NET_LINK net_link_cb;

extern u8 net_mac[ETH_ALEN];        /* the card's own address, from its descriptor */

/* Bring the attached card up. Thread-only. Returns 0 on success or the step that
 * failed (see NET_STEP_*). */
u32  net_start(void);
void net_detached(void);

/* Queue one frame. Any IRQL up to DISPATCH. Returns 1 if queued, 0 if dropped. */
u32  net_tx(const u8 *frame, u32 length);

u32  net_link_up(void);
u32  net_speed_mbps(void);

/* DMA memory from the block net_start allocates; never freed. */
void *net_dma_alloc(u32 bytes);

enum {
    NET_STEP_OK = 0,
    NET_STEP_DMA,
    NET_STEP_DEVICE_DESC,
    NET_STEP_CONFIG_DESC,
    NET_STEP_LAYOUT,
    NET_STEP_MAC,
    NET_STEP_SET_CONFIG,
    NET_STEP_SET_INTERFACE,
    NET_STEP_ENDPOINTS,
    NET_STEP_FILTER,
};

typedef struct {
    u32 step;               /* last bring-up step reached */
    u32 step_status;        /* status of the step that failed */
    u32 ready;
    u32 link;
    u32 speed_mbps;
    u32 rx_frames;
    u32 rx_bytes;
    u32 rx_errors;
    u32 rx_last_error;
    u32 rx_short;
    u32 tx_frames;
    u32 tx_bytes;
    u32 tx_drops;
    u32 tx_errors;
    u32 tx_last_error;
    u32 tx_inflight;
    u32 intr_events;
    u32 intr_errors;
    u32 last_notify[2];
    u32 rx_stopped;
    u32 config_value;
    u32 data_interface;
    u32 ep_in;
    u32 ep_out;
    u32 ep_intr;
    u32 rx_bytes_hi;
    u32 tx_bytes_hi;
} NET_STATS;

extern volatile NET_STATS net_stats;

#endif /* XE_NET_H */
