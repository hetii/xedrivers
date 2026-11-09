/* USB Audio Class: any UAC1 or UAC2 device with a stereo output at 48 kHz plays the
 * console's sound. Chosen from the device's own descriptors (descriptors.c), not a list
 * of products. Full speed on the companion (OHCI) controllers and high speed on EHCI; see
 * stream.h for how the two differ. The microphone, when the card has one, is captured
 * beside it (mic.c).
 *
 * The kernel offers a composite device one interface at a time, having configured it
 * itself: we take its first AudioStreaming one, which does everything through the default
 * endpoint all its interfaces share, and no SET_CONFIGURATION is sent.
 *
 * It is not a network card: its link stays down, so the network layer never picks it
 * as the uplink, and xmit has nothing to do.
 */

#include "driver.h"
#include "audio.h"
#include "stream.h"

#define STEP_MEMORY     1u
#define STEP_CONFIG     2u
#define STEP_NOTHING    3u      /* nothing usable in the descriptors */
#define STEP_SPEED      4u      /* full speed behind a high-speed hub, or an interval past 8 microframes */
#define STEP_INTERFACE  6u
#define STEP_RATE       7u
#define STEP_ENDPOINT   8u
#define STEP_AUDIO      9u
#define STEP_STREAM     10u

#define CONTROL_BYTES   1024u
#define CONTROL_MS      1000u

volatile UAC_STATS uac_stats[NET_MAX];

static UAC devices[NET_MAX];

/* One streaming interface of one card. Set in match, since the next interface is offered
 * before the worker starts the first. */
static volatile u32 taken;

u32 uac_control(UAC *u, u32 type, u32 request, u32 value, u32 index, u32 length, u32 *actual)
{
    u8 setup[8];

    setup[0] = (u8)type;
    setup[1] = (u8)request;
    setup[2] = (u8)value;
    setup[3] = (u8)(value >> 8);
    setup[4] = (u8)index;
    setup[5] = (u8)(index >> 8);
    setup[6] = (u8)length;
    setup[7] = (u8)(length >> 8);
    return usb_control(u->dev, setup, u->control, length, actual, CONTROL_MS);
}

/* The sample rate: on the endpoint for UAC1 (a device with one rate may refuse, which
 * does not matter), on the clock source for UAC2, read back there. */
static u32 set_rate(UAC *u)
{
    const UAC_STREAM *p = &u->info.play;
    u32 actual = 0, status, i;

    if (u->info.version == 1) {
        u->control[0] = (u8)UAC_RATE;
        u->control[1] = (u8)(UAC_RATE >> 8);
        u->control[2] = (u8)(UAC_RATE >> 16);
        uac_control(u, 0x22, 0x01, 0x0100, p->endpoint, 3, &actual);
        return 0;
    }
    if (!u->info.clock)
        return 1;
    for (i = 0; i < 4; i++)
        u->control[i] = (u8)(UAC_RATE >> (8 * i));
    status = uac_control(u, 0x21, 0x01, 0x0100, u->info.clock << 8 | u->info.control_interface, 4,
                     &actual);
    if (!NT_SUCCESS(status))
        return 1;
    status = uac_control(u, 0xA1, 0x01, 0x0100, u->info.clock << 8 | u->info.control_interface, 4,
                     &actual);
    if (!NT_SUCCESS(status) || actual < 4)
        return 1;
    return (u->control[0] | (u32)u->control[1] << 8 | (u32)u->control[2] << 16) != UAC_RATE;
}

static void copy_stream(UAC_STREAM *to, const UAC_STREAM *from)
{
    u32 i;

    for (i = 0; i < sizeof(*to) / sizeof(u32); i++)     /* no memcpy in the plugin */
        ((u32 *)to)[i] = ((const u32 *)from)[i];
}

