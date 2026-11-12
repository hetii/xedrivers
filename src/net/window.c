/* See window.h. The first half is pure (tests/net/test_window.c). */

#include "window.h"

static u32 rd16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

typedef struct {
    u32 peer_ip, ports;         /* as the connection is kept: ours << 16 | the peer's */
    u32 seq, ack, flags, window, payload;
    u32 tcp;                    /* offset of the TCP header in the frame */
    u32 scale_at;               /* offset of the window scale option's value, 0: none */
} SEG;

#define F_FIN 0x01u
#define F_SYN 0x02u
#define F_RST 0x04u
#define F_ACK 0x10u

/* TCP over IPv4, not a fragment. `out`: from xnet (the peer is the destination). */
static u32 parse(const u8 *frame, u32 length, u32 out, SEG *s)
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
    s->peer_ip = rd32(ip + (out ? 16 : 12));
    s->ports = out ? rd16(tcp) << 16 | rd16(tcp + 2) : rd16(tcp + 2) << 16 | rd16(tcp);
    s->seq = rd32(tcp + 4);
    s->ack = rd32(tcp + 8);
    s->flags = tcp[13];
    s->window = rd16(tcp + 14);
    s->payload = total - ihl - doff;
    s->tcp = ETH_HLEN + ihl;
    s->scale_at = 0;
    if (s->flags & F_SYN)
        for (at = 20; at + 1 < doff; ) {
            u32 kind = tcp[at];

            if (kind == 0)
                break;
            if (kind == 1) {
                at++;
                continue;
            }
            if (tcp[at + 1] < 2)
                break;
            if (kind == 3 && tcp[at + 1] == 3 && at + 2 < doff)
                s->scale_at = s->tcp + at + 2;
            at += tcp[at + 1];
        }
    return 1;
}

static WIN_FLOW *place(WIN_TABLE *t, u32 peer_ip, u32 ports)
{
    u32 h = peer_ip ^ ports * 0x9E3779B1u;

    return &t->flow[(h ^ h >> 15) & (WIN_FLOWS - 1)];
}

static WIN_FLOW *find(WIN_TABLE *t, const SEG *s)
{
    WIN_FLOW *f = place(t, s->peer_ip, s->ports);

    return f->ports && f->peer_ip == s->peer_ip && f->ports == s->ports ? f : 0;
}

void win_free(WIN_TABLE *t, u32 slot)
{
    t->next[slot] = t->free;
    t->free = (u16)slot;
}

/* What a connection holds is let go; 1 for each is counted by the caller's statistics. */
static u32 forget(WIN_TABLE *t, WIN_FLOW *f)
{
    u32 n = 0;

    while (f->first != WIN_NONE) {
        u32 slot = f->first;

        f->first = t->next[slot];
        win_free(t, slot);
        t->held_now--;
        n++;
    }
    f->last = WIN_NONE;
    f->held_bytes = 0;
    f->ports = 0;
    t->dropped += n;
    return n;
}

void win_reset(WIN_TABLE *t, u8 *pool)
{
    u32 i;

    for (i = 0; i < WIN_FLOWS; i++) {
        t->flow[i].ports = 0;
        t->flow[i].first = t->flow[i].last = WIN_NONE;
        t->flow[i].held_bytes = 0;
    }
    t->pool = pool;
    t->free = WIN_NONE;
    for (i = WIN_FRAMES; i-- > 0; )
        win_free(t, i);
    t->held_now = t->held_most = 0;
    t->opened = t->not_taken = t->dropped = 0;
    t->busy = 0;
}

/* What one connection may be told, and may have kept, beyond xnet's edge. */
static u32 share(const WIN_TABLE *t, u32 extra)
{
    u32 room = WIN_FRAMES * 1400u / (t->busy ? t->busy : 1u);

    return extra < room ? extra : room;
}

/* The ones'-complement sum at `sum` after the 16-bit word `old` became `new` (RFC 1624). */
static void resum(u8 *sum, u32 old, u32 new)
{
    u32 c = (~rd16(sum) & 0xFFFFu) + (~old & 0xFFFFu) + new;

    c = (c & 0xFFFFu) + (c >> 16);
    c = (c & 0xFFFFu) + (c >> 16);
    c = ~c & 0xFFFFu;
    sum[0] = (u8)(c >> 8);
    sum[1] = (u8)c;
}

