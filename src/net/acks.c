/* See acks.h. The first half is pure (tests/net/test_acks.c). */

#include "acks.h"

static u32 rd16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

u32 ack_kind(const u8 *frame, u32 length, u32 *peer_ip, u32 *ports, u32 *number)
{
    const u8 *ip = frame + ETH_HLEN, *tcp;
    u32 ihl, doff, total, at;

    if (length < ETH_HLEN + 40 || rd16(frame + 12) != 0x0800 || (ip[0] >> 4) != 4 ||
        ip[9] != 6 || (rd16(ip + 6) & 0x3FFFu))
        return 0;
    ihl = (ip[0] & 0x0Fu) * 4;
    total = rd16(ip + 2);
    if (ihl < 20 || total < ihl + 20 || ETH_HLEN + total > length)
        return 0;
    tcp = ip + ihl;
    doff = (u32)(tcp[12] >> 4) * 4;
    if (doff < 20 || ihl + doff > total)
        return 0;
    *peer_ip = rd32(ip + 16);
    *ports = rd16(tcp) << 16 | rd16(tcp + 2);
    *number = rd32(tcp + 8);
    if (tcp[13] != 0x10 || total != ihl + doff || ETH_HLEN + total > ACK_FRAME)
        return 1;                               /* data, SYN, FIN, RST, PSH, URG */
    for (at = 20; at < doff; ) {                /* options: SACK blocks say what is missing */
        u32 kind = tcp[at];

        if (kind == 0)
            break;
        if (kind == 1) {
            at++;
            continue;
        }
        if (kind == 5 || at + 1 >= doff || tcp[at + 1] < 2)
            return 1;
        at += tcp[at + 1];
    }
    return 2;
}

static ACK_FLOW *place(ACK_TABLE *t, u32 peer_ip, u32 ports)
{
    u32 h = peer_ip ^ ports * 0x9E3779B1u;

    return &t->flow[(h ^ h >> 15) & (ACK_FLOWS - 1)];
}

static void let_go(ACK_TABLE *t, ACK_FLOW *f)
{
    if (f->held) {
        f->held = 0;
        t->held_now--;
    }
}

u32 ack_out(ACK_TABLE *t, const u8 *frame, u32 length, u32 thin, u32 now, u32 hold,
            ACK_FLOW **flush)
{
    u32 peer, ports, number, kind = ack_kind(frame, length, &peer, &ports, &number), i;
    ACK_FLOW *f;

    *flush = 0;
    if (!kind)
        return 0;
    f = place(t, peer, ports);
    if (f->peer_ip != peer || f->ports != ports) {
        if (kind != 2 || thin < 2)
            return 0;                           /* nothing of this connection is kept */
        if (f->held) {
            *flush = f;                         /* the caller sends it, then asks again */
            return 0;
        }
        f->peer_ip = peer;
        f->ports = ports;
        f->last = number - 1;
        f->skipped = 0;
        f->push = 0;
        f->sent_at = now - hold - 1;            /* its first goes at once */
    }
    if (kind == 1 || thin < 2 || number == f->last) {
        /* what is held is older than this frame and says less: gone */
        let_go(t, f);
        f->skipped = 0;
        f->sent_at = now;
        if (kind == 2)
            f->last = number;
        return 0;
    }
    f->last = number;
    if (f->push || f->skipped + 1 >= thin || now - f->sent_at > hold) {
        f->push = 0;
        let_go(t, f);
        f->skipped = 0;
        f->sent_at = now;
        return 0;
    }
    if (length > ACK_FRAME)
        length = ACK_FRAME;                     /* padding: the frame itself is shorter */
    for (i = 0; i < length; i++)
        f->frame[i] = frame[i];
    if (!f->held)
        t->held_now++;
    f->held = length;
    f->skipped++;
    return 1;
}

