/* USB Audio Class: any UAC1 or UAC2 device with a stereo output at 48 kHz plays the
 * console's sound. Chosen from the device's own descriptors (descriptors.c), not a list
 * of products. Full speed only: the kernel's isochronous path was read for OHCI, so a
 * device on an EHCI controller is refused rather than tried. The microphone is not done
 * yet.
 *
 * The kernel offers a composite device one interface at a time, having configured it
 * itself: we take the AudioStreaming ones, the first of a device does everything through
 * the default endpoint they share, and no SET_CONFIGURATION is sent. A second streaming
 * interface (the microphone's) stays idle, holding its slot.
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
#define STEP_HIGH_SPEED 4u
#define STEP_BUSY       5u      /* a sibling interface, or another card, already plays */
#define STEP_INTERFACE  6u
#define STEP_RATE       7u
#define STEP_ENDPOINT   8u
#define STEP_AUDIO      9u
#define STEP_STREAM     10u

#define CONTROL_BYTES   1024u
#define CONTROL_MS      1000u

volatile UAC_STATS uac_stats[NET_MAX];

static UAC devices[NET_MAX];
static UAC *playing;            /* one card at a time */

static u32 control(UAC *u, u32 type, u32 request, u32 value, u32 index, u32 length, u32 *actual)
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
        control(u, 0x22, 0x01, 0x0100, p->endpoint, 3, &actual);
        return 0;
    }
    if (!u->info.clock)
        return 1;
    for (i = 0; i < 4; i++)
        u->control[i] = (u8)(UAC_RATE >> (8 * i));
    status = control(u, 0x21, 0x01, 0x0100, u->info.clock << 8 | u->info.control_interface, 4,
                     &actual);
    if (!NT_SUCCESS(status))
        return 1;
    status = control(u, 0xA1, 0x01, 0x0100, u->info.clock << 8 | u->info.control_interface, 4,
                     &actual);
    if (!NT_SUCCESS(status) || actual < 4)
        return 1;
    return (u->control[0] | (u32)u->control[1] << 8 | (u32)u->control[2] << 16) != UAC_RATE;
}

static u32 uac_start(NET_DEV *nd)
{
    USB_DEV *dev = nd->usb;
    UAC *u = &devices[dev->index];
    u32 status, actual = 0, total;

    u->nd = nd;
    u->dev = dev;
    u->st = &uac_stats[dev->index];
    u->stopping = 0;
    u->ep_play = u->ep_feedback = 0;
    nd->priv = u;
    if ((dev->where & 3u) < 2u)
        return STEP_HIGH_SPEED;
    if (playing)
        return STEP_BUSY;
    if (!u->control)
        u->control = (u8 *)net_dma_scratch(CONTROL_BYTES);
    if (!u->control)
        return STEP_MEMORY;

    /* The whole first configuration: its total length, then all of it. */
    status = control(u, 0x80, 0x06, 0x0200, 0, 9, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return STEP_CONFIG;
    total = u->control[2] | (u32)u->control[3] << 8;
    if (total > CONTROL_BYTES)
        total = CONTROL_BYTES;
    status = control(u, 0x80, 0x06, 0x0200, 0, total, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return STEP_CONFIG;
    if (!uac_parse(u->control, actual, &u->info) || !u->info.play.found)
        return STEP_NOTHING;

    u->st->version = u->info.version;
    u->st->play_interface = u->info.play.interface;
    u->st->play_alternate = u->info.play.alternate;
    u->st->play_bytes = u->info.play.bytes;
    u->st->play_endpoint = u->info.play.endpoint;
    u->st->play_max_packet = u->info.play.max_packet;
    u->st->play_sync = u->info.play.sync;
    u->st->feedback_endpoint = u->info.play.feedback;

    status = control(u, 0x01, 0x0B, u->info.play.alternate, u->info.play.interface, 0, 0);
    if (!NT_SUCCESS(status))
        return STEP_INTERFACE;
    if (set_rate(u))
        return STEP_RATE;

    u->ep_play = usb_open(dev, USB_TYPE_ISOCH, u->info.play.endpoint, u->info.play.max_packet,
                          u->info.play.interval);
    if (!u->ep_play)
        return STEP_ENDPOINT;
    if (u->info.play.feedback) {
        u->ep_feedback = usb_open(dev, USB_TYPE_ISOCH, u->info.play.feedback,
                                  u->info.play.feedback_max_packet, 1);
        if (!u->ep_feedback)
            return STEP_ENDPOINT;
    }
    u->st->audio_open = audio_open();
    if (u->st->audio_open)
        return STEP_AUDIO;
    if (stream_start(u)) {
        audio_close();
        return STEP_STREAM;
    }
    playing = u;
    return 0;
}

static void uac_detached(NET_DEV *nd)
{
    UAC *u = (UAC *)nd->priv;

    if (u) {
        u->stopping = 1;
        if (playing == u) {
            playing = 0;
            audio_close();
        }
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
    if (interface && interface[5] == 0x01 && interface[6] == 0x02 && !interface[3] &&
        (interface[7] == 0x00 || interface[7] == 0x20))
        return XE_MATCH_CLASS;
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
};
