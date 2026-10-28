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
#include "prof.h"
#include "bench.h"

#define RX_DESC_SIZE    24
#define FCS_LEN         4
#define MPDU_MAX        11454           /* IEEE80211_MAX_MPDU_LEN_VHT_11454 */
#define DESC_RATE_MAX   0x54
#define RX_RETRIES      4               /* failed completions in a row before a transfer stays out */

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
    u8 *b = r->rx_buf[i];
    PROF_START(t);

    if (!r->rx_running || !r->dev->active) {
        bench_trace("x");               /* a transfer of the card that was */
        return;
    }
    if (!NT_SUCCESS(status)) {
        /* Queued again at once, a transfer the endpoint refuses comes back at
         * once, and the USB processor does nothing else from then on: after a
         * few in a row it stays out, and the watchdog resets the card. */
        r->st->rx_errors++;
        r->st->rx_last_error = status;
        if (r->st->rx_errors <= 8) {
            static char mark[] = "[E 00000000 a00000]";
            static const char hex[] = "0123456789abcdef";
            u32 k;

            for (k = 0; k < 8; k++)
                mark[3 + k] = hex[(status >> (28 - 4 * k)) & 15];
            for (k = 0; k < 5; k++)
                mark[13 + k] = hex[(urb->Actual >> (16 - 4 * k)) & 15];
            bench_trace(mark);
        }
        if (++r->rx_failing[i] < RX_RETRIES) {
            submit(r, i);
        } else {
            __sync_fetch_and_add(&r->rx_parked, 1);
            bench_trace("[rx parked]");
        }
        return;
    }
    r->rx_failing[i] = 0;
    r->st->rx_transfers++;

    if (urb->Actual > r->st->rx_largest)
        r->st->rx_largest = urb->Actual;
    while (at + RX_DESC_SIZE <= urb->Actual) {
        u8 *d = b + at;

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
            u8 *frame = d + RX_DESC_SIZE + info + shift;
            s32 signal = (w0 & BIT(26)) && info ? signal_of(d + RX_DESC_SIZE + shift) : -100;

            r->st->rx_frames++;
            rtw_rx_frame(r, frame, len - FCS_LEN, signal, w0);
        }
        at += (skip + 7) & ~7u;
    }
    submit(r, i);
    PROF_END(rx_done, t, urb->Actual);
}

static void submit(RTW *r, u32 i)
{
    usb_prepare(&r->rx_urb[i], r->dev, r->ep_in, rx_done, r->rx_buf[i], RTW_RX_SIZE, 0);
    r->rx_urb[i].Owner = r;
    usb_submit(&r->rx_urb[i]);
}

/* rtw_usb_dynamic_rx_agg_v1, enabled, with a threshold the receive buffers hold:
 * the chip closes a transfer once it passes 4 x 4 KB, so one frame more stays under
 * RTW_RX_SIZE (rtw88 takes 5 into 32 KB buffers). The timeout is a quarter of
 * rtw88's: what the chip waits before sending a short aggregate is in every round
 * trip, and 8 against 0x20 took the median from 3.6 to 2.7 ms and one TCP stream in
 * from 4.5 to 5.3 MB/s. Shorter ones (1, or no aggregation) are faster still, but
 * under four streams the chip then stops sending on bulk IN within a minute, 141
 * frames in its FIFO (REG_RXPKT_NUM) and all eight transfers queued; 8 held a 20
 * minute soak both ways. Thread. */
#define RX_AGG_PAGES    4
#define RX_AGG_TIMEOUT  0x08

static void rx_aggregation(RTW *r)
{
    rtw_write8_set(r, REG_TXDMA_PQ_MAP, BIT_RXDMA_AGG_EN);
    rtw_write8_clr(r, REG_RXDMA_AGG_PG_TH + 3, BIT(7));
    rtw_write16(r, REG_RXDMA_AGG_PG_TH, RX_AGG_PAGES | RX_AGG_TIMEOUT << 8);
}

/* rtw_usb_rx_resubmit for every buffer. Thread. */
void rtw_rx_start(RTW *r)
{
    u32 i;

    bench_trace("[rx start]");
    rx_aggregation(r);
    for (i = 0; i < RTW_RX_COUNT; i++)
        r->rx_failing[i] = 0;
    r->rx_parked = 0;
    r->rx_running = 1;
    mem_sync();
    for (i = 0; i < RTW_RX_COUNT; i++)
        submit(r, i);
}
