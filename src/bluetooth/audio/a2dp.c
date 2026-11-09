/* See a2dp.h. AVDTP's signalling (Vol: AVDTP 1.3, section 8) both ways and the media
 * channel's RTP packets of SBC frames.
 *
 * We ask (a2dp_connect, after pairing or at a link we made): DISCOVER, GET_ALL_CAPABILITIES
 * of the first free audio sink, SET_CONFIGURATION with the best SBC it takes, OPEN, the
 * media channel, START. The headset asks: it is answered as one source endpoint (SEID 1)
 * that takes any SBC configuration. Either way, START begins the stream and SUSPEND / CLOSE /
 * ABORT stop it.
 *
 * The stream is paced by the time base: every worker tick sends what the elapsed time owes,
 * whole SBC frames, as many to a packet as the media channel's MTU takes.
 */

#include "a2dp.h"
#include "file.h"
#if XE_AUDIO
#include "audio.h"
#endif

#define PSM_AVDTP       0x0019u
#define OUR_SEID        1u

#define SIG_DISCOVER        0x01u
#define SIG_GET_CAPS        0x02u
#define SIG_SET_CONFIG      0x03u
#define SIG_GET_CONFIG      0x04u
#define SIG_RECONFIGURE     0x05u
#define SIG_OPEN            0x06u
#define SIG_START           0x07u
#define SIG_CLOSE           0x08u
#define SIG_SUSPEND         0x09u
#define SIG_ABORT           0x0Au
#define SIG_SECURITY        0x0Bu
#define SIG_GET_ALL_CAPS    0x0Cu
#define SIG_DELAY_REPORT    0x0Du

#define MSG_COMMAND         0u
#define MSG_GENERAL_REJECT  1u
#define MSG_ACCEPT          2u
#define MSG_REJECT          3u

#define CAT_MEDIA_TRANSPORT 0x01u
#define CAT_MEDIA_CODEC     0x07u
#define CAT_DELAY_REPORTING 0x08u

#define TIMEBASE_HZ         50000000u

volatile A2DP_STATS a2dp_stats;

static volatile BT_CONN *link;
static L2CAP_CHANNEL *signalling, *media;
static u32 label;
static SBC_CAPS config;                 /* what is configured */
static u32 config_bitpool;
static u32 switch_ticks;
static u32 sink_delay;                  /* the sink takes delay reporting */
static u32 no_reconfigure;              /* the sink refused RECONFIGURE: steps by CLOSE */
static void set_config_step(u32 t);
static SBC_CAPS pending_config;         /* asked for by RECONFIGURE */
static u32 pending_bitpool, pending_step;
static void reconfigure_from(const u8 *p, u32 n);
static void start_now(void);
static void read_config(const SBC_CAPS *c, u32 *rate, u32 *mode, u32 *blocks, u32 *alloc);
static void reconfigure_to(u32 t);
static SBC sbc;
static u32 sequence, timestamp, last_tb, rate_hz;
static u32 per_tick;                    /* samples per time-base tick, x 2^32 */
static u64 owed_fp;                     /* samples owed since the start, x 2^32 */
static u64 samples_sent;
#if XE_AUDIO
static u32 played_at_start;
#endif
static u32 pending;                     /* bytes of `packet` still to go */
static u32 quiet_ticks, down_gap;
static u8 packet[1024];
static s16 pcm[128 * 2];
#if XE_AUDIO
static s32 frames32[128 * 2];
#endif

static const u32 rates_hz[4] = { 16000, 32000, 44100, 48000 };

/* --- the choice, pure ----------------------------------------------------------------- */

/* Bits a second of SBC with 8 subbands. */
static u32 bitrate(u32 rate, u32 mode, u32 blocks, u32 bitpool)
{
    SBC s;

    sbc_init(&s, rate, blocks, mode, SBC_LOUDNESS, bitpool);
    return sbc_frame_length(&s) * 8u * rates_hz[rate] / (blocks * 8u);   /* < 2^28 */
}

static u32 limit_kbps;

void a2dp_link_limit(u32 kbps)
{
    limit_kbps = kbps;
}