/* Two bytes of the frame at an even offset of the TCP header take a new value; the
 * checksum follows, and both are remembered. */
static void change(u8 *frame, const SEG *s, u32 at, u32 value, WIN_PATCH *p)
{
    u8 *sum = frame + s->tcp + 16;
    u32 old = rd16(frame + at);

    if (!p->n) {                            /* the checksum as it was, once */
        p->at[0] = (u16)(s->tcp + 16);
        p->old[0][0] = sum[0];
        p->old[0][1] = sum[1];
        p->n = 1;
    }
    p->at[p->n] = (u16)at;
    p->old[p->n][0] = frame[at];
    p->old[p->n][1] = frame[at + 1];
    p->n++;
    frame[at] = (u8)(value >> 8);
    frame[at + 1] = (u8)value;
    resum(sum, old, value);
}

void win_restore(u8 *frame, const WIN_PATCH *p)
{
    u32 i;

    for (i = 0; i < p->n; i++) {
        frame[p->at[i]] = p->old[i][0];
        frame[p->at[i] + 1] = p->old[i][1];
    }
}

/* xnet opens or answers a connection: a place for it, and its window scale replaced. */
static void opening(WIN_TABLE *t, u8 *frame, const SEG *s, u32 now, WIN_PATCH *p, u32 *taken)
{
    WIN_FLOW *f = place(t, s->peer_ip, s->ports);

    *taken = 0;
    if (f->ports && (f->peer_ip != s->peer_ip || f->ports != s->ports) &&
        (f->first != WIN_NONE || now - f->seen < WIN_IDLE)) {
        t->not_taken++;
        return;                             /* another's, and that one is alive */
    }
    if (f->ports)
        forget(t, f);
    f->peer_ip = s->peer_ip;
    f->ports = s->ports;
    f->seen = f->moved = now;
    f->shift = f->told_shift = 0;
    f->edge_known = 0;
    f->fins = 0;
    f->data = now - WIN_BUSY;
    f->grant = 0xFFFFFFFFu;                 /* all of `extra` until frames are kept */
    f->widest = 0;
    f->kept = 0;
    t->opened++;
    *taken = 1;
    if (s->scale_at) {
        u32 even = (s->scale_at - s->tcp) & ~1u, word;

        f->shift = frame[s->scale_at];
        if (f->shift < WIN_SHIFT) {
            f->told_shift = WIN_SHIFT;
            word = rd16(frame + s->tcp + even);
            word = (s->scale_at - s->tcp) & 1u ? (word & 0xFF00u) | WIN_SHIFT
                                               : (word & 0x00FFu) | WIN_SHIFT << 8;
            change(frame, s, s->tcp + even, word, p);
        } else {
            f->told_shift = f->shift;
        }
    }
    if (s->flags & F_ACK) {                 /* a SYN's own window is never scaled */
        f->edge = f->told = s->ack + s->window;
        f->edge_known = 1;
    }
}

WIN_FLOW *win_out(WIN_TABLE *t, u8 *frame, u32 length, u32 extra, u32 now, WIN_PATCH *p)
{
    SEG s;
    WIN_FLOW *f;
    u32 edge, want, field, taken;

    p->n = 0;
    if (!parse(frame, length, 1, &s))
        return 0;
    if (s.flags & F_SYN) {
        opening(t, frame, &s, now, p, &taken);
        return 0;
    }
    f = find(t, &s);
    if (!f)
        return 0;
    f->seen = now;
    if (s.flags & F_RST) {
        forget(t, f);
        return 0;
    }
    if (!(s.flags & F_ACK))
        return 0;
    edge = s.ack + (s.window << f->shift);
    if (s.window << f->shift > f->widest)
        f->widest = s.window << f->shift;
    if (!f->edge_known || (s32)(edge - f->edge) > 0) {
        if (!f->edge_known)
            f->told = edge;
        f->edge = edge;
        f->edge_known = 1;
        f->moved = now;
    }
    extra = share(t, extra);
    if (f->grant < extra)
        extra = f->grant;
    else
        f->grant = extra;
    want = f->edge + (extra > f->held_bytes ? extra - f->held_bytes : 0);
    if ((s32)(want - f->told) > 0)
        f->told = want;
    field = (f->told - s.ack) >> f->told_shift;
    if (field > 0xFFFFu) {
        field = 0xFFFFu;
        f->told = s.ack + (field << f->told_shift);
    }
    if (field != s.window)
        change(frame, &s, s.tcp + 14, field, p);
    if (s.flags & F_FIN)
        f->fins |= 1;
    if (f->fins == 3 && f->first == WIN_NONE) {
        f->ports = 0;                       /* closed both ways: the place is free */
        return 0;
    }
    return f->first != WIN_NONE && (s32)(t->end[f->first] - f->edge) <= 0 ? f : 0;
}

