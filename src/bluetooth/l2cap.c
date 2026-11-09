/* L2CAP over the ACL links (Core 5.4 Vol 3 Part A), basic mode only, as every audio and HID
 * profile uses it: channels opened either way, configured for their MTU, and the signalling
 * channel's echo and information requests answered.
 *
 * ACL comes in reassembled per link: a packet starts an L2CAP frame (PB 10) or continues it
 * (PB 01), and the frame goes up when its own length is in. Out, a frame fits one ACL packet
 * (the controller takes 1021 bytes, our MTU is less) and is sent when there is a credit.
 */

#include "host.h"

#define CHANNELS        8u
#define FRAME_MAX       1024u           /* an L2CAP frame put together, header in */
#define OUR_MTU         1017u           /* fits one ACL packet of 1021 with its header */
#define FIRST_CID       0x0040u

#define SIG_REJECT          0x01u
#define SIG_CONN_REQ        0x02u
#define SIG_CONN_RSP        0x03u
#define SIG_CONF_REQ        0x04u
#define SIG_CONF_RSP        0x05u
#define SIG_DISC_REQ        0x06u
#define SIG_DISC_RSP        0x07u
#define SIG_ECHO_REQ        0x08u
#define SIG_ECHO_RSP        0x09u
#define SIG_INFO_REQ        0x0Au
#define SIG_INFO_RSP        0x0Bu

#define CONF_OURS_DONE      1u          /* our request accepted */
#define CONF_THEIRS_DONE    2u          /* theirs answered */

volatile L2CAP_STATS l2cap_stats;
L2CAP_CHANNEL l2cap_channels[CHANNELS];

static struct {
    u16 psm;
    const L2CAP_USER *user;
    void *ctx;
} listening[6];

static u8 frame[BT_CONN_MAX][FRAME_MAX];
static u32 frame_have[BT_CONN_MAX];
static u8 out[FRAME_MAX + 4];
static u8 ident;

static u16 le16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static void put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static u32 conn_index(volatile BT_CONN *c)
{
    return (u32)(c - bt_conns);
}

/* --- out ------------------------------------------------------------------------------ */

/* One L2CAP frame to `cid` on `c`: `a` then `b` (either may be empty). 0 if sent. */
static u32 send_frame(volatile BT_CONN *c, u32 cid, const u8 *a, u32 na, const u8 *b, u32 nb)
{
    u32 i;

    if (na + nb > FRAME_MAX - 4 || c->state < BT_CONN_UP)
        return 1;
    put16(out, na + nb);
    put16(out + 2, cid);
    for (i = 0; i < na; i++)
        out[4 + i] = a[i];
    for (i = 0; i < nb; i++)
        out[4 + na + i] = b[i];
    if (hci_acl_send(c->handle, out, 4 + na + nb)) {
        l2cap_stats.send_failures++;
        return 1;
    }
    l2cap_stats.frames_out++;
    return 0;
}

static u32 signal(volatile BT_CONN *c, u32 code, u32 id, const u8 *data, u32 n)
{
    u8 h[4];

    h[0] = (u8)code;
    h[1] = (u8)id;
    put16(h + 2, n);
    l2cap_stats.signals_out++;
    return send_frame(c, 0x0001, h, 4, data, n);
}

static u32 next_ident(void)
{
    if (++ident == 0)
        ident = 1;
    return ident;
}

/* --- channels ------------------------------------------------------------------------- */

static L2CAP_CHANNEL *by_local(volatile BT_CONN *c, u32 cid)
{
    u32 i;

    for (i = 0; i < CHANNELS; i++)
        if (l2cap_channels[i].state != L2CAP_FREE && l2cap_channels[i].conn == c &&
            l2cap_channels[i].local_cid == cid)
            return &l2cap_channels[i];
    return 0;
}