u32 a2dp_choose(const SBC_CAPS *sink, u32 min_bitpool, u32 max_bitpool, SBC_CAPS *out,
                u32 *bitpool)
{
    /* element bits: b0 = rate 16/32/44.1/48 at 7..4, mode mono/dual/stereo/joint at 3..0;
     * b1 = blocks 4/8/12/16 at 7..4, subbands 4/8 at 3..2, allocation SNR/loudness at 1..0 */
    static const u32 mode_bit[4] = { 0x08, 0x04, 0x02, 0x01 };     /* by SBC_* mode */
    u32 rate, mode, best_mode = 4, best_rate = 4, best = 0, blocks = 16, pool, b;

    if (!(sink->b[1] & 0x04) || min_bitpool > max_bitpool)
        return 0;                       /* 8 subbands: every sink takes them */
    for (rate = 3; rate < 4 && best_rate == 4; rate--) {        /* 48k, 44.1k, ... */
        if (sink->b[0] & (0x10u << (3 - rate)))
            best_rate = rate;
        if (rate == 0)
            break;
    }
    if (best_rate == 4)
        return 0;
    for (b = 0; b < 4; b++)                                     /* most blocks it takes */
        if (sink->b[1] & (0x10u << b)) {
            blocks = 16 - 4 * b;
            break;
        }
    pool = max_bitpool;
    for (mode = 0; mode < 4; mode++) {
        u32 r, p = pool;

        if (!(sink->b[0] & mode_bit[mode]))
            continue;
        if ((mode == SBC_MONO || mode == SBC_DUAL) && p > 128)
            p = 128;                    /* 16 x subbands */
        while (limit_kbps && p > min_bitpool && bitrate(best_rate, mode, blocks, p) >
               limit_kbps * 1000)
            p--;                        /* what the link carries */
        r = bitrate(best_rate, mode, blocks, p);
        /* mono is a last resort; of the rest the most bits, joint over stereo when equal */
        if (mode == SBC_MONO && best)
            continue;
        if (r > best || (r == best && mode == SBC_JOINT) || best_mode == SBC_MONO) {
            best = r;
            best_mode = mode;
            *bitpool = p;
        }
    }
    if (best_mode == 4)
        return 0;
    out->b[0] = (u8)((0x10u << (3 - best_rate)) | mode_bit[best_mode]);
    out->b[1] = (u8)((0x10u << ((16 - blocks) / 4)) | 0x04 |
                     ((sink->b[1] & 0x01) ? 0x01 : 0x02));      /* loudness if it takes it */
    out->b[2] = (u8)min_bitpool;
    out->b[3] = (u8)*bitpool;
    return 1;
}

/* --- the quality ladder ----------------------------------------------------------------
 * bt_quality in xedrivers.ini:
 *   auto (default)  start at the best the sink takes; when the bitpool has been held under 60%
 *                   of the step's for 10 s, go one step down (a pause of ~0.2 s: SUSPEND,
 *                   RECONFIGURE, START); after 60 s clean try one up, waiting twice as long
 *                   (to 10 min) each time an up does not hold
 *   max             the best the sink takes, never another step (only the bitpool follows)
 *   xq / hq         dual 38 / joint 53 (or the nearest the sink takes), never another step
 * Steps, best first, only those the sink takes and each under the one before: its best, dual
 * 38 (SBC-XQ), joint 53 (SBC HQ), joint 35. */

#define QUALITY_AUTO 0u
#define QUALITY_MAX  1u
#define QUALITY_XQ   2u
#define QUALITY_HQ   3u

typedef struct {
    SBC_CAPS c;
    u32 bitpool;
    u32 kbps;
} STEP;

static STEP steps[4];
static u32 step_count, step_now, quality;

/* The sink's element with the mode `mode` and bitpool `bp` (capped to its range); 0 if the mode
 * is not taken. */
static u32 make_step(const SBC_CAPS *sink, u32 min_bp, u32 max_bp, u32 mode, u32 bp, STEP *out)
{
    static const u32 mode_bit[4] = { 0x08, 0x04, 0x02, 0x01 };
    SBC_CAPS one = *sink;
    u32 got;

    if (!(sink->b[0] & mode_bit[mode]))
        return 0;
    one.b[0] = (u8)((sink->b[0] & 0xF0u) | mode_bit[mode]);     /* that mode only */
    if (bp > max_bp)
        bp = max_bp;
    if (!a2dp_choose(&one, min_bp, bp, &out->c, &got))
        return 0;
    out->bitpool = got;
    {
        u32 rate, m, blocks, alloc;

        read_config(&out->c, &rate, &m, &blocks, &alloc);
        out->kbps = bitrate(rate, m, blocks, got) / 1000;
    }
    return 1;
}

u32 a2dp_steps(const SBC_CAPS *sink, u32 min_bp, u32 max_bp)
{
    STEP s;
    u32 bp;

    step_count = 0;
    if (!a2dp_choose(sink, min_bp, max_bp, &steps[0].c, &bp))
        return 0;
    steps[0].bitpool = bp;
    {
        u32 rate, m, blocks, alloc;

        read_config(&steps[0].c, &rate, &m, &blocks, &alloc);
        steps[0].kbps = bitrate(rate, m, blocks, bp) / 1000;
    }
    step_count = 1;
    if (make_step(sink, min_bp, max_bp, SBC_DUAL, 38, &s) && s.kbps < steps[step_count - 1].kbps)
        steps[step_count++] = s;
    if ((make_step(sink, min_bp, max_bp, SBC_JOINT, 53, &s) ||
         make_step(sink, min_bp, max_bp, SBC_STEREO, 53, &s)) && s.kbps < steps[step_count - 1].kbps)
        steps[step_count++] = s;
    if ((make_step(sink, min_bp, max_bp, SBC_JOINT, 35, &s) ||
         make_step(sink, min_bp, max_bp, SBC_STEREO, 35, &s)) && s.kbps < steps[step_count - 1].kbps)
        steps[step_count++] = s;
    return step_count;
}