void ack_in(ACK_TABLE *t, const u8 *frame, u32 length)
{
    const u8 *ip = frame + ETH_HLEN, *tcp;
    u32 ihl, peer, ports;
    ACK_FLOW *f;

    if (length < ETH_HLEN + 40 || rd16(frame + 12) != 0x0800 || (ip[0] >> 4) != 4 ||
        ip[9] != 6 || (rd16(ip + 6) & 0x1FFFu))
        return;
    ihl = (ip[0] & 0x0Fu) * 4;
    if (ihl < 20 || length < ETH_HLEN + ihl + 20)
        return;
    tcp = ip + ihl;
    if (!(tcp[13] & 0x09u))                     /* PSH or FIN */
        return;
    peer = rd32(ip + 12);
    ports = rd16(tcp + 2) << 16 | rd16(tcp);    /* ours, the peer's: as on the way out */
    f = place(t, peer, ports);
    if (f->peer_ip == peer && f->ports == ports)
        f->push = 1;
}

ACK_FLOW *ack_due(ACK_TABLE *t, u32 now, u32 hold, u32 force)
{
    u32 i;

    if (!t->held_now)
        return 0;
    for (i = 0; i < ACK_FLOWS; i++) {
        ACK_FLOW *f = &t->flow[i];

        if (f->held && (force || now - f->sent_at > hold))
            return f;
    }
    return 0;
}

void ack_sent(ACK_TABLE *t, ACK_FLOW *flow, u32 now)
{
    let_go(t, flow);
    flow->skipped = 0;
    flow->sent_at = now;
}

#if defined(__powerpc__)

#include "file.h"

#define TB_PER_US 50u           /* the time base, near enough: 49.875 MHz */

volatile ACK_STATS net_ack_stats;
volatile u32 net_ack_thin = ACK_THIN;
volatile u32 net_ack_holding;
volatile u32 net_ack_hold_us = ACK_HOLD_US;

static ACK_TABLE table;
static KSPIN_LOCK lock;

static u32 tb(void)
{
    u32 t;

    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

void acks_init(void)
{
    char value[8];
    u32 n = 0, i;

    if (!file_setting("xedrivers.ini", "ack_thin", value, sizeof(value)))
        return;
    for (i = 0; value[i] >= '0' && value[i] <= '9'; i++)
        n = n * 10 + (u32)(value[i] - '0');
    net_ack_thin = n > 64 ? 64 : n;
}

/* A held one out, the lock not held while the card has it. 1 if it went. */
static u32 send_held(ACK_FLOW *f, u32 (*send)(const u8 *, u32), u32 irql)
{
    u8 copy[ACK_FRAME];
    u32 n = f->held, i;

    for (i = 0; i < n; i++)
        copy[i] = f->frame[i];
    ack_sent(&table, f, tb());
    net_ack_holding = table.held_now;
    KfReleaseSpinLock(&lock, irql);
    return send(copy, n);
}

u32 acks_tx(const u8 *frame, u32 length, u32 (*send)(const u8 *, u32))
{
    u32 thin = net_ack_thin, peer, ports, number, irql, kept, kind;
    ACK_FLOW *flush;

    if (thin < 2 && !net_ack_holding)
        return 0;
    kind = ack_kind(frame, length, &peer, &ports, &number);
    if (!kind)
        return 0;
    for (;;) {
        irql = KfAcquireSpinLock(&lock);
        kept = ack_out(&table, frame, length, thin, tb(), net_ack_hold_us * TB_PER_US, &flush);
        if (!flush)
            break;
        send_held(flush, send, irql);           /* releases the lock */
    }
    net_ack_holding = table.held_now;
    if (kind == 2) {
        net_ack_stats.seen++;
        if (!kept)
            net_ack_stats.sent++;
    }
    KfReleaseSpinLock(&lock, irql);
    return kept;
}

/* No lock: one word written, and a push missed costs one acknowledgement its hold. */
void acks_rx(const u8 *frame, u32 length)
{
    ack_in(&table, frame, length);
}

void acks_flush(u32 (*send)(const u8 *, u32), u32 force)
{
    for (;;) {
        u32 irql = KfAcquireSpinLock(&lock);
        ACK_FLOW *f = ack_due(&table, tb(), net_ack_hold_us * TB_PER_US, force);

        if (!f) {
            KfReleaseSpinLock(&lock, irql);
            return;
        }
        net_ack_stats.late++;
        if (!send_held(f, send, irql))
            return;                             /* the card is full: xnet's own will follow */
    }
}

#endif /* __powerpc__ */
