/* The RTL8822BU's receive path: bulk IN 0x84, several frames to a transfer, as
 * rtw88's usb.c (rtw_usb_rx_handler) and rx.c (rtw_rx_query_rx_desc) read them.
 *
 * Each frame is a 24-byte descriptor, then drv_info_size * 8 bytes of PHY status,
 * `shift` bytes of padding, and the 802.11 frame with its FCS; the next starts at
 * the following multiple of 8. A frame the firmware sends the host (C2H) carries a
 * flag in the descriptor instead.
 *
 * Completions run on the USB processor at DISPATCH_LEVEL; what a frame is good
 * for goes up from there (rtw.h's rx_frame) and the buffer is queued again.
 */

#include "rtw.h"

#define RX_DESC_SIZE    24
#define FCS_LEN         4
#define MPDU_MAX        11454           /* IEEE80211_MAX_MPDU_LEN_VHT_11454 */
#define DESC_RATE_MAX   0x54

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* query_phy_status: page 0 (CCK) reports path A's power, page 1 both paths'. */
static s32 signal_of(const u8 *phy)
{
    s32 a = (s32)phy[1] - 110, b = (s32)phy[2] - 110;

    if ((phy[0] & 0xF) == 0)
        return a;
    return a > b ? a : b;
}

static void submit(RTW *r, u32 i);

static void rx_done(URB *urb, u32 status)
{
    RTW *r = (RTW *)urb->Owner;
    u32 i = (u32)(urb - r->rx_urb), at = 0, w0, len, info, shift, skip;
    const u8 *b = r->rx_buf[i];

    if (!r->rx_running || !r->dev->active)
        return;
    if (!NT_SUCCESS(status)) {
        r->st->rx_errors++;
        r->st->rx_last_error = status;
        submit(r, i);
        return;
    }
    r->st->rx_transfers++;
    while (at + RX_DESC_SIZE <= urb->Actual) {
        const u8 *d = b + at;

        w0 = le32(d);
        len = w0 & 0x3FFF;
        info = ((w0 >> 16) & 0xF) * 8;
        shift = (w0 >> 24) & 3;
        skip = RX_DESC_SIZE + info + shift + len;
        if (!len || len > MPDU_MAX || at + skip > urb->Actual ||
            (d[12] & 0x7F) >= DESC_RATE_MAX) {
            r->st->rx_bad++;
            break;
        }
        if (le32(d + 8) & BIT(28)) {                    /* C2H */
            r->st->rx_c2h++;
        } else if (w0 & (BIT(14) | BIT(15))) {          /* CRC or ICV error */
            r->st->rx_crc_errors++;
        } else if (len > FCS_LEN) {
            const u8 *frame = d + RX_DESC_SIZE + info + shift;
            s32 signal = (w0 & BIT(26)) && info ? signal_of(d + RX_DESC_SIZE + shift) : -100;

            r->st->rx_frames++;
            rtw_rx_frame(r, frame, len - FCS_LEN, signal, (w0 >> 20) & 7);
        }
        at += (skip + 7) & ~7u;
    }
    submit(r, i);
}

static void submit(RTW *r, u32 i)
{
    usb_prepare(&r->rx_urb[i], r->dev, r->ep_in, rx_done, r->rx_buf[i], RTW_RX_SIZE, 0);
    r->rx_urb[i].Owner = r;
    usb_submit(&r->rx_urb[i]);
}

/* rtw_usb_rx_resubmit for every buffer. Thread. */
void rtw_rx_start(RTW *r)
{
    u32 i;

    r->rx_running = 1;
    mem_sync();
    for (i = 0; i < RTW_RX_COUNT; i++)
        submit(r, i);
}
