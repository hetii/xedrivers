/* A USB Audio Class device: its state, shared by uac.c (bring-up) and stream.c (the
 * isochronous transfers). */

#ifndef XE_UAC_STREAM_H
#define XE_UAC_STREAM_H

#include "descriptors.h"
#include "net.h"
#include "pacer.h"
#include "usb.h"

/* A transfer is 4 ms either way: 4 packets at full speed (a frame each), 32 at high speed
 * (a microframe each, of which only every `period`-th carries data, the endpoint's
 * interval). The kernel's EHCI side fills a microframe a packet, from the next free one, and
 * keeps 16 iTDs of 8 microframes for an endpoint without checking: 3 transfers of 4 stay
 * below that (17559, 0x800df798). */
#define UAC_FS_PACKETS  4u
#define UAC_HS_PACKETS  32u
#define UAC_PACKETS_MAX 32u
#define UAC_SLOTS       3u          /* transfers out at once */
#define UAC_FRAMES_MAX  (4u * 48u + UAC_PACKETS_MAX)    /* a transfer's frames at most */
#define UAC_BUFFER      (UAC_FRAMES_MAX * 6u * 4u)      /* and its bytes: 6 channels of 4 */
#define UAC_MIC_SLOTS   2u
#define UAC_MIC_BUFFER  8192u                           /* a microphone transfer at most */

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
    u32 high_speed;
    u32 period;             /* microframes between packets with data (1 at full speed) */
    u32 mic_step;           /* 0 capturing, else why not (mic.c) */
    u32 mic_transfers;
    u32 mic_packets_ok;
    u32 mic_packets_bad;
    u32 mic_last_status;
    u32 mic_samples;        /* 16 kHz samples handed to audio_mic_write */
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
    u32 packets;                /* a transfer's: UAC_FS_PACKETS or UAC_HS_PACKETS */
    u32 period;                 /* every period-th packet carries data */
    u32 feedback_period;
    u32 frames_nominal;         /* a data packet's frames */
    ISOCH play[UAC_SLOTS];
    u16 play_lengths[UAC_SLOTS][UAC_PACKETS_MAX];
    u8 *play_buf[UAC_SLOTS];
    ISOCH feedback[UAC_SLOTS];
    u16 feedback_lengths[UAC_SLOTS][UAC_PACKETS_MAX];
    u8 *feedback_buf[UAC_SLOTS];
    s32 frames[UAC_FRAMES_MAX * 6];
    u32 surround;               /* audio_surround() when this stream was chosen */
    u32 ticks;
    void *ep_mic;
    ISOCH mic[UAC_MIC_SLOTS];
    u16 mic_lengths[UAC_MIC_SLOTS][UAC_PACKETS_MAX];
    u8 *mic_buf;                /* UAC_MIC_SLOTS x UAC_MIC_BUFFER */
    u32 mic_room;               /* a data packet's bytes, the endpoint's largest */
    u32 mic_bad_in_a_row;
    u32 mic_sum, mic_count;     /* the 48 -> 16 kHz decimator's state */
} UAC;

/* Worker thread: buffers, then the first transfers; a step number, or 0. */
u32  stream_start(UAC *u);

/* Worker thread: the microphone's interface, rate, endpoint and first transfers, after the
 * output is up (mic.c). Records why not in st->mic_step; a card plays without it. */
void mic_start(UAC *u);

/* Thread-only: one control transfer on the card's default endpoint (uac.c). */
u32  uac_control(UAC *u, u32 type, u32 request, u32 value, u32 index, u32 length, u32 *actual);

#endif /* XE_UAC_STREAM_H */
