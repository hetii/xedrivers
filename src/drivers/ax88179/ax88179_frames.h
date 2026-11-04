/* The AX88179 / AX88178A bulk framing, pure: what a receive transfer holds and what
 * a transmitted frame is prefixed with. From Linux ax88179_178a.c (GPL-2.0):
 * ax88179_rx_fixup, ax88179_tx_fixup.
 *
 * A receive transfer is
 *     <frame 1> <pad to 8> ... <frame N> <pad to 8>
 *     <metadata 1> ... <metadata M> [<pad 4>] <rx header>
 * the rx header (last 4 bytes, little-endian) the count of metadata words (low 16
 * bits) and the offset of the first (high 16); each metadata word a frame's
 * length in bits 28..16 and its error bits (31 drop, 29 CRC). Current firmware puts
 * a dummy word (length 0) after each real one. A frame starts with 2 bytes of IP
 * alignment before the Ethernet header.
 *
 * A transmitted frame is prefixed with 8 bytes: its length (le32), then the TSO
 * MSS (0 here), with 0x80008000 set when frame and header end on a USB packet
 * boundary -- the chip then expects a padding byte rather than a short packet.
 */

#ifndef XE_AX88179_FRAMES_H
#define XE_AX88179_FRAMES_H

#include "kernel.h"

#define AX179_TX_HEADER   8u
#define AX179_RX_ALIGN    2u        /* bytes before each received frame's header */

/* Called for every good frame of a transfer: `frame` its Ethernet header on. */
typedef void (*AX179_FRAME_FN)(const u8 *frame, u32 length, void *context);

/* Split one receive transfer; the frames go to `fn`. Returns the frames delivered,
 * or -1 if the transfer did not parse (counted by the caller). `*bad` gets the
 * frames the chip marked bad (CRC, dropped, runt). */
s32 ax179_rx_split(const u8 *data, u32 length, AX179_FRAME_FN fn, void *context,
                   u32 *bad);

/* The 8-byte header for a frame of `length` bytes on a pipe of `max_packet`. */
void ax179_tx_header(u8 *header, u32 length, u32 max_packet);

/* The AX88179A/B (bcdDevice 2.00), after CUsbEnet's CAx179AFamily
 * (InitializeReceiveParser, GetNextReceiveFrame, AppendTransmitFrame). A receive
 * transfer is
 *     <frame 1> <pad to 8> ... <frame N> <pad to 8> <descriptor 1..N> <trailer>
 * the trailer the last 8 bytes, its low word the count (bits 12..0) and the
 * descriptors' offset (bits 31..13), which must be just before it; each
 * descriptor 8 bytes, its low word the frame's length (bits 30..16), RX_OK (bit
 * 11) and drop (bit 31). No alignment bytes before a frame.
 *
 * A transmitted frame is an 8-byte descriptor (the length, bits 20..0) and the
 * frame, padded to 8; when the record ends on a USB packet boundary, bit 28 says
 * an 8-byte terminator of zeroes follows. Returns the bytes written to `out`, 0
 * if `size` is too small. */
s32 ax179a_rx_split(const u8 *data, u32 length, AX179_FRAME_FN fn, void *context,
                    u32 *bad);
u32 ax179a_tx_build(u8 *out, u32 size, const u8 *frame, u32 length, u32 max_packet);

#endif /* XE_AX88179_FRAMES_H */
