/* A USB Audio Class device: its state, shared by uac.c (bring-up) and stream.c (the
 * isochronous transfers). */

#ifndef XE_UAC_STREAM_H
#define XE_UAC_STREAM_H

#include "descriptors.h"
#include "net.h"
#include "pacer.h"
#include "usb.h"

#define UAC_PACKETS     4u          /* a transfer: 4 ms */
#define UAC_SLOTS       3u          /* transfers out at once */
#define UAC_MOST_FRAMES 49u         /* a packet's frames */
#define UAC_BUFFER      (UAC_MOST_FRAMES * 6u * 4u * UAC_PACKETS)  /* a transfer at most */

/* What the host sees: xectl status. One a device slot. */
typedef struct {
    u32 version;
    u32 play_interface, play_alternate, play_bytes, play_endpoint, play_max_packet, play_sync;
    u32 feedback_endpoint;
    u32 transfers;
    u32 packets_ok;
    u32 packets_late;       /* the controller got to it too late (0xC005100E) */
    u32 packets_bad;
    u32 last_status;
    u32 gave_up;            /* transfers kept failing: the stream stopped */
    u32 feedback_taken;
    u32 feedback_rate;      /* 16.16 samples a millisecond */
    u32 audio_open;
    u32 channels;           /* of the output in use */
} UAC_STATS;

extern volatile UAC_STATS uac_stats[NET_MAX];

typedef struct UAC {
    NET_DEV *nd;
    USB_DEV *dev;
    volatile UAC_STATS *st;
    UAC_INFO info;
    u8 *control;                /* DMA buffer for control transfers */
    void *ep_play, *ep_feedback;
    UAC_PACER pacer;
    volatile u32 stopping;
    u32 bad_in_a_row;
    ISOCH play[UAC_SLOTS];
    u16 play_lengths[UAC_SLOTS][UAC_PACKETS];
    u8 *play_buf[UAC_SLOTS];
    ISOCH feedback[UAC_SLOTS];
    u16 feedback_lengths[UAC_SLOTS][UAC_PACKETS];
    u8 *feedback_buf[UAC_SLOTS];
    s32 frames[UAC_MOST_FRAMES * UAC_PACKETS * 6];
    u32 surround;               /* audio_surround() when this stream was chosen */
    u32 ticks;
} UAC;

/* Worker thread: buffers, then the first transfers; a step number, or 0. */
u32  stream_start(UAC *u);

#endif /* XE_UAC_STREAM_H */