static L2CAP_CHANNEL *channel_new(volatile BT_CONN *c, u32 psm, const L2CAP_USER *u,
                                  void *ctx)
{
    u32 i, k;

    for (i = 0; i < CHANNELS; i++) {
        L2CAP_CHANNEL *ch = &l2cap_channels[i];

        if (ch->state == L2CAP_FREE) {
            for (k = 0; k < sizeof(*ch); k++)
                ((u8 *)ch)[k] = 0;
            ch->conn = c;
            ch->psm = (u16)psm;
            ch->local_cid = (u16)(FIRST_CID + i);
            ch->remote_mtu = 672;           /* the default until told */
            ch->user = u;
            ch->ctx = ctx;
            return ch;
        }
    }
    l2cap_stats.no_channel++;
    return 0;
}

static void conf_request(L2CAP_CHANNEL *ch)
{
    u8 p[8];

    put16(p, ch->remote_cid);
    put16(p + 2, 0);                        /* flags */
    p[4] = 0x01;                            /* MTU option */
    p[5] = 2;
    put16(p + 6, OUR_MTU);
    ch->state = L2CAP_CONFIG;
    signal(ch->conn, SIG_CONF_REQ, next_ident(), p, 8);
}

static void maybe_open(L2CAP_CHANNEL *ch)
{
    if (ch->state == L2CAP_CONFIG &&
        (ch->config & (CONF_OURS_DONE | CONF_THEIRS_DONE)) ==
        (CONF_OURS_DONE | CONF_THEIRS_DONE)) {
        ch->state = L2CAP_OPEN;
        l2cap_stats.opened++;
        if (ch->user && ch->user->open)
            ch->user->open(ch);
    }
}

static void channel_free(L2CAP_CHANNEL *ch)
{
    u32 was_open = ch->state == L2CAP_OPEN;

    ch->state = L2CAP_FREE;
    if (ch->user && ch->user->closed)
        ch->user->closed(ch, was_open);
}

u32 l2cap_listen(u32 psm, const L2CAP_USER *u, void *ctx)
{
    u32 i;

    for (i = 0; i < sizeof(listening) / sizeof(listening[0]); i++) {
        if (!listening[i].psm || listening[i].psm == psm) {
            listening[i].psm = (u16)psm;
            listening[i].user = u;
            listening[i].ctx = ctx;
            return 0;
        }
    }
    return 1;
}

L2CAP_CHANNEL *l2cap_open(volatile BT_CONN *c, u32 psm, const L2CAP_USER *u, void *ctx)
{
    L2CAP_CHANNEL *ch;
    u8 p[4];

    if (c->state != BT_CONN_SECURE)
        return 0;
    ch = channel_new(c, psm, u, ctx);
    if (!ch)
        return 0;
    put16(p, psm);
    put16(p + 2, ch->local_cid);
    ch->state = L2CAP_CONNECTING;
    ch->ident = (u8)next_ident();
    if (signal(c, SIG_CONN_REQ, ch->ident, p, 4)) {
        ch->state = L2CAP_FREE;
        return 0;
    }
    return ch;
}

u32 l2cap_send(L2CAP_CHANNEL *ch, const u8 *data, u32 length)
{
    if (ch->state != L2CAP_OPEN || length > ch->remote_mtu)
        return 1;
    return send_frame(ch->conn, ch->remote_cid, data, length, 0, 0);
}

void l2cap_close(L2CAP_CHANNEL *ch)
{
    u8 p[4];

    if (ch->state == L2CAP_FREE)
        return;
    put16(p, ch->remote_cid);
    put16(p + 2, ch->local_cid);
    ch->state = L2CAP_CLOSING;
    signal(ch->conn, SIG_DISC_REQ, next_ident(), p, 4);
}

u32 l2cap_echo(volatile BT_CONN *c)
{
    static const u8 hello[4] = { 'x', 'e', 'd', 'r' };

    l2cap_stats.echo_asked++;
    return signal(c, SIG_ECHO_REQ, next_ident(), hello, 4);
}

/* --- signalling in -------------------------------------------------------------------- */

