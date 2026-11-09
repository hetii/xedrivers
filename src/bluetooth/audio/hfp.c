/* See hfp.h. The headset leads the service level connection with AT commands, which are
 * answered here, one line at a time:
 *
 *   AT+BRSF=<hf features>   +BRSF: <ours>        what each side does
 *   AT+BAC=1,2              OK                   the codecs it takes (1 CVSD, 2 mSBC)
 *   AT+CIND=?               +CIND: (...)         the indicators, and their ranges
 *   AT+CIND?                +CIND: 1,0,0,0,5,0,5 their values: service, no call
 *   AT+CMER=3,0,0,1         OK                   indicator events on: the connection is up
 *
 * and after it whatever else it asks (gains, CLIP, CMEE, NREC, BIA, XAPL ...) with OK, or
 * ERROR for what is not known. Once it is up the voice link goes up: the console's voice chat
 * is a call that never ends.
 */

#include "hfp.h"
#include "sco.h"

/* AG features: bit 8 extended error codes -- no three-way calling, no in-band ring, no
 * codec negotiation yet (CVSD). */
#define AG_FEATURES 0x00000100u

volatile HFP_STATS hfp_stats;

static RFCOMM_DLC *dlc;
static char line[128];
static u32 line_length;
static char answer[256];
static u32 sco_wanted;

static u32 starts(const char *s, u32 n, const char *prefix)
{
    u32 i;

    for (i = 0; prefix[i]; i++)
        if (i >= n || (s[i] & ~0x20) != (prefix[i] & ~0x20))  /* case does not matter */
            return 0;
    return 1;
}

static u32 number(const char *s, u32 n)
{
    u32 v = 0, i;

    for (i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++)
        v = v * 10 + (u32)(s[i] - '0');
    return v;
}

static u32 put(char *out, u32 at, u32 room, const char *s)
{
    while (*s && at < room)
        out[at++] = *s++;
    return at;
}

static u32 put_number(char *out, u32 at, u32 room, u32 v)
{
    char d[10];
    u32 k = 0;

    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (k && at < room)
        out[at++] = d[--k];
    return at;
}

u32 hfp_answer(const char *s, u32 n, char *out, u32 room)
{
    u32 at = 0, i;

    hfp_stats.commands++;
    if (starts(s, n, "AT+BRSF=")) {
        hfp_stats.hf_features = number(s + 8, n - 8);
        at = put(out, at, room, "\r\n+BRSF: ");
        at = put_number(out, at, room, AG_FEATURES);
        at = put(out, at, room, "\r\n");
    } else if (starts(s, n, "AT+BAC=")) {
        hfp_stats.codecs = 0;
        for (i = 7; i < n; i++)
            if (s[i] >= '0' && s[i] <= '9')
                hfp_stats.codecs |= 1u << (s[i] - '0');
    } else if (starts(s, n, "AT+CIND=?")) {
        at = put(out, at, room, "\r\n+CIND: (\"service\",(0,1)),(\"call\",(0,1)),"
                                "(\"callsetup\",(0-3)),(\"callheld\",(0-2)),(\"signal\",(0-5)),"
                                "(\"roam\",(0,1)),(\"battchg\",(0-5))\r\n");
    } else if (starts(s, n, "AT+CIND?")) {
        at = put(out, at, room, "\r\n+CIND: 1,0,0,0,5,0,5\r\n");
    } else if (starts(s, n, "AT+CMER=")) {
        if (hfp_stats.state < 2) {
            hfp_stats.state = 2;                /* the service level connection is up */
            sco_wanted = 1;
        }
    } else if (starts(s, n, "AT+VGS=")) {
        hfp_stats.speaker_gain = number(s + 7, n - 7);
    } else if (starts(s, n, "AT+VGM=")) {
        hfp_stats.mic_gain = number(s + 7, n - 7);
    } else if (starts(s, n, "AT+CHLD=?")) {
        at = put(out, at, room, "\r\n+CHLD: (0,1,2)\r\n");
    } else if (starts(s, n, "AT+CLCC")) {
        /* no calls listed */
    } else if (starts(s, n, "AT+COPS?")) {
        at = put(out, at, room, "\r\n+COPS: 0,0,\"Xbox 360\"\r\n");
    } else if (starts(s, n, "AT+CNUM")) {
        /* no number */
    } else if (starts(s, n, "AT+CLIP=") || starts(s, n, "AT+CCWA=") ||
               starts(s, n, "AT+CMEE=") || starts(s, n, "AT+NREC=") ||
               starts(s, n, "AT+BIA=") || starts(s, n, "AT+XAPL=") ||
               starts(s, n, "AT+IPHONEACCEV=") || starts(s, n, "AT+COPS=") ||
               starts(s, n, "AT+BIND=") || starts(s, n, "AT+BIEV=") ||
               starts(s, n, "AT+XEVENT=") || starts(s, n, "AT+BTRH?") ||
               starts(s, n, "AT+CSRSF=") || starts(s, n, "AT+BCS=")) {
        if (starts(s, n, "AT+XAPL="))
            at = put(out, at, room, "\r\n+XAPL=iPhone,6\r\n");
    } else {
        hfp_stats.unknown++;
        for (i = 0; i < 8 && i < n; i++)
            ((volatile u8 *)hfp_stats.last_unknown)[i] = (u8)s[i];
        return put(out, at, room, "\r\nERROR\r\n");
    }
    return put(out, at, room, "\r\nOK\r\n");
}

