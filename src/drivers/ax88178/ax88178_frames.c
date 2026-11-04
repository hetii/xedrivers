/* AX88178 framing; see ax88178_frames.h. */

#include "ax88178_frames.h"

static u32 le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }

void ax178_rx_reset(AX178_RX *rx)
{
    rx->have = 0;
    rx->size = 0;
    rx->header = 0;
    rx->split = 0;
}

/* A header checks when its length and the inverted copy agree. */
static u32 header_size(u32 header)
{
    u32 size = header & 0x7FFu;

    return size == ((~header >> 16) & 0x7FFu) ? size : 0xFFFFFFFFu;
}

u32 ax178_rx_feed(AX178_RX *rx, const u8 *data, u32 length, AX178_FRAME_FN fn,
                  void *context)
{
    u32 at = 0, delivered = 0, size, n, i;

    while (at + 2 <= length || (rx->size && at < length)) {
        if (!rx->size) {
            if (rx->split) {
                rx->header |= le16(data + at) << 16;
                rx->split = 0;
                at += 2;
            } else if (length - at == 2) {
                rx->header = le16(data + at);
                rx->split = 1;
                at += 2;
                break;
            } else {
                rx->header = le16(data + at) | le16(data + at + 2) << 16;
                at += 4;
            }
            size = header_size(rx->header);
            if (size == 0xFFFFFFFFu || !size || size > AX178_FRAME_MAX) {
                rx->resyncs++;
                ax178_rx_reset(rx);
                return delivered;
            }
            rx->size = size;
            rx->have = 0;
        }
        n = rx->size - rx->have;
        if (n > length - at)
            n = length - at;
        for (i = 0; i < n; i++)
            rx->frame[rx->have + i] = data[at + i];
        rx->have += n;
        at += n + (n & 1);                      /* each piece padded to even, as Linux
                                                 * steps over them */
        if (rx->have < rx->size)
            break;                              /* continues in the next transfer */
        fn(rx->frame, rx->size, context);
        delivered++;
        rx->size = 0;
        rx->have = 0;
    }
    return delivered;
}

u32 ax178_tx_build(u8 *out, u32 size, const u8 *frame, u32 length, u32 max_packet)
{
    u32 header = ((length ^ 0xFFFFu) << 16) | length, total = 4 + length, i;
    u32 pad = max_packet && (total % max_packet) == 0;

    if (length > 0x7FFu || total + (pad ? 4 : 0) > size)
        return 0;
    out[0] = (u8)header;
    out[1] = (u8)(header >> 8);
    out[2] = (u8)(header >> 16);
    out[3] = (u8)(header >> 24);
    for (i = 0; i < length; i++)
        out[4 + i] = frame[i];
    if (pad) {
        out[total] = 0x00;
        out[total + 1] = 0x00;
        out[total + 2] = 0xFF;
        out[total + 3] = 0xFF;
        total += 4;
    }
    return total;
}
