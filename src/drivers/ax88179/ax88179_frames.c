/* AX88179 / AX88178A framing; see ax88179_frames.h. */

#include "ax88179_frames.h"

#define RXHDR_DROP_ERR  0x80000000u
#define RXHDR_CRC_ERR   0x20000000u
#define ETH_HLEN_MIN    14u

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

s32 ax179_rx_split(const u8 *data, u32 length, AX179_FRAME_FN fn, void *context,
                   u32 *bad)
{
    u32 rx_hdr, count, offset, at = 0, i, delivered = 0;

    *bad = 0;
    if (length < 4)
        return -1;
    length -= 4;
    rx_hdr = le32(data + length);
    count = rx_hdr & 0xFFFFu;
    offset = rx_hdr >> 16;
    if (!count)
        return 0;
    if (offset > length || count * 4 > length - offset)
        return -1;
    /* the frames end where the metadata starts */
    for (i = 0; i < count; i++) {
        u32 meta = le32(data + offset + 4 * i);
        u32 frame_len = (meta >> 16) & 0x1FFFu, padded = (frame_len + 7) & ~7u;

        if (!frame_len)
            continue;                           /* the dummy word after each frame */
        if (padded > offset - at)
            return -1;
        if ((meta & (RXHDR_DROP_ERR | RXHDR_CRC_ERR)) ||
            frame_len < AX179_RX_ALIGN + ETH_HLEN_MIN) {
            (*bad)++;
        } else {
            fn(data + at + AX179_RX_ALIGN, frame_len - AX179_RX_ALIGN, context);
            delivered++;
        }
        at += padded;
    }
    return (s32)delivered;
}

void ax179_tx_header(u8 *header, u32 length, u32 max_packet)
{
    u32 second = 0;

    if (max_packet && (length + AX179_TX_HEADER) % max_packet == 0)
        second = 0x80008000u;
    header[0] = (u8)length;
    header[1] = (u8)(length >> 8);
    header[2] = (u8)(length >> 16);
    header[3] = (u8)(length >> 24);
    header[4] = (u8)second;
    header[5] = (u8)(second >> 8);
    header[6] = (u8)(second >> 16);
    header[7] = (u8)(second >> 24);
}

#define A_RX_OK         0x00000800u
#define A_RX_DROP       0x80000000u
#define A_TX_TERMINATOR 0x10000000u

s32 ax179a_rx_split(const u8 *data, u32 length, AX179_FRAME_FN fn, void *context,
                    u32 *bad)
{
    u32 trailer, count, meta, at = 0, i, delivered = 0;

    *bad = 0;
    if (length < 8)
        return -1;
    trailer = le32(data + length - 8);
    count = trailer & 0x1FFFu;
    meta = (trailer & 0xFFFFE000u) >> 13;
    if (!count)
        return 0;
    if (count > (length - 8) / 8 || meta != length - 8 - count * 8)
        return -1;
    for (i = 0; i < count; i++) {
        u32 d = le32(data + meta + 8 * i);
        u32 frame_len = (d & 0x7FFF0000u) >> 16, padded = (frame_len + 7) & ~7u;

        if (padded > meta - at)
            return -1;
        if ((d & A_RX_DROP) || !(d & A_RX_OK) || frame_len < ETH_HLEN_MIN) {
            (*bad)++;
        } else {
            fn(data + at, frame_len, context);
            delivered++;
        }
        at += padded;
    }
    return (s32)delivered;
}

u32 ax179a_tx_build(u8 *out, u32 size, const u8 *frame, u32 length, u32 max_packet)
{
    u32 record = (8 + length + 7) & ~7u, total, d, i;
    u32 terminator = max_packet && record % max_packet == 0;

    total = record + (terminator ? 8 : 0);
    if (length > 0x1FFFFFu || total > size)
        return 0;
    d = length | (terminator ? A_TX_TERMINATOR : 0);
    out[0] = (u8)d;
    out[1] = (u8)(d >> 8);
    out[2] = (u8)(d >> 16);
    out[3] = (u8)(d >> 24);
    out[4] = out[5] = out[6] = out[7] = 0;
    for (i = 0; i < length; i++)
        out[8 + i] = frame[i];
    for (i = 8 + length; i < total; i++)
        out[i] = 0;
    return total;
}