/* The step the setting starts at. */
static u32 first_step(void)
{
    u32 i, want = quality == QUALITY_XQ ? 492 : quality == QUALITY_HQ ? 345 : 0xFFFFFFFFu;

    if (quality == QUALITY_AUTO || quality == QUALITY_MAX)
        return 0;
    for (i = 0; i < step_count; i++)
        if (steps[i].kbps <= want + 10)
            return i;
    return step_count - 1;
}

/* Read once (a step change must not touch the file system mid-stream). */
static void read_quality(void)
{
    static u32 read;
    char v[8];

    if (read)
        return;
    read = 1;
    quality = QUALITY_AUTO;
    if (file_setting("xedrivers.ini", "bt_quality", v, sizeof(v))) {
        if (v[0] == 'm')
            quality = QUALITY_MAX;
        else if (v[0] == 'x')
            quality = QUALITY_XQ;
        else if (v[0] == 'h')
            quality = QUALITY_HQ;
    }
    a2dp_stats.quality = quality;
}

/* What the element says, for the encoder. */
static void read_config(const SBC_CAPS *c, u32 *rate, u32 *mode, u32 *blocks, u32 *alloc)
{
    u32 i;

    *rate = SBC_RATE_48000;
    for (i = 0; i < 4; i++)
        if (c->b[0] & (0x80u >> i))
            *rate = i;
    *mode = SBC_JOINT;
    for (i = 0; i < 4; i++)
        if (c->b[0] & (0x08u >> i))
            *mode = i;
    *blocks = 16;
    for (i = 0; i < 4; i++)
        if (c->b[1] & (0x80u >> i))
            *blocks = 4 + 4 * i;
    *alloc = (c->b[1] & 0x01) ? SBC_LOUDNESS : SBC_SNR;
}

/* --- signalling ----------------------------------------------------------------------- */

static void send_signal(u32 lbl, u32 type, u32 signal, const u8 *params, u32 n)
{
    u8 m[64];
    u32 i;

    if (!signalling || n + 2 > sizeof(m))
        return;
    m[0] = (u8)(lbl << 4 | type);
    m[1] = (u8)signal;
    for (i = 0; i < n; i++)
        m[2 + i] = params[i];
    l2cap_send(signalling, m, n + 2);
}

static void command(u32 signal, const u8 *params, u32 n)
{
    label = (label + 1) & 15u;
    send_signal(label, MSG_COMMAND, signal, params, n);
}

static void stop_stream(void)
{
#if XE_AUDIO
    if (a2dp_stats.state == A2DP_STREAMING)
        audio_close(AUDIO_READER_BLUETOOTH);
#endif
}

static void start_stream(void)
{
    u32 rate, mode, blocks, alloc, room;

    read_config(&config, &rate, &mode, &blocks, &alloc);
    sbc_init(&sbc, rate, blocks, mode, alloc, config_bitpool);
    a2dp_stats.chose_rate = rates_hz[rate];
    a2dp_stats.chose_mode = mode;
    a2dp_stats.chose_blocks = blocks;
    a2dp_stats.chose_bitpool = config_bitpool;
    a2dp_stats.chose_allocation = alloc;
    a2dp_stats.frame_length = sbc_frame_length(&sbc);
    room = media ? media->remote_mtu : 672;
    if (room > sizeof(packet))
        room = sizeof(packet);
    a2dp_stats.media_mtu = room;
    a2dp_stats.frames_per_packet = (room - 13) / a2dp_stats.frame_length;
    if (a2dp_stats.frames_per_packet > 15)
        a2dp_stats.frames_per_packet = 15;
    if (!a2dp_stats.frames_per_packet)
        a2dp_stats.frames_per_packet = 1;
    rate_hz = rates_hz[rate];
    samples_sent = 0;
    pending = 0;
    quiet_ticks = down_gap = 0;
    a2dp_stats.bitpool_lowest = 0;
    owed_fp = 0;
    per_tick = (u32)((double)rate_hz * 4294967296.0 / (double)TIMEBASE_HZ);
#if defined(__powerpc__)
    __asm__ volatile("mftb %0" : "=r"(last_tb));
#endif
#if XE_AUDIO
    audio_open(AUDIO_READER_BLUETOOTH, AUDIO_STEREO);
#endif
    a2dp_stats.state = A2DP_STREAMING;
    a2dp_stats.starts++;
}

/* The sink's endpoints: the first audio sink not in use. */
/* The sink's free audio endpoints, from DISCOVER: each asked in turn until one takes SBC (a
 * PC's PipeWire lists one per codec, AAC or LDAC first). */
static u8 candidates[16];
static u32 candidate_count, candidate_at;

static void ask_next_candidate(void)
{
    u8 q;

    if (candidate_at >= candidate_count) {
        a2dp_stats.state = A2DP_IDLE;   /* none takes SBC */
        return;
    }
    a2dp_stats.remote_seid = candidates[candidate_at++];
    a2dp_stats.state = A2DP_CAPABILITIES;
    q = (u8)(a2dp_stats.remote_seid << 2);
    command(SIG_GET_ALL_CAPS, &q, 1);
}

