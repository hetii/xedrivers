/* The voice link's data (HCI SCO), host side: what the headset's microphone sends becomes the
 * console's microphone (16 kHz, audio_mic_write), and the console's sound goes to its earpiece.
 * CVSD over the air, 16-bit linear PCM at 8 kHz here (voice setting 0x0060); the transport
 * moves it as a byte stream of SCO packets (btusb's isochronous interface).
 */

#ifndef XE_SCO_H
#define XE_SCO_H

#include "host.h"

#define SCO_PAYLOAD 48u         /* bytes of PCM a packet: 24 samples, 3 ms */
#define SCO_MSBC_PAYLOAD 24u    /* bytes of mSBC units a packet: 9 bytes a ms carry 24 + 3 */

typedef struct {
    u32 packets_in;
    u32 packets_out;
    u32 bad_headers;            /* a packet that did not start where one should: resynced */
    u32 samples_in;
    u32 samples_out;
    u32 out_underruns;          /* samples of silence sent: the console's sound was late */
    u32 out_overruns;
    u32 peak_in;                /* of the last packet, out of 32767 */
    u32 handle;
    u32 bad_frames;             /* mSBC frames that did not decode */
    u32 peak_max;               /* the loudest since the link went up */
    u32 wideband;
} SCO_STATS;

extern volatile SCO_STATS sco_stats;

/* The link is up on `handle` (0 when it went), CVSD or mSBC (`wideband`); worker thread. */
void sco_link(u32 handle, u32 wideband);

/* The transport's side, at DISPATCH_LEVEL: bytes in as they came, and `n` bytes to send. */
void bt_sco_take(const u8 *data, u32 n);
void bt_sco_fill(u8 *data, u32 n);

/* Worker thread, every tick: the console's sound into the outgoing ring. */
void sco_tick(void);

/* Pure (tests/bluetooth): 8 kHz into 16 kHz, `n` samples into 2n. */
void sco_up(const s16 *in, u32 n, s16 *out, s16 *last);

#endif /* XE_SCO_H */
