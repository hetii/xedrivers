/* See rfcomm.h. Frames: address (DLCI, C/R, EA), control (SABM, UA, DM, DISC, UIH with the
 * P/F bit), a length of one or two bytes, with credit-based flow a credit byte on a UIH with P/F
 * set, the data, and the FCS (over address and control; with the length too but on UIH).
 * DLCI 0 carries the multiplexer's own commands: PN (parameters, and the offer of credits),
 * MSC (modem status), RPN and RLS answered as asked, the rest refused with NSC.
 *
 * Only the side that answered as responder is spoken here so far: a headset (or the PC as one)
 * opens the session and the DLC to the hands-free server channel, which is how headsets do it.
 */

#include "rfcomm.h"

#define PSM_RFCOMM      0x0003u

#define CTL_SABM        0x2Fu
#define CTL_UA          0x63u
#define CTL_DM          0x0Fu
#define CTL_DISC        0x43u
#define CTL_UIH         0xEFu
#define PF              0x10u

#define MCC_PN          0x20u
#define MCC_MSC         0x38u
#define MCC_RPN         0x24u
#define MCC_RLS         0x14u
#define MCC_TEST        0x08u
#define MCC_FCON        0x28u
#define MCC_FCOFF       0x18u
#define MCC_NSC         0x04u

#define CREDITS_GIVE    7u
#define OUR_MTU         127u

volatile RFCOMM_STATS rfcomm_stats;

static RFCOMM_DLC dlcs[RFCOMM_DLCS];
static struct {
    u32 channel;
    const RFCOMM_USER *user;
} servers[2];

static L2CAP_CHANNEL *session;          /* one link's multiplexer at a time */
static u32 session_up, initiator, cfc;
static u8 frame[OUR_MTU + 16];

/* The reflected CRC-8 of TS 07.10 (x^8 + x^2 + x + 1), by bits: frames are short. */
u8 rfcomm_fcs(const u8 *p, u32 n)
{
    u32 crc = 0xFF, i, b;

    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (b = 0; b < 8; b++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xE0u : crc >> 1;
    }
    return (u8)(0xFF - crc);
}

/* C/R in the address: set on commands from the initiator and on responses from the responder. */
static u32 address(u32 dlci, u32 command)
{
    u32 cr = command ? initiator : !initiator;

    return (dlci << 2) | (cr << 1) | 1u;
}

static void send(u32 dlci, u32 command, u32 control, const u8 *data, u32 n, s32 credits)
{
    u32 at = 0, i;

    if (!session || n > OUR_MTU)
        return;
    frame[at++] = (u8)address(dlci, command);
    frame[at++] = (u8)(control | (credits >= 0 ? PF : 0));
    frame[at++] = (u8)(n << 1 | 1u);               /* our frames are under 128 */
    if (credits >= 0)
        frame[at++] = (u8)credits;
    for (i = 0; i < n; i++)
        frame[at++] = data[i];
    frame[at] = rfcomm_fcs(frame, (control & ~PF) == CTL_UIH ? 2 : 3);
    at++;
    if (!l2cap_send(session, frame, at))
        rfcomm_stats.frames_out++;
}

static void control(u32 dlci, u32 command, u32 ctl)
{
    u32 at = 0;

    if (!session)
        return;
    frame[at++] = (u8)address(dlci, command);
    frame[at++] = (u8)ctl;
    frame[at++] = 1;                                /* no data */
    frame[at] = rfcomm_fcs(frame, 3);
    at++;
    if (!l2cap_send(session, frame, at))
        rfcomm_stats.frames_out++;
}

static void mcc(u32 type, u32 command, const u8 *v, u32 n)
{
    u8 m[16];
    u32 i;

    m[0] = (u8)(type << 2 | (command ? 2u : 0u) | 1u);
    m[1] = (u8)(n << 1 | 1u);
    for (i = 0; i < n && i < 14; i++)
        m[2 + i] = v[i];
    send(0, 1, CTL_UIH, m, 2 + n, -1);
}