static void discovered(const u8 *p, u32 n)
{
    u32 i;

    candidate_count = candidate_at = 0;
    for (i = 0; i + 2 <= n && candidate_count < sizeof(candidates); i += 2) {
        u32 seid = p[i] >> 2, in_use = (p[i] >> 1) & 1u, media_type = p[i + 1] >> 4,
            sink = (p[i + 1] >> 3) & 1u;

        if (!in_use && media_type == 0 && sink)
            candidates[candidate_count++] = (u8)seid;
    }
    ask_next_candidate();
}

/* Its capabilities: the SBC element among them, and our configuration from it. */
static void capabilities(const u8 *p, u32 n)
{
    u32 at = 0, delay = 0;
    u8 set[16];

    while (at + 2 <= n) {
        u32 cat = p[at], len = p[at + 1];

        if (at + 2 + len > n)
            break;
        if (cat == CAT_DELAY_REPORTING)
            delay = 1;
        if (cat == CAT_MEDIA_CODEC && len >= 6 && (p[at + 2] >> 4) == 0 && p[at + 3] == 0) {
            SBC_CAPS sink, chosen;
            u32 bp;

            sink.b[0] = p[at + 4];
            sink.b[1] = p[at + 5];
            a2dp_stats.caps_seen[0] = (u32)(p[at + 4] << 8 | p[at + 5]);
            a2dp_stats.caps_seen[1] = (u32)(p[at + 6] << 8 | p[at + 7]);
            read_quality();
            if (!a2dp_steps(&sink, p[at + 6] < 2 ? 2 : p[at + 6], p[at + 7]))
                break;
            (void)chosen;
            (void)bp;
            (void)set;
            sink_delay = delay;
            no_reconfigure = 0;
            set_config_step(first_step());
            return;
        }
        at += 2 + len;
    }
    ask_next_candidate();               /* not SBC here: the next endpoint */
}

static void media_open(L2CAP_CHANNEL *ch);
static void media_closed(L2CAP_CHANNEL *ch, u32 was_open);
static const L2CAP_USER media_user = { media_open, 0, media_closed };

static void accepted(u32 signal, const u8 *p, u32 n)
{
    u8 q = (u8)(a2dp_stats.remote_seid << 2);

    switch (signal) {
    case SIG_DISCOVER:
        if (a2dp_stats.state == A2DP_DISCOVERING)
            discovered(p, n);
        break;
    case SIG_GET_ALL_CAPS:
    case SIG_GET_CAPS:
        if (a2dp_stats.state == A2DP_CAPABILITIES)
            capabilities(p, n);
        else if (a2dp_stats.state == A2DP_RECONF_CAPS)
            reconfigure_from(p, n);
        break;
    case SIG_RECONFIGURE:
        if (a2dp_stats.state == A2DP_RECONFIGURING) {
            config = pending_config;
            config_bitpool = pending_bitpool;
            step_now = pending_step;
            a2dp_stats.reconfigured++;
            a2dp_stats.step = step_now;
            start_now();
        }
        break;
    case SIG_SET_CONFIG:
        a2dp_stats.state = A2DP_OPENING;
        command(SIG_OPEN, &q, 1);
        break;
    case SIG_OPEN:
        /* the media channel: the second L2CAP channel on PSM 0x19, ours to open */
        media = l2cap_open(link, PSM_AVDTP, &media_user, 0);
        if (!media)
            a2dp_stats.state = A2DP_IDLE;
        break;
    case SIG_START:
        start_stream();
        switch_ticks = 0;
        break;
    case SIG_SUSPEND:
        if (a2dp_stats.state == A2DP_SWITCHING) {
            reconfigure_to(pending_step);   /* suspended: now the new step */
            break;
        }
        stop_stream();
        a2dp_stats.state = A2DP_OPEN;
        break;
    default:
        break;
    }
}

static void rejected(u32 signal, const u8 *p, u32 n)
{
    u8 q = (u8)(a2dp_stats.remote_seid << 2);

    a2dp_stats.rejects++;
    a2dp_stats.last_reject = signal << 8 | (n ? p[n - 1] : 0);
    if (signal == SIG_RECONFIGURE && switch_ticks && a2dp_stats.state == A2DP_RECONFIGURING) {
        /* no RECONFIGURE (BlueZ: NOT SUPPORTED COMMAND): this sink keeps its step, the bitpool
         * alone follows the radio. Closing the stream and configuring it anew was tried:
         * BlueZ dropped the whole link once its only stream was closed. */
        no_reconfigure = 1;
        a2dp_stats.step_refused++;
        start_now();
        return;
    }
    if (a2dp_stats.state == A2DP_RECONF_CAPS || a2dp_stats.state == A2DP_RECONFIGURING ||
        a2dp_stats.state == A2DP_SWITCHING) {
        a2dp_stats.step_refused++;
        start_now();                    /* as it was configured, then */
        return;
    }
    if (signal == SIG_GET_ALL_CAPS && a2dp_stats.state == A2DP_CAPABILITIES) {
        command(SIG_GET_CAPS, &q, 1);   /* an AVDTP 1.2 sink */
        return;
    }
    if (signal == SIG_START)
        return;                         /* it may start by itself */
    a2dp_stats.state = A2DP_IDLE;
}

