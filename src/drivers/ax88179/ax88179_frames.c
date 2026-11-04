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