static RFCOMM_DLC *find(u32 dlci)
{
    u32 i;

    for (i = 0; i < RFCOMM_DLCS; i++)
        if (dlcs[i].state && dlcs[i].dlci == dlci)
            return &dlcs[i];
    return 0;
}

static RFCOMM_DLC *make(u32 dlci)
{
    RFCOMM_DLC *d = find(dlci);
    u32 i;

    if (d)
        return d;
    for (i = 0; i < RFCOMM_DLCS; i++) {
        if (!dlcs[i].state) {
            d = &dlcs[i];
            d->state = 1;
            d->dlci = dlci;
            d->mtu = OUR_MTU;
            d->credits_out = 0;
            d->credits_given = 0;
            d->conn = session ? session->conn : 0;
            d->user = 0;
            return d;
        }
    }
    return 0;
}

static const RFCOMM_USER *server_for(u32 dlci)
{
    u32 i;

    for (i = 0; i < 2; i++)
        if (servers[i].user && servers[i].channel == dlci >> 1)
            return servers[i].user;
    return 0;
}

static void close_dlc(RFCOMM_DLC *d)
{
    u32 was = d->state == 2;

    d->state = 0;
    if (was && d->user && d->user->closed)
        d->user->closed(d);
}

/* --- the multiplexer's commands (DLCI 0) ---------------------------------------------- */

static void multiplexer(const u8 *p, u32 n)
{
    u32 type, command, len;
    const u8 *v;
    u8 r[8];
    RFCOMM_DLC *d;

    if (n < 2)
        return;
    type = p[0] >> 2;
    command = (p[0] >> 1) & 1u;
    len = p[1] >> 1;
    v = p + 2;
    if (len + 2 > n)
        return;
    switch (type) {
    case MCC_PN:                                    /* DLCI, CL, priority, T, N (2), NA, k */
        if (len < 8 || !command)
            return;
        d = make(v[0] & 0x3Fu);
        if (!d)
            return;
        d->mtu = (u32)(v[4] | v[5] << 8);
        if (d->mtu > OUR_MTU)
            d->mtu = OUR_MTU;
        cfc = (v[1] & 0xF0u) == 0xF0u;              /* credit-based flow offered */
        if (cfc)
            d->credits_out = v[7] & 7u;
        r[0] = v[0];
        r[1] = cfc ? 0xE0 : 0x00;
        r[2] = v[2];
        r[3] = 0;
        r[4] = (u8)d->mtu;
        r[5] = (u8)(d->mtu >> 8);
        r[6] = 0;
        r[7] = cfc ? CREDITS_GIVE : 0;
        if (cfc)
            d->credits_given = CREDITS_GIVE;
        mcc(MCC_PN, 0, r, 8);
        break;
    case MCC_MSC:                                   /* DLCI, signals */
        if (len < 2)
            return;
        if (command) {
            mcc(MCC_MSC, 0, v, len < 2 ? len : 2);  /* the same back, as the answer */
            d = find(v[0] >> 2);
            if (d && d->state == 1) {
                r[0] = v[0];
                r[1] = 0x8D;                        /* EA, RTC, RTR, DV */
                mcc(MCC_MSC, 1, r, 2);
                d->state = 2;
                rfcomm_stats.dlcs_opened++;
                if (d->user && d->user->open)
                    d->user->open(d);
            }
        }
        break;
    case MCC_RPN:
    case MCC_RLS:
    case MCC_TEST:
        if (command)
            mcc(type, 0, v, len > 14 ? 14 : len);
        break;
    case MCC_FCON:
    case MCC_FCOFF:
        if (command)
            mcc(type, 0, 0, 0);
        break;
    default:
        if (command) {
            r[0] = p[0];
            mcc(MCC_NSC, 0, r, 1);
        }
        break;
    }
}

/* --- frames in ------------------------------------------------------------------------ */