/* What the headset asks. */
static void asked(u32 lbl, u32 signal, const u8 *p, u32 n)
{
    static const u8 caps[] = { CAT_MEDIA_TRANSPORT, 0,
                               CAT_MEDIA_CODEC, 6, 0x00, 0x00, 0xFF, 0xFF, 2, 76,
                               CAT_DELAY_REPORTING, 0 };
    u8 r[16];
    u32 at;

    switch (signal) {
    case SIG_DISCOVER:
        r[0] = (u8)(OUR_SEID << 2 | (a2dp_stats.state >= A2DP_OPENING ? 2 : 0));
        r[1] = 0x00;                    /* audio, source */
        send_signal(lbl, MSG_ACCEPT, signal, r, 2);
        break;
    case SIG_GET_CAPS:
        send_signal(lbl, MSG_ACCEPT, signal, caps, 10);
        break;
    case SIG_GET_ALL_CAPS:
        send_signal(lbl, MSG_ACCEPT, signal, caps, sizeof(caps));
        break;
    case SIG_SET_CONFIG:
        /* ACP SEID, INT SEID, then categories: take the codec's element */
        if (n >= 2)
            a2dp_stats.remote_seid = p[1] >> 2;
        for (at = 2; at + 2 <= n; at += 2 + p[at + 1]) {
            if (p[at] == CAT_MEDIA_CODEC && p[at + 1] >= 6 && at + 8 <= n && p[at + 3] == 0) {
                config.b[0] = p[at + 4];
                config.b[1] = p[at + 5];
                config.b[2] = p[at + 6];
                config.b[3] = p[at + 7];
                config_bitpool = p[at + 7];
            }
        }
        a2dp_stats.state = A2DP_OPENING;
        send_signal(lbl, MSG_ACCEPT, signal, 0, 0);
        break;
    case SIG_GET_CONFIG:
        r[0] = CAT_MEDIA_TRANSPORT; r[1] = 0;
        r[2] = CAT_MEDIA_CODEC; r[3] = 6; r[4] = 0; r[5] = 0;
        r[6] = config.b[0]; r[7] = config.b[1]; r[8] = config.b[2]; r[9] = config.b[3];
        send_signal(lbl, MSG_ACCEPT, signal, r, 10);
        break;
    case SIG_OPEN:
        send_signal(lbl, MSG_ACCEPT, signal, 0, 0);
        a2dp_stats.state = A2DP_OPENING;        /* it opens the media channel */
        break;
    case SIG_START:
        send_signal(lbl, MSG_ACCEPT, signal, 0, 0);
        if (media && media->state == L2CAP_OPEN)
            start_stream();
        break;
    case SIG_SUSPEND:
        send_signal(lbl, MSG_ACCEPT, signal, 0, 0);
        stop_stream();
        a2dp_stats.state = A2DP_OPEN;
        break;
    case SIG_CLOSE:
    case SIG_ABORT:
        send_signal(lbl, MSG_ACCEPT, signal, 0, 0);
        stop_stream();
        a2dp_stats.state = A2DP_IDLE;
        break;
    case SIG_DELAY_REPORT:
    case SIG_SECURITY:
        send_signal(lbl, MSG_ACCEPT, signal, 0, 0);
        break;
    default:
        send_signal(lbl, MSG_GENERAL_REJECT, signal, 0, 0);
        break;
    }
}

static void signalling_data(L2CAP_CHANNEL *ch, const u8 *d, u32 n)
{
    u32 lbl, type, signal;

    (void)ch;
    if (n < 2 || ((d[0] >> 2) & 3u) != 0)      /* single packets only: ours all fit */
        return;
    lbl = d[0] >> 4;
    type = d[0] & 3u;
    signal = d[1] & 0x3Fu;
    a2dp_stats.last_signal = signal << 8 | type << 4 | lbl;
    if (type == MSG_COMMAND)
        asked(lbl, signal, d + 2, n - 2);
    else if (type == MSG_ACCEPT)
        accepted(signal, d + 2, n - 2);
    else
        rejected(signal, d + 2, n - 2);
}

static void signalling_open(L2CAP_CHANNEL *ch)
{
    if (!ch->ctx)                       /* the headset opened it: it leads */
        return;
    if (ch != signalling) {             /* the headset's came first: ours is not needed */
        l2cap_close(ch);
        return;
    }
    a2dp_stats.state = A2DP_DISCOVERING;
    command(SIG_DISCOVER, 0, 0);
}

static void signalling_closed(L2CAP_CHANNEL *ch, u32 was_open)
{
    (void)was_open;
    if (ch != signalling)
        return;
    stop_stream();
    signalling = 0;
    if (media)
        l2cap_close(media);
    a2dp_stats.state = A2DP_IDLE;
}

/* The media channel is there: the source starts the stream, whoever configured it -- a sink
 * that set it up waits for us (BlueZ reconnecting by itself did). */