static void conn_request(volatile BT_CONN *c, u32 id, const u8 *d, u32 n)
{
    L2CAP_CHANNEL *ch = 0;
    u32 psm, i, result = 0x0002;            /* PSM not supported */
    u8 p[8];

    if (n < 4)
        return;
    psm = le16(d);
    for (i = 0; i < sizeof(listening) / sizeof(listening[0]); i++)
        if (listening[i].psm && listening[i].psm == psm)
            break;
    if (i < sizeof(listening) / sizeof(listening[0])) {
        ch = channel_new(c, psm, listening[i].user, listening[i].ctx);
        result = ch ? 0x0000 : 0x0004;      /* or no resources */
    }
    l2cap_stats.incoming++;
    if (result) {
        l2cap_stats.refused++;
        l2cap_stats.last_refused_psm = psm;
    }
    put16(p, ch ? ch->local_cid : 0);
    put16(p + 2, le16(d + 2));
    put16(p + 4, result);
    put16(p + 6, 0);
    signal(c, SIG_CONN_RSP, id, p, 8);
    if (ch) {
        ch->remote_cid = le16(d + 2);
        conf_request(ch);
    }
}

static void conn_response(volatile BT_CONN *c, const u8 *d, u32 n)
{
    L2CAP_CHANNEL *ch;
    u32 result;

    if (n < 8)
        return;
    ch = by_local(c, le16(d + 2));
    if (!ch || ch->state != L2CAP_CONNECTING)
        return;
    result = le16(d + 4);
    if (result == 0x0001)                   /* pending: another answer follows */
        return;
    if (result) {
        ch->last_result = result;
        l2cap_stats.refused++;
        channel_free(ch);
        return;
    }
    ch->remote_cid = le16(d);
    conf_request(ch);
}

/* Their configuration request: we take any MTU (ours is what we send by) and refuse
 * nothing -- basic mode, the defaults for the rest. */
static void conf_request_in(volatile BT_CONN *c, u32 id, const u8 *d, u32 n)
{
    L2CAP_CHANNEL *ch;
    u32 at = 4;
    u8 p[6];

    if (n < 4)
        return;
    ch = by_local(c, le16(d));
    if (!ch)
        return;
    while (at + 2 <= n) {
        u32 type = d[at] & 0x7F, len = d[at + 1];

        if (at + 2 + len > n)
            break;
        if (type == 0x01 && len >= 2)
            ch->remote_mtu = le16(d + at + 2);
        at += 2 + len;
    }
    if (ch->remote_mtu > FRAME_MAX - 4)
        ch->remote_mtu = FRAME_MAX - 4;
    put16(p, ch->remote_cid);
    put16(p + 2, 0);
    put16(p + 4, 0);                        /* success */
    signal(c, SIG_CONF_RSP, id, p, 6);
    ch->config |= CONF_THEIRS_DONE;
    maybe_open(ch);
}

static void conf_response_in(volatile BT_CONN *c, const u8 *d, u32 n)
{
    L2CAP_CHANNEL *ch;

    if (n < 6)
        return;
    ch = by_local(c, le16(d));
    if (!ch)
        return;
    if (le16(d + 4)) {                      /* they did not take it: give up on it */
        ch->last_result = 0x100u | le16(d + 4);
        l2cap_close(ch);
        return;
    }
    ch->config |= CONF_OURS_DONE;
    maybe_open(ch);
}

static void info_request(volatile BT_CONN *c, u32 id, const u8 *d, u32 n)
{
    u8 p[12];
    u32 type, len = 4;

    if (n < 2)
        return;
    type = le16(d);
    put16(p, type);
    put16(p + 2, 0);
    if (type == 0x0002) {                   /* extended features: fixed channels only */
        put16(p + 4, 0x0080);
        put16(p + 6, 0);
        len = 8;
    } else if (type == 0x0003) {            /* fixed channels: signalling */
        p[4] = 0x02;
        for (n = 5; n < 12; n++)
            p[n] = 0;
        len = 12;
    } else {
        put16(p + 2, 1);                    /* not supported */
    }
    signal(c, SIG_INFO_RSP, id, p, len);
}