static void frame_in(L2CAP_CHANNEL *ch, const u8 *p, u32 n)
{
    u32 dlci, ctl, len, at, pf, header;
    RFCOMM_DLC *d;

    (void)ch;
    if (n < 4)
        return;
    rfcomm_stats.frames_in++;
    dlci = p[0] >> 2;
    ctl = p[1] & ~PF;
    pf = p[1] & PF;
    rfcomm_stats.last_control = p[1];
    if (p[2] & 1u) {
        len = p[2] >> 1;
        at = 3;
    } else {
        len = (u32)(p[2] >> 1) | (u32)p[3] << 7;
        at = 4;
    }
    header = at;
    if (rfcomm_fcs(p, ctl == CTL_UIH ? 2 : header) != p[n - 1]) {
        rfcomm_stats.bad_fcs++;
        return;
    }
    switch (ctl) {
    case CTL_SABM:
        if (dlci == 0) {
            session_up = 1;
            rfcomm_stats.sessions++;
            control(0, 0, CTL_UA | PF);
            return;
        }
        d = find(dlci);
        if (!d)
            d = make(dlci);
        if (d && server_for(dlci)) {
            d->user = server_for(dlci);
            control(dlci, 0, CTL_UA | PF);
            /* open once both MSCs went: theirs is answered in multiplexer() */
        } else {
            rfcomm_stats.refused++;
            if (d)
                d->state = 0;
            control(dlci, 0, CTL_DM | PF);
        }
        return;
    case CTL_DISC:
        control(dlci, 0, CTL_UA | PF);
        if (dlci == 0) {
            u32 i;

            for (i = 0; i < RFCOMM_DLCS; i++)
                if (dlcs[i].state)
                    close_dlc(&dlcs[i]);
            session_up = 0;
        } else if ((d = find(dlci)) != 0) {
            close_dlc(d);
        }
        return;
    case CTL_UA:
    case CTL_DM:
        return;
    case CTL_UIH:
        if (dlci == 0) {
            multiplexer(p + at, len);
            return;
        }
        d = find(dlci);
        if (!d)
            return;
        if (pf && cfc) {                            /* a credit byte first */
            d->credits_out += p[at];
            at++;
        }
        if (len && d->state == 2) {
            if (cfc && d->credits_given)
                d->credits_given--;
            if (d->user && d->user->data)
                d->user->data(d, p + at, len);
            /* give back what was used, a batch at a time */
            if (cfc && d->credits_given < CREDITS_GIVE / 2) {
                send(d->dlci, 1, CTL_UIH, 0, 0, (s32)(CREDITS_GIVE - d->credits_given));
                d->credits_given = CREDITS_GIVE;
            }
        }
        return;
    default:
        return;
    }
}

u32 rfcomm_send(RFCOMM_DLC *d, const u8 *data, u32 length)
{
    if (d->state != 2 || length > d->mtu)
        return 1;
    if (cfc) {
        if (!d->credits_out) {
            rfcomm_stats.no_credit++;
            return 1;
        }
        d->credits_out--;
    }
    send(d->dlci, 1, CTL_UIH, data, length, -1);
    return 0;
}

/* --- the L2CAP channel ---------------------------------------------------------------- */

static void l2_open(L2CAP_CHANNEL *ch)
{
    if (session && session != ch)
        return;                                     /* one at a time */
    session = ch;
    initiator = 0;                                  /* the other side opened it */
    session_up = 0;
    cfc = 0;
}

static void l2_closed(L2CAP_CHANNEL *ch, u32 was_open)
{
    u32 i;

    (void)was_open;
    if (ch != session)
        return;
    for (i = 0; i < RFCOMM_DLCS; i++)
        if (dlcs[i].state)
            close_dlc(&dlcs[i]);
    session = 0;
    session_up = 0;
}

static const L2CAP_USER rfcomm_user = { l2_open, frame_in, l2_closed };

void rfcomm_init(void)
{
    l2cap_listen(PSM_RFCOMM, &rfcomm_user, 0);
}

u32 rfcomm_listen(u32 channel, const RFCOMM_USER *u)
{
    u32 i;

    for (i = 0; i < 2; i++) {
        if (!servers[i].user || servers[i].channel == channel) {
            servers[i].channel = channel;
            servers[i].user = u;
            return 0;
        }
    }
    return 1;
}

void rfcomm_gone(volatile BT_CONN *c)
{
    (void)c;                                        /* l2_closed comes with the link */
}