static void media_open(L2CAP_CHANNEL *ch)
{
    u8 q = (u8)(a2dp_stats.remote_seid << 2);

    media = ch;
    if (a2dp_stats.state == A2DP_OPENING && a2dp_stats.remote_seid) {
        if (!a2dp_stats.role_initiator) {
            /* the sink chose (027570: joint 38, though it takes dual 38): see what it takes,
             * and reconfigure to the best before the start */
            a2dp_stats.state = A2DP_RECONF_CAPS;
            command(SIG_GET_ALL_CAPS, &q, 1);
            return;
        }
        a2dp_stats.state = A2DP_STARTING;
        command(SIG_START, &q, 1);
    } else {
        a2dp_stats.state = A2DP_OPEN;
    }
}

/* RECONFIGURE to `t` (the stream suspended or not yet started). */
static void reconfigure_to(u32 t)
{
    u8 r[9];

    pending_config = steps[t].c;
    pending_bitpool = steps[t].bitpool;
    pending_step = t;
    r[0] = (u8)(a2dp_stats.remote_seid << 2);
    r[1] = CAT_MEDIA_CODEC;
    r[2] = 6;
    r[3] = 0x00;
    r[4] = 0x00;
    r[5] = steps[t].c.b[0];
    r[6] = steps[t].c.b[1];
    r[7] = steps[t].c.b[2];
    r[8] = steps[t].c.b[3];
    a2dp_stats.state = A2DP_RECONFIGURING;
    command(SIG_RECONFIGURE, r, 9);
}

static void set_config_step(u32 t)
{
    u8 set[16];
    u32 n = 12;

    step_now = t;
    a2dp_stats.step = t;
    config = steps[t].c;
    config_bitpool = steps[t].bitpool;
    set[0] = (u8)(a2dp_stats.remote_seid << 2);
    set[1] = (u8)(OUR_SEID << 2);
    set[2] = CAT_MEDIA_TRANSPORT;
    set[3] = 0;
    set[4] = CAT_MEDIA_CODEC;
    set[5] = 6;
    set[6] = 0x00;                      /* audio */
    set[7] = 0x00;                      /* SBC */
    set[8] = config.b[0];
    set[9] = config.b[1];
    set[10] = config.b[2];
    set[11] = config.b[3];
    if (sink_delay) {
        set[12] = CAT_DELAY_REPORTING;
        set[13] = 0;
        n = 14;
    }
    a2dp_stats.state = A2DP_CONFIGURING;
    command(SIG_SET_CONFIG, set, n);
}

/* auto: one step down or up while streaming: SUSPEND, then (accepted) RECONFIGURE, START. */
static void switch_step(u32 t)
{
    u8 q = (u8)(a2dp_stats.remote_seid << 2);

    if (t >= step_count || t == step_now || a2dp_stats.state != A2DP_STREAMING)
        return;
    if (no_reconfigure)
        return;                         /* this sink: the bitpool only (see rejected()) */
    pending_step = t;
    stop_stream();
    switch_ticks = 1;
    a2dp_stats.state = A2DP_SWITCHING;
    command(SIG_SUSPEND, &q, 1);
}

static void start_now(void)
{
    u8 q = (u8)(a2dp_stats.remote_seid << 2);

    a2dp_stats.state = A2DP_STARTING;
    command(SIG_START, &q, 1);
}

/* The sink's capabilities, as the sink had configured the stream: a better configuration
 * there is replaced by RECONFIGURE (the codec only), else the stream starts as it is. */
static void reconfigure_from(const u8 *p, u32 n)
{
    u32 at = 0;

    while (at + 2 <= n) {
        u32 cat = p[at], len = p[at + 1];

        if (at + 2 + len > n)
            break;
        if (cat == CAT_MEDIA_CODEC && len >= 6 && (p[at + 2] >> 4) == 0 && p[at + 3] == 0) {
            SBC_CAPS sink, chosen;
            u32 bp, now, better;
            u8 r[9];

            sink.b[0] = p[at + 4];
            sink.b[1] = p[at + 5];
            a2dp_stats.caps_seen[0] = (u32)(p[at + 4] << 8 | p[at + 5]);
            a2dp_stats.caps_seen[1] = (u32)(p[at + 6] << 8 | p[at + 7]);
            read_quality();
            if (!a2dp_steps(&sink, p[at + 6] < 2 ? 2 : p[at + 6], p[at + 7]))
                break;
            step_now = first_step();
            chosen = steps[step_now].c;
            bp = steps[step_now].bitpool;
            (void)now;
            (void)better;
            if (chosen.b[0] == config.b[0] && chosen.b[1] == config.b[1] && bp == config_bitpool)
                break;                          /* the sink chose what we would */
            pending_config = chosen;
            pending_bitpool = bp;
            pending_step = step_now;
            r[0] = (u8)(a2dp_stats.remote_seid << 2);
            r[1] = CAT_MEDIA_CODEC;
            r[2] = 6;
            r[3] = 0x00;
            r[4] = 0x00;
            r[5] = chosen.b[0];
            r[6] = chosen.b[1];
            r[7] = chosen.b[2];
            r[8] = chosen.b[3];
            a2dp_stats.state = A2DP_RECONFIGURING;
            command(SIG_RECONFIGURE, r, 9);
            return;
        }
        at += 2 + len;
    }
    start_now();
}

