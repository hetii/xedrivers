/* The AX88178 bulk framing, pure. From Linux asix_common.c (GPL-2.0):
 * asix_rx_fixup_internal, asix_tx_fixup.
 *
 * Received frames come as a stream: each a 4-byte header -- its length in bits 10..0
 * and the same length inverted in bits 26..16 -- then the frame, padded to an even
 * length; a frame, or its header, may continue in the next transfer. Sent frames
 * get the same header; when header and frame end on a USB packet boundary, 4 more
 * bytes (0xFFFF0000) follow, so the transfer never ends exactly on one.
 */

#ifndef XE_AX88178_FRAMES_H
#define XE_AX88178_FRAMES_H

#include "kernel.h"

#define AX178_FRAME_MAX   1536u     /* a frame longer than this is a lost header */

typedef void (*AX178_FRAME_FN)(const u8 *frame, u32 length, void *context);

/* The state carried from one receive transfer to the next. */
typedef struct {
    u8  frame[AX178_FRAME_MAX];
    u32 have;           /* bytes of the current frame copied so far */
    u32 size;           /* its length, 0 when between frames */
    u32 header;         /* a header split across transfers: its first half */
    u32 split;          /* 1 while that half waits for the rest */
    u32 resyncs;        /* headers that did not check: the stream was lost */
} AX178_RX;

void ax178_rx_reset(AX178_RX *rx);

/* Feed one receive transfer; whole frames go to `fn`. Returns the frames
 * delivered. A header that does not check drops the transfer's remainder and starts
 * over at the next one (rx->resyncs counts it). */
u32 ax178_rx_feed(AX178_RX *rx, const u8 *data, u32 length, AX178_FRAME_FN fn,
                  void *context);

/* Header, frame and the padding word into `out` (`size` bytes); the bytes
 * written, 0 if they do not fit. */
u32 ax178_tx_build(u8 *out, u32 size, const u8 *frame, u32 length, u32 max_packet);

#endif /* XE_AX88178_FRAMES_H */