static void signalling(volatile BT_CONN *c, const u8 *d, u32 n)
{
    while (n >= 4) {
        u32 code = d[0], id = d[1], len = le16(d + 2);
        const u8 *p = d + 4;
        L2CAP_CHANNEL *ch;
        u8 r[4];

        if (len + 4 > n)
            return;
        l2cap_stats.signals_in++;
        switch (code) {
        case SIG_CONN_REQ: conn_request(c, id, p, len); break;
        case SIG_CONN_RSP: conn_response(c, p, len); break;
        case SIG_CONF_REQ: conf_request_in(c, id, p, len); break;
        case SIG_CONF_RSP: conf_response_in(c, p, len); break;
        case SIG_DISC_REQ:
            if (len >= 4) {
                ch = by_local(c, le16(p));
                r[0] = p[0]; r[1] = p[1]; r[2] = p[2]; r[3] = p[3];
                signal(c, SIG_DISC_RSP, id, r, 4);
                if (ch)
                    channel_free(ch);
            }
            break;
        case SIG_DISC_RSP:
            if (len >= 4 && (ch = by_local(c, le16(p + 2))) != 0)
                channel_free(ch);
            break;
        case SIG_ECHO_REQ: signal(c, SIG_ECHO_RSP, id, p, len); break;
        case SIG_ECHO_RSP: l2cap_stats.echo_answered++; break;
        case SIG_INFO_REQ: info_request(c, id, p, len); break;
        case SIG_REJECT: l2cap_stats.rejected++; break;
        default:
            r[0] = 0; r[1] = 0;             /* command not understood */
            signal(c, SIG_REJECT, id, r, 2);
            break;
        }
        d += 4 + len;
        n -= 4 + len;
    }
}

/* A whole L2CAP frame from link `c`. */
static void frame_in(volatile BT_CONN *c, const u8 *f, u32 n)
{
    u32 cid = le16(f + 2);
    L2CAP_CHANNEL *ch;

    l2cap_stats.frames_in++;
    if (cid == 0x0001) {
        signalling(c, f + 4, n - 4);
        return;
    }
    ch = by_local(c, cid);
    if (ch && ch->state == L2CAP_OPEN && ch->user && ch->user->data)
        ch->user->data(ch, f + 4, n - 4);
    else
        l2cap_stats.no_channel_in++;
}

/* --- for hci.c ------------------------------------------------------------------------ */

void l2cap_acl(const u8 *packet, u32 length)
{
    volatile BT_CONN *c;
    u32 handle, pb, n, i, at, need;

    if (length < 4)
        return;
    handle = (u32)(packet[0] | (packet[1] & 0x0F) << 8);
    pb = (packet[1] >> 4) & 3u;
    n = le16(packet + 2);
    if (n + 4 > length)
        return;
    c = bt_conn_by_handle(handle);
    if (!c)
        return;
    at = conn_index(c);
    if (pb == 0x2 || pb == 0x0)             /* a start: whatever was there is lost */
        frame_have[at] = 0;
    else if (!frame_have[at])
        return;                             /* a continuation of nothing */
    for (i = 0; i < n && frame_have[at] < FRAME_MAX; i++)
        frame[at][frame_have[at]++] = packet[4 + i];
    if (frame_have[at] < 4)
        return;
    need = 4u + le16(frame[at]);
    if (need > FRAME_MAX) {
        l2cap_stats.too_long++;
        frame_have[at] = 0;
        return;
    }
    if (frame_have[at] >= need) {
        frame_in(c, frame[at], need);
        frame_have[at] = 0;
    }
}

void bt_l2cap_ready(volatile BT_CONN *c)
{
    l2cap_echo(c);                          /* a first frame both ways: the ACL path works */
    bt_profiles_ready(c);
}

void bt_l2cap_gone(volatile BT_CONN *c)
{
    u32 i;

    frame_have[conn_index(c)] = 0;
    for (i = 0; i < CHANNELS; i++)
        if (l2cap_channels[i].state != L2CAP_FREE && l2cap_channels[i].conn == c)
            channel_free(&l2cap_channels[i]);
    bt_profiles_gone(c);
}