static void media_closed(L2CAP_CHANNEL *ch, u32 was_open)
{
    (void)was_open;
    if (ch != media)
        return;
    stop_stream();
    media = 0;
    if (a2dp_stats.state > A2DP_OPEN)
        a2dp_stats.state = A2DP_OPEN;
}

/* Incoming on PSM 0x19: the first channel of a link is signalling, the next media. */
static u32 lead_wait;                   /* ticks the headset's signalling channel was idle */
static void limit_for(volatile BT_CONN *c);

static void avdtp_open(L2CAP_CHANNEL *ch)
{
    if (!signalling || signalling->state != L2CAP_OPEN) {
        signalling = ch;
        link = ch->conn;
        limit_for(link);
        a2dp_stats.role_initiator = 0;
        a2dp_stats.state = A2DP_SIGNALLING;
        lead_wait = 0;
        return;
    }
    media_open(ch);
}

static void avdtp_data(L2CAP_CHANNEL *ch, const u8 *d, u32 n)
{
    if (ch == signalling)
        signalling_data(ch, d, n);
}

static void avdtp_closed(L2CAP_CHANNEL *ch, u32 was_open)
{
    if (ch == signalling)
        signalling_closed(ch, was_open);
    else if (ch == media)
        media_closed(ch, was_open);
}

static const L2CAP_USER incoming_user = { avdtp_open, avdtp_data, avdtp_closed };
static const L2CAP_USER signalling_user = { signalling_open, signalling_data, signalling_closed };

void a2dp_init(void)
{
    l2cap_listen(PSM_AVDTP, &incoming_user, 0);
}

/* No fixed limit by the link's features: an EDR dongle a metre away (Edimax, 2M+3M) could
 * not carry 804 kbit/s either -- what a link carries is measured while it plays. */
static void limit_for(volatile BT_CONN *c)
{
    (void)c;
    a2dp_link_limit(0);
}

void a2dp_connect(volatile BT_CONN *c)
{
    limit_for(c);
    if (signalling)
        return;                         /* one headset at a time */
    link = c;
    a2dp_stats.role_initiator = 1;
    a2dp_stats.state = A2DP_SIGNALLING;
    signalling = l2cap_open(c, PSM_AVDTP, &signalling_user, (void *)1);
    if (!signalling)
        a2dp_stats.state = A2DP_IDLE;
}

void a2dp_gone(volatile BT_CONN *c)
{
    if (c != link)
        return;
    stop_stream();
    signalling = media = 0;
    link = 0;
    a2dp_stats.state = A2DP_IDLE;
}

/* --- the stream ----------------------------------------------------------------------- */

static void one_packet(void)
{
    u32 k, at = 13, per = sbc_samples(&sbc), i, n = a2dp_stats.frames_per_packet;

    packet[0] = 0x80;                   /* RTP version 2 */
    packet[1] = 0x60;                   /* payload type 96 */
    packet[2] = (u8)(sequence >> 8);
    packet[3] = (u8)sequence;
    packet[4] = (u8)(timestamp >> 24);
    packet[5] = (u8)(timestamp >> 16);
    packet[6] = (u8)(timestamp >> 8);
    packet[7] = (u8)timestamp;
    packet[8] = 0; packet[9] = 0; packet[10] = 0; packet[11] = 1;      /* SSRC */
    packet[12] = (u8)n;                 /* frames in it, no fragmentation */
    for (k = 0; k < n; k++) {
#if XE_AUDIO
        audio_read(AUDIO_READER_BLUETOOTH, frames32, per);
        for (i = 0; i < per * 2; i++)
            pcm[i] = (s16)(frames32[i] >> 16);
#else
        for (i = 0; i < per * 2; i++)
            pcm[i] = 0;
#endif
        if (sbc.mode == SBC_MONO)
            for (i = 0; i < per; i++)
                pcm[i] = (s16)(((s32)pcm[2 * i] + pcm[2 * i + 1]) / 2);
        at += sbc_encode(&sbc, pcm, packet + at);
    }
    /* sent now, or held until the controller has room: a packet given up is a gap the
     * listener hears (6% of them, with a headset at -74 dBm) */
    pending = at;
    if (!l2cap_send(media, packet, at)) {
        pending = 0;
        a2dp_stats.packets++;
        a2dp_stats.frames += n;
    }
    sequence++;
    timestamp += n * per;
    samples_sent += n * per;
}

/* The bitpool follows the radio, as Android's does: down by 4 when a packet had to wait (at
 * most every 0.5 s, not under 18), up by 1 after 2 s without that, back to what was agreed.
 * Every SBC frame says its own bitpool, so the sink follows without asking. */
#define BITPOOL_FLOOR   18u
#define DOWN_GAP_TICKS  25u
#define UP_AFTER_TICKS  100u