static u32 uac_start(NET_DEV *nd)
{
    USB_DEV *dev = nd->usb;
    UAC *u = &devices[dev->index];
    u32 status, actual = 0, total, layout, high_speed;

    u->nd = nd;
    u->dev = dev;
    u->st = &uac_stats[dev->index];
    u->stopping = 0;
    u->ep_play = u->ep_feedback = u->ep_mic = 0;
    nd->priv = u;
    if (!u->control)
        u->control = (u8 *)net_dma_scratch(CONTROL_BYTES);
    if (!u->control)
        return STEP_MEMORY;

    /* The whole first configuration: its total length, then all of it. */
    status = uac_control(u, 0x80, 0x06, 0x0200, 0, 9, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return STEP_CONFIG;
    total = u->control[2] | (u32)u->control[3] << 8;
    if (total > CONTROL_BYTES)
        total = CONTROL_BYTES;
    status = uac_control(u, 0x80, 0x06, 0x0200, 0, total, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return STEP_CONFIG;
    if (actual > total)
        actual = total;
    /* The speed (node byte 8 bits 5-6 0x40, xerunner: usb-node-layout@1) before the parse,
     * which keeps the best setting that fits it. A full-speed device behind a high-speed hub
     * would need split transactions (0x800ddab0), not read: refused. */
    high_speed = !USB_ON_COMPANION(dev);
    if (high_speed && (dev->where & 0x60u) != 0x40u)
        return STEP_SPEED;
    if (!uac_parse(u->control, actual, &u->info, high_speed) ||
        (!u->info.play.found && !u->info.play6.found))
        return STEP_NOTHING;
    /* 5.1 to a 5.1 card when the dashboard asks for it; otherwise the stereo mix, on the stereo
     * output if there is one. From here on info.play is the output in use. */
    u->surround = audio_surround();
    layout = AUDIO_STEREO;
    if (u->info.play6.found && (u->surround || !u->info.play.found)) {
        layout = u->surround ? AUDIO_SIX : AUDIO_STEREO_IN_SIX;
        copy_stream(&u->info.play, &u->info.play6);
    }
    u->st->channels = u->info.play.channels;

    if (!high_speed) {
        u->packets = UAC_FS_PACKETS;
        u->period = 1;
        u->feedback_period = 1;
        u->frames_nominal = UAC_RATE / 1000u;
    } else {
        u->packets = UAC_HS_PACKETS;
        u->period = 1u << (u->info.play.interval - 1);
        u->feedback_period = u->info.play.feedback_interval >= 1 &&
            u->info.play.feedback_interval <= 4 ? 1u << (u->info.play.feedback_interval - 1) : 8u;
        u->frames_nominal = UAC_RATE / 8000u * u->period;
    }
    u->st->high_speed = u->packets == UAC_HS_PACKETS;
    u->st->period = u->period;

    u->st->version = u->info.version;
    u->st->play_interface = u->info.play.interface;
    u->st->play_alternate = u->info.play.alternate;
    u->st->play_bytes = u->info.play.bytes;
    u->st->play_endpoint = u->info.play.endpoint;
    u->st->play_max_packet = u->info.play.max_packet;
    u->st->play_sync = u->info.play.sync;
    u->st->feedback_endpoint = u->info.play.feedback;

    status = uac_control(u, 0x01, 0x0B, u->info.play.alternate, u->info.play.interface, 0, 0);
    if (!NT_SUCCESS(status))
        return STEP_INTERFACE;
    if (set_rate(u))
        return STEP_RATE;

    /* High speed: opened with interval 1 whatever the descriptor says, so that the kernel links
     * every one of the endpoint's iTDs into the schedule; the interval is kept by leaving the
     * microframes between empty (stream.c). Opened with 4, the iTDs of the frames in between were
     * never run and the first transfer never completed (17559, 2026-11-08). */
    u->ep_play = usb_open(dev, USB_TYPE_ISOCH, u->info.play.endpoint, u->info.play.max_packet,
                          u->packets == UAC_HS_PACKETS ? 1 : u->info.play.interval);
    if (!u->ep_play)
        return STEP_ENDPOINT;
    if (u->info.play.feedback) {
        u->ep_feedback = usb_open(dev, USB_TYPE_ISOCH, u->info.play.feedback,
                                  u->info.play.feedback_max_packet,
                                  u->packets == UAC_HS_PACKETS || !u->info.play.feedback_interval ? 1 :
                                  u->info.play.feedback_interval);
        if (!u->ep_feedback)
            return STEP_ENDPOINT;
    }
    u->st->audio_open = audio_open(AUDIO_READER_CARD, layout);
    if (u->st->audio_open)
        return STEP_AUDIO;
    if (stream_start(u)) {
        audio_close(AUDIO_READER_CARD);
        return STEP_STREAM;
    }
    if (u->info.mic.found)
        mic_start(u);
    else
        u->st->mic_step = 1;
    return 0;
}

static void uac_detached(NET_DEV *nd)
{
    UAC *u = (UAC *)nd->priv;

    taken = 0;
    if (u) {
        u->stopping = 1;
        audio_close(AUDIO_READER_CARD);
    }
}

/* About every 3 s: the dashboard's setting changed under a card that has a 5.1 output. The
 * device is reset, which is the same path as unplugging it, and chosen for again. */
static void uac_tick(NET_DEV *nd)
{
    UAC *u = (UAC *)nd->priv;

    if (!u || u->stopping || !u->info.play6.found)
        return;
    /* Asked rather than told: the dashboard calls XAudioSetSpeakerConfig through its own
     * import, reloaded with every dash.xex, so a hook would have to be put back each time
     * (counted 2026-11-08: 5 of 5 changes that way, none through xam). The call reads one word. */
    if (++u->ticks < 90)
        return;
    u->ticks = 0;
    if (audio_surround() != u->surround) {
        u->stopping = 1;
        UsbdResetDevice(u->dev->device);
    }
}

static u32 uac_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    (void)nd;
    (void)frame;
    (void)length;
    return 0;
}

static u32 uac_tx_space(NET_DEV *nd)
{
    (void)nd;
    return 0;
}

static u32 uac_match(const u8 *device, const u8 *interface)
{
    /* Microsoft's own audio (Kinect, headsets) stays with the kernel's drivers. */
    if (device[8] == 0x5E && device[9] == 0x04)
        return XE_MATCH_NONE;
    if (!taken && interface && interface[5] == 0x01 && interface[6] == 0x02 && !interface[3] &&
        (interface[7] == 0x00 || interface[7] == 0x20)) {
        taken = 1;
        return XE_MATCH_CLASS;
    }
    return XE_MATCH_NONE;
}

const XE_DRIVER xe_driver_uac = {
    .name = "uac",
    .kind = XE_KIND_AUDIO,
    .match = uac_match,
    .start = uac_start,
    .detached = uac_detached,
    .xmit = uac_xmit,
    .tx_space = uac_tx_space,
    .tick = uac_tick,
};
