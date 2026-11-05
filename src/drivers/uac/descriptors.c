/* USB Audio Class descriptors; see descriptors.h. */

#include "descriptors.h"

#define DT_INTERFACE        4u
#define DT_ENDPOINT         5u
#define DT_CS_INTERFACE     0x24u

#define CLASS_AUDIO         1u
#define SUB_CONTROL         1u
#define SUB_STREAMING       2u

#define AC_HEADER           1u
#define AC_CLOCK_SOURCE     0x0Au       /* UAC2 */
#define AS_GENERAL          1u
#define AS_FORMAT_TYPE      2u
#define FORMAT_TYPE_I       1u
#define UAC1_FORMAT_PCM     1u
#define UAC2_FORMAT_PCM     1u          /* bit 0 of bmFormats */

#define EP_ISOCH            1u
#define EP_USAGE_FEEDBACK   1u
#define EP_IN               0x80u

static u32 le16(const u8 *p)
{
    return p[0] | (u32)p[1] << 8;
}

static u32 le24(const u8 *p)
{
    return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16;
}

static u32 le32(const u8 *p)
{
    return le24(p) | (u32)p[3] << 24;
}

/* What one alternate setting has said so far. */
typedef struct {
    u32 pcm;
    UAC_STREAM s;
    u32 synch_address;      /* UAC1: bSynchAddress of the data endpoint */
} ALT;

/* A UAC1 format type I descriptor: channels, sample size, the rates. */
static void format_uac1(ALT *a, const u8 *d, u32 n)
{
    u32 kinds, i;

    if (n < 8 || d[3] != FORMAT_TYPE_I)
        return;
    a->s.channels = d[4];
    a->s.bytes = d[5];
    a->s.bits = d[6];
    kinds = d[7];
    if (!kinds) {                           /* a continuous range: low, high */
        if (n >= 14 && le24(d + 8) <= UAC_RATE && le24(d + 11) >= UAC_RATE)
            a->s.rate_listed = 1;
        return;
    }
    for (i = 0; i < kinds && 8 + (i + 1) * 3 <= n; i++)
        if (le24(d + 8 + i * 3) == UAC_RATE)
            a->s.rate_listed = 1;
}

/* Is this setting better than the one already kept? The fewest bytes a sample that
 * still make stereo for playing (nothing is gained from 24 bits of our 16), and two
 * channels before one for the microphone only when nothing else differs. */
static u32 better(const UAC_STREAM *a, const UAC_STREAM *kept)
{
    if (!kept->found)
        return 1;
    if (a->rate_listed != kept->rate_listed)
        return a->rate_listed > kept->rate_listed;
    return a->bytes < kept->bytes;
}

/* The setting just read, kept if it can be used and beats what there is. */
static void finish_alt(ALT *a, UAC_INFO *out)
{
    UAC_STREAM *to;

    if (!a->pcm || !a->s.endpoint || !a->s.channels)
        return;
    if (a->s.bytes != 2 && a->s.bytes != 3 && a->s.bytes != 4)
        return;
    if (out->version == 1 && !a->s.rate_listed)
        return;
    if (a->s.endpoint & EP_IN) {
        if (a->s.channels > 2)
            return;
        to = &out->mic;
    } else {
        if (a->s.channels != 2)
            return;
        to = &out->play;
    }
    if (a->s.max_packet < uac_packet_bytes(&a->s) && a->s.sync == UAC_SYNC_ASYNC)
        return;
    if (a->s.max_packet < (UAC_RATE / 1000u) * a->s.channels * a->s.bytes)
        return;
    if (better(&a->s, to)) {
        const u32 *from = (const u32 *)&a->s;
        u32 *into = (u32 *)to, i;

        for (i = 0; i < sizeof(*to) / sizeof(u32); i++)     /* no memcpy in the plugin */
            into[i] = from[i];
        to->found = 1;
    }
}

static void endpoint(ALT *a, const u8 *d, u32 n)
{
    u32 address, attributes, packet;

    if (n < 7 || (d[3] & 3u) != EP_ISOCH)
        return;
    address = d[2];
    attributes = d[3];
    packet = le16(d + 4) & 0x7FFu;          /* no high-bandwidth packets */
    if (((attributes >> 4) & 3u) == EP_USAGE_FEEDBACK || (a->s.endpoint && (address & EP_IN) &&
        !(a->s.endpoint & EP_IN) && address == a->synch_address)) {
        a->s.feedback = address;
        a->s.feedback_max_packet = packet;
        a->s.feedback_interval = d[6];
        return;
    }
    if (a->s.endpoint)
        return;                             /* one data endpoint a setting */
    a->s.endpoint = address;
    a->s.max_packet = packet;
    a->s.interval = d[6];
    a->s.sync = (attributes >> 2) & 3u;
    if (n >= 9)
        a->synch_address = d[8];            /* UAC1's longer endpoint descriptor */
}

u32 uac_packet_bytes(const UAC_STREAM *s)
{
    return (UAC_RATE / 1000u + 1u) * s->channels * s->bytes;
}

u32 uac_parse(const u8 *config, u32 length, UAC_INFO *out)
{
    u32 at = 0, i, in_streaming = 0, in_control = 0;
    ALT a;

    for (i = 0; i < sizeof(*out); i++)
        ((u8 *)out)[i] = 0;
    for (i = 0; i < sizeof(a); i++)
        ((u8 *)&a)[i] = 0;

    while (at + 2 <= length) {
        const u8 *d = config + at;
        u32 n = d[0];

        if (n < 2 || at + n > length)
            break;
        if (d[1] == DT_INTERFACE && n >= 9) {
            if (in_streaming)
                finish_alt(&a, out);
            for (i = 0; i < sizeof(a); i++)
                ((u8 *)&a)[i] = 0;
            in_control = d[5] == CLASS_AUDIO && d[6] == SUB_CONTROL;
            in_streaming = d[5] == CLASS_AUDIO && d[6] == SUB_STREAMING;
            if (in_control && !out->version)
                out->control_interface = d[2];
            if (in_streaming) {
                a.s.interface = d[2];
                a.s.alternate = d[3];
            }
        } else if (d[1] == DT_CS_INTERFACE && in_control && n >= 3) {
            if (d[2] == AC_HEADER && n >= 5 && !out->version)
                out->version = le16(d + 3) >= 0x0200 ? 2 : 1;
            else if (d[2] == AC_CLOCK_SOURCE && n >= 4 && !out->clock)
                out->clock = d[3];
        } else if (d[1] == DT_CS_INTERFACE && in_streaming && n >= 3) {
            if (out->version == 2) {
                if (d[2] == AS_GENERAL && n >= 16 && d[5] == FORMAT_TYPE_I) {
                    a.s.terminal = d[3];
                    a.pcm = (le32(d + 6) & UAC2_FORMAT_PCM) != 0;
                    a.s.channels = d[10];
                } else if (d[2] == AS_FORMAT_TYPE && n >= 6 && d[3] == FORMAT_TYPE_I) {
                    a.s.bytes = d[4];
                    a.s.bits = d[5];
                }
            } else {
                if (d[2] == AS_GENERAL && n >= 7) {
                    a.s.terminal = d[3];
                    a.pcm = le16(d + 5) == UAC1_FORMAT_PCM;
                } else if (d[2] == AS_FORMAT_TYPE) {
                    format_uac1(&a, d, n);
                }
            }
        } else if (d[1] == DT_ENDPOINT && in_streaming) {
            endpoint(&a, d, n);
        }
        at += n;
    }
    if (in_streaming)
        finish_alt(&a, out);
    return out->version && (out->play.found || out->mic.found);
}