static void bitpool_tick(u32 waited)
{
    if (waited) {
        quiet_ticks = 0;
        if (++down_gap >= DOWN_GAP_TICKS && sbc.bitpool > BITPOOL_FLOOR + 4) {
            sbc.bitpool -= 4;
            down_gap = 0;
            a2dp_stats.bitpool_downs++;
        }
    } else if (++quiet_ticks >= UP_AFTER_TICKS && sbc.bitpool < config_bitpool) {
        sbc.bitpool++;
        quiet_ticks = 0;
    }
    a2dp_stats.bitpool_now = sbc.bitpool;
    if (sbc.bitpool < a2dp_stats.bitpool_lowest || !a2dp_stats.bitpool_lowest)
        a2dp_stats.bitpool_lowest = sbc.bitpool;
}

/* auto's steps: down when the bitpool stays under 60% of the step's for 10 s, up after a
 * clean minute (twice as long each time an up does not hold, to 10 min). */
#define DOWN_TICKS      500u
#define UP_TICKS_FIRST  3000u
#define UP_TICKS_MOST   30000u
#define HELD_TICKS      1000u           /* an up that lasts 20 s held */

static u32 low_ticks, clean_ticks, up_wait = UP_TICKS_FIRST, since_up = 0xFFFFFFFFu;

#define SWITCH_TIMEOUT_TICKS 100u        /* 2 s for the sink to answer a step's SUSPEND */

static void ladder_tick(u32 waited)
{
    /* a sink that does not answer the step's SUSPEND or RECONFIGURE: play on as it was */
    if (switch_ticks && a2dp_stats.state != A2DP_STREAMING) {
        if (++switch_ticks >= SWITCH_TIMEOUT_TICKS) {
            switch_ticks = 0;
            a2dp_stats.step_timeouts++;
            start_stream();             /* the old configuration, streaming again */
        }
        return;
    }
    switch_ticks = 0;
    if (quality != QUALITY_AUTO || a2dp_stats.state != A2DP_STREAMING || !step_count)
        return;
    if (a2dp_voice_busy() || no_reconfigure)   /* voice link up (hfp.c), or a sink that keeps
                                                 * its step: no steps */
        return;
    if (since_up != 0xFFFFFFFFu)
        since_up++;
    if (sbc.bitpool * 10 < steps[step_now].bitpool * 6)
        low_ticks++;
    else
        low_ticks = 0;
    if (!waited && sbc.bitpool >= steps[step_now].bitpool)
        clean_ticks++;
    else
        clean_ticks = 0;
    if (low_ticks >= DOWN_TICKS && step_now + 1 < step_count) {
        if (since_up < HELD_TICKS && up_wait < UP_TICKS_MOST)
            up_wait *= 2;               /* that up did not hold */
        low_ticks = clean_ticks = 0;
        since_up = 0xFFFFFFFFu;
        a2dp_stats.steps_down++;
        switch_step(step_now + 1);
    } else if (clean_ticks >= up_wait && step_now > 0) {
        low_ticks = clean_ticks = 0;
        since_up = 0;
        a2dp_stats.steps_up++;
        switch_step(step_now - 1);
    }
}

void a2dp_tick(void)
{
    u32 now = 0, per, waited = 0;
    u64 owed;

    /* the headset opened signalling and then waited for us (027570 did, its channel and ours
     * crossing): after 5 s we lead on its channel */
    if (a2dp_stats.state == A2DP_SIGNALLING && !a2dp_stats.role_initiator && signalling &&
        signalling->state == L2CAP_OPEN && ++lead_wait >= 250) {
        a2dp_stats.role_initiator = 1;
        a2dp_stats.state = A2DP_DISCOVERING;
        a2dp_stats.led_for_sink++;
        command(SIG_DISCOVER, 0, 0);
        return;
    }
    if (a2dp_stats.state != A2DP_STREAMING || !media)
        return;
#if defined(__powerpc__)
    __asm__ volatile("mftb %0" : "=r"(now));
#endif
#if XE_AUDIO
    /* paced by the console's audio clock, not the time base: the time base is ~49.875 MHz,
     * not the 50 assumed, and a stream sent by it ran 0.28% slow -- the headset heard it as
     * the pitch wandering, the capture's reader fell a ring behind and skipped */
    (void)now;
    owed = (u64)(audio_frames_played() - played_at_start);
#else
    owed_fp += (u64)(now - last_tb) * per_tick;
    last_tb = now;
    owed = owed_fp >> 32;
#endif
    per = sbc_samples(&sbc) * a2dp_stats.frames_per_packet;
    if (owed > samples_sent + rate_hz / 5) {        /* 200 ms behind: start over from now */
        a2dp_stats.late += (u32)(owed - samples_sent);
        samples_sent = owed;
    }
    while (media) {
        if (pending) {
            if (l2cap_send(media, packet, pending)) {
                a2dp_stats.send_failures++;     /* no room yet: it waits */
                waited = 1;
                break;
            }
            pending = 0;
            a2dp_stats.packets++;
            a2dp_stats.frames += a2dp_stats.frames_per_packet;
            continue;
        }
        if (samples_sent + per > owed + per / 2)
            break;
        one_packet();
    }
    bitpool_tick(waited);
    ladder_tick(waited);
}