u32 win_in(WIN_TABLE *t, const u8 *frame, u32 length, u32 extra, u32 now)
{
    SEG s;
    WIN_FLOW *f;
    u32 end, slot, i;
    u8 *to;

    if (!parse(frame, length, 0, &s))
        return 0;
    f = find(t, &s);
    if (!f)
        return 0;
    f->seen = now;
    if (s.flags & F_RST) {
        forget(t, f);
        return 0;
    }
    if (s.flags & F_SYN) {
        /* the peer's answer to a connection xnet opened: without a window scale in it
         * neither side scales */
        if (!s.scale_at)
            f->shift = f->told_shift = 0;
        return 0;
    }
    end = s.seq + s.payload + (s.flags & F_FIN ? 1u : 0u);
    if (s.payload) {
        if (now - f->data >= WIN_BUSY)
            t->busy++;                      /* counted at once: its share shrinks the others' */
        f->data = now;
    }
    if (s.flags & F_FIN)
        f->fins |= 2;
    if (!f->edge_known || end == s.seq || (s32)(end - f->edge) <= 0)
        return 0;                           /* fits, or carries nothing */
    if (!t->pool || t->free == WIN_NONE || length > WIN_FRAME ||
        f->held_bytes + s.payload > extra)
        return 2;                           /* not kept: xnet may drop it */
    slot = t->free;
    t->free = t->next[slot];
    to = t->pool + slot * WIN_FRAME;
    for (i = 0; i < length; i++)
        to[i] = frame[i];
    t->end[slot] = end;
    t->length[slot] = (u16)length;
    t->payload[slot] = (u16)s.payload;
    t->next[slot] = WIN_NONE;
    if (f->last == WIN_NONE) {
        f->first = (u16)slot;
        f->moved = now;
    } else {
        t->next[f->last] = (u16)slot;
    }
    f->last = (u16)slot;
    f->held_bytes += s.payload;
    f->kept++;
    if (++t->held_now > t->held_most)
        t->held_most = t->held_now;
    return 1;
}

u32 win_take(WIN_TABLE *t, WIN_FLOW *f, u32 force)
{
    u32 slot = f->first;

    if (slot == WIN_NONE || (!force && (s32)(t->end[slot] - f->edge) > 0))
        return WIN_NONE;
    f->first = t->next[slot];
    if (f->first == WIN_NONE)
        f->last = WIN_NONE;
    f->held_bytes -= t->payload[slot];
    if (f->fins == 3 && f->first == WIN_NONE)
        f->ports = 0;
    t->held_now--;
    return slot;
}

WIN_FLOW *win_stale(WIN_TABLE *t, u32 now)
{
    u32 i;

    WIN_FLOW *stale = 0;
    u32 busy = 0;

    for (i = 0; i < WIN_FLOWS; i++) {
        WIN_FLOW *f = &t->flow[i];

        if (!f->ports)
            continue;
        if (now - f->data < WIN_BUSY)
            busy++;
        if (f->kept) {
            if (f->widest > WIN_SMALL)
                f->grant = 0;
            f->kept = 0;
        } else if (f->grant < 0xFFFFFFFFu - WIN_GROW) {
            f->grant += WIN_GROW;
        }
        if (!stale && f->first != WIN_NONE && now - f->moved >= WIN_PROBE) {
            f->moved = now;
            stale = f;
        }
    }
    t->busy = busy;
    return stale;
}

/* === the plugin's ============================================================ */
#if defined(__powerpc__)

#include "usb.h"
#include "file.h"

volatile WIN_STATS net_win_stats;
volatile u32 net_win_extra_kb = WIN_EXTRA_KB;