/* An answer, in pieces of the DLC's frame size. */
static void reply(RFCOMM_DLC *d, u32 k)
{
    u32 at = 0;

    while (at < k) {
        u32 part = k - at > d->mtu ? d->mtu : k - at;

        if (rfcomm_send(d, (const u8 *)answer + at, part))
            return;
        at += part;
    }
}

static void data(RFCOMM_DLC *d, const u8 *p, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        char c = (char)p[i];

        if (c == '\r' || c == '\n') {
            if (line_length)
                reply(d, hfp_answer(line, line_length, answer, sizeof(answer)));
            line_length = 0;
            continue;
        }
        if (line_length < sizeof(line))
            line[line_length++] = c;
    }
}

static void opened(RFCOMM_DLC *d)
{
    dlc = d;
    line_length = 0;
    hfp_stats.state = 1;
}

static void closed(RFCOMM_DLC *d)
{
    if (d != dlc)
        return;
    dlc = 0;
    if (hfp_stats.state == 3) {
        hci_sco(0);
        sco_link(0);
    }
    hfp_stats.state = 0;
    sco_wanted = 0;
}

static const RFCOMM_USER hfp_user = { opened, data, closed };

void hfp_init(void)
{
    rfcomm_init();
    rfcomm_listen(HFP_CHANNEL, &hfp_user);
}

/* The voice link, once the service level connection is up: CVSD, 64 kbit/s each way, the
 * controller converting from 16-bit linear PCM (voice setting 0x0060). */
void hfp_tick(void)
{
    u8 p[17];

    sco_tick();
    if (!sco_wanted || !dlc || !dlc->conn || dlc->conn->state != BT_CONN_SECURE)
        return;
    sco_wanted = 0;
    p[0] = (u8)dlc->conn->handle;
    p[1] = (u8)(dlc->conn->handle >> 8);
    p[2] = 0x40; p[3] = 0x1F; p[4] = 0; p[5] = 0;      /* transmit bandwidth 8000 */
    p[6] = 0x40; p[7] = 0x1F; p[8] = 0; p[9] = 0;      /* receive bandwidth 8000 */
    p[10] = 0xFF; p[11] = 0xFF;                         /* latency: any */
    p[12] = 0x60; p[13] = 0x00;                         /* voice setting */
    p[14] = 0xFF;                                       /* retransmission: any */
    p[15] = 0xFF; p[16] = 0x03;                         /* HV1-3, EV3-5; no EDR eSCO */
    hfp_stats.sco_asked++;
    hci_queue(HCI_SETUP_SYNC, p, 17);
}

/* Synchronous Connection Complete: status, handle, address, link type, ..., air mode last. */
void hfp_sco_event(const u8 *e, u32 length)
{
    if (e[0] == HCI_EV_SYNC_COMPLETE && length >= 2 + 17) {
        hfp_stats.sco_status = e[2];
        if (!e[2]) {
            hfp_stats.sco_handle = (u32)(e[3] | (e[4] & 0x0F) << 8);
            hfp_stats.sco_air = e[2 + 16];
            hfp_stats.state = 3;
            sco_link(hfp_stats.sco_handle);
            hfp_stats.sco_transport = hci_sco(1);
        }
    } else if (e[0] == HCI_EV_DISCONNECTION_COMPLETE && length >= 6 && hfp_stats.state == 3 &&
               (u32)(e[3] | (e[4] & 0x0F) << 8) == hfp_stats.sco_handle) {
        hfp_stats.state = 2;
        hfp_stats.sco_handle = 0;
        hci_sco(0);
        sco_link(0);
    }
}

u32 hfp_voice(void)
{
    if (hfp_stats.state != 2)
        return 1;
    sco_wanted = 1;
    return 0;
}

void hfp_gone(volatile BT_CONN *c)
{
    if (dlc && dlc->conn == c)
        closed(dlc);
}
