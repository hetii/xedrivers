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

/* Called at the end of every transmit completion, at DISPATCH_LEVEL on the USB
 * processor, once a slot has been freed -- for a sender that wants to keep the ring
 * full without a thread waiting on it (stack.c's generator). */
typedef void (*NET_TX_IDLE)(void);
extern volatile NET_TX_IDLE net_tx_idle;

extern u8 net_mac[ETH_ALEN];        /* the card's own address, from its descriptor */

/* Bring the attached card up. Thread-only. Returns 0 on success or the step that
 * failed (see NET_STEP_*). */
u32  net_start(void);
void net_detached(void);

/* Queue one frame. Any IRQL up to DISPATCH. Returns 1 if queued, 0 if dropped. */
u32  net_tx(const u8 *frame, u32 length);

/* Whether the next transmit slot is free: a hint for a sender deciding to try. */
u32  net_tx_space(void);

u32  net_link_up(void);
u32  net_speed_mbps(void);

/* DMA memory from the block net_start allocates; never freed. */
void *net_dma_alloc(u32 bytes);

/* REG_SIZE bytes of DMA memory that stay put across bring-ups, for rtl.c's
 * register transfers (the rings are carved again on each one). */
void *net_reg_buffer(void);

/* NCM: below this many blocks in flight a frame goes out at once; at or above
 * it, frames are aggregated. Tunable at run time (xectl.py set net_tx_eager N). */
extern volatile u32 net_tx_eager;

/* NCM: put the NDP after the datagrams rather than after the header. */
extern volatile u32 net_ncm_ndp_end;

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
    NET_STEP_NTB_PARAMS,
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
    u32 transport;          /* 0 ECM, 1 NCM */
    u32 rx_ntbs;
    u32 rx_ntb_errors;
    u32 tx_ntbs;
    u32 ntb_in_max;
    u32 ntb_out_max;
    u32 ntb_out_align;      /* divisor << 16 | remainder */
    u32 ntb_out_datagrams;
    u32 ntb_input_status;
    u32 ntb_format_status;  /* SET_NTB_FORMAT 16, or 0xFFFFFFFF when not sent */
    u32 crc_status;         /* SET_CRC_MODE off */
    u32 dgram_status;       /* SET_MAX_DATAGRAM_SIZE */
    u32 ntb_formats;
} NET_STATS;

extern volatile NET_STATS net_stats;

#endif /* XE_NET_H */
