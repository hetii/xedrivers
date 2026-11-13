/* The CDC Ethernet driver: any device whose current configuration is CDC ECM or
 * NCM, matched by class codes, and the transport other drivers build on when their
 * card speaks CDC too (rtl8156). See cdc_net.c for how it works and what it was
 * measured to do; cdc.h for the descriptor and NTB parsing it uses.
 */

#ifndef XE_CDC_NET_H
#define XE_CDC_NET_H

#include "kernel.h"
#include "driver.h"
#include "net.h"

/* How many CDC devices (the RTL8156 counts) can run at once. */
#define CDC_MAX 2

extern const XE_DRIVER xe_driver_cdc;

/* The driver's operations, for a driver that wraps it. cdc_start takes a free
 * instance for the device and picks the configuration for the transport xe_config
 * asks for, falling back to the other one when the device has only that. */
u32  cdc_start(NET_DEV *nd);
void cdc_detached(NET_DEV *nd);
u32  cdc_xmit(NET_DEV *nd, const u8 *frame, u32 length);
u32  cdc_tx_space(NET_DEV *nd);
void cdc_tick(NET_DEV *nd);

/* NCM: below this many blocks in flight a frame goes out at once; at or above
 * it, frames are aggregated. Tunable at run time (xectl.py set cdc_tx_eager N). */
extern volatile u32 cdc_tx_eager;

/* NCM: put the NDP after the datagrams rather than after the header. */
extern volatile u32 cdc_ncm_ndp_end;

/* 0: hand received frames up from the USB completion; N+1: from a DPC on hardware
 * thread N, resubmitting from there. */
extern volatile u32 cdc_rx_cpu;

enum {
    CDC_STEP_OK = 0,
    CDC_STEP_DMA,
    CDC_STEP_DEVICE_DESC,
    CDC_STEP_CONFIG_DESC,
    CDC_STEP_LAYOUT,
    CDC_STEP_MAC,
    CDC_STEP_SET_CONFIG,
    CDC_STEP_SET_INTERFACE,
    CDC_STEP_ENDPOINTS,
    CDC_STEP_FILTER,
    CDC_STEP_NTB_PARAMS,
};

/* Read by tools/xectl.py; field order is its STRUCTS["cdc_stats"]. */
typedef struct {
    u32 transport;          /* 0 ECM, 1 NCM */
    u32 config_value;
    u32 data_interface;
    u32 ep_in;
    u32 ep_out;
    u32 ep_intr;
    u32 intr_events;
    u32 intr_errors;
    u32 last_notify[2];
    u32 rx_deferred;        /* receive batches handed to the cdc_rx_cpu DPC */
    u32 rx_ntbs;
    u32 rx_ntb_errors;
    u32 tx_ntbs;
    u32 ntb_in_max;
    u32 ntb_out_max;
    u32 ntb_out_align;      /* divisor << 16 | remainder */
    u32 ntb_out_datagrams;
    u32 ntb_formats;
    u32 ntb_input_status;
    u32 ntb_format_status;  /* SET_NTB_FORMAT 16, or 0xFFFFFFFF when not sent */
    u32 crc_status;         /* SET_CRC_MODE off */
    u32 dgram_status;       /* SET_MAX_DATAGRAM_SIZE */
    u32 intr_parked;        /* repeats that parked the interrupt endpoint */
    u32 rx_parked;          /* receive requests not queued again: the card was not wanted */
    u32 rx_woken;           /* times its receive ring was started again */
} CDC_STATS;

extern volatile CDC_STATS cdc_stats[CDC_MAX];

#endif /* XE_CDC_NET_H */