static WIN_TABLE table;
static KSPIN_LOCK lock;
static KDPC release_dpc;
static volatile u32 release_queued, releasing, ticks;
static WIN_FLOW *volatile ready[WIN_FLOWS];     /* connections whose held frames fit now */

/* Hand up what fits of every connection marked; one at a time does this (the receive
 * path's processor, by a frame coming in or the DPC), so the order holds. */
static void release(void)
{
    u32 i;

    if (__sync_lock_test_and_set(&releasing, 1))
        return;
    for (i = 0; i < WIN_FLOWS; i++) {
        WIN_FLOW *f = ready[i];
        u32 force = (u32)f & 1u;

        if (!f)
            continue;
        ready[i] = 0;
        f = (WIN_FLOW *)((u32)f & ~1u);
        for (;;) {
            u32 irql = KfAcquireSpinLock(&lock), slot = win_take(&table, f, force);

            net_win_stats.held_now = table.held_now;
            KfReleaseSpinLock(&lock, irql);
            if (slot == WIN_NONE)
                break;
            if (force)
                net_win_stats.probed++;
            else
                net_win_stats.released++;
            if (net_rx)
                net_rx(table.pool + slot * WIN_FRAME, table.length[slot]);
            irql = KfAcquireSpinLock(&lock);
            win_free(&table, slot);
            KfReleaseSpinLock(&lock, irql);
            force = 0;                      /* a probe is one frame */
        }
    }
    __sync_lock_release(&releasing);
}

static void release_routine(KDPC *dpc, void *context, void *a1, void *a2)
{
    (void)dpc; (void)context; (void)a1; (void)a2;
    release_queued = 0;
    release();
}

static void mark(WIN_FLOW *f, u32 force)
{
    ready[f - table.flow] = (WIN_FLOW *)((u32)f | force);
    if (!__sync_lock_test_and_set(&release_queued, 1))
        KeInsertQueueDpc(&release_dpc, 0, 0);
}

void window_init(void)
{
    char value[8];
    u32 n = 0, i;
    u8 *pool;

    if (file_setting("xedrivers.ini", "rx_window_kb", value, sizeof(value))) {
        for (i = 0; value[i] >= '0' && value[i] <= '9'; i++)
            n = n * 10 + (u32)(value[i] - '0');
        net_win_extra_kb = n > 384 ? 384 : n;
    }
    if (!net_win_extra_kb)
        return;
    pool = (u8 *)MmAllocatePhysicalMemory(2, WIN_FRAMES * WIN_FRAME, 4);
    if (!pool) {
        net_win_extra_kb = 0;
        return;
    }
    win_reset(&table, pool);
    usb_dpc_init(&release_dpc, release_routine, 0);
    release_dpc.TargetNumber = USB_PROCESSOR + 1;
}

void window_tx(u8 *frame, u32 length, WIN_PATCH *p)
{
    u32 irql = KfAcquireSpinLock(&lock);
    WIN_FLOW *f = win_out(&table, frame, length, net_win_extra_kb << 10, ticks, p);

    if (p->n)
        net_win_stats.patched++;
    KfReleaseSpinLock(&lock, irql);
    if (f)
        mark(f, 0);
}

u32 window_rx(const u8 *frame, u32 length)
{
    u32 irql, kept;

    if (release_queued)
        release();                          /* what waits goes up before what comes now */
    irql = KfAcquireSpinLock(&lock);
    kept = win_in(&table, frame, length, net_win_extra_kb << 10, ticks);
    net_win_stats.held_now = table.held_now;
    net_win_stats.held_most = table.held_most;
    net_win_stats.opened = table.opened;
    net_win_stats.not_taken = table.not_taken;
    net_win_stats.dropped = table.dropped;
    KfReleaseSpinLock(&lock, irql);
    if (kept == 1)
        net_win_stats.held++;
    else if (kept == 2)
        net_win_stats.no_room++;
    return kept == 1;
}

void window_tick(void)
{
    u32 irql;
    WIN_FLOW *f;

    ticks++;
    irql = KfAcquireSpinLock(&lock);
    f = win_stale(&table, ticks);
    KfReleaseSpinLock(&lock, irql);
    if (f)
        mark(f, 1);
}

#endif /* __powerpc__ */
