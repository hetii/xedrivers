/* See btusb.h. Events and ACL data come in transfers of whatever the endpoint gives; one HCI
 * packet may span several, so each is put together by its header's length before it goes up.
 */

#include "btusb.h"
#include "sco.h"

#define DT_CONFIG       2u
#define DT_INTERFACE    4u
#define DT_ENDPOINT     5u
#define EP_IN           0x80u
#define EP_BULK         2u
#define EP_INTERRUPT    3u
#define CONFIG_MAX      1024u
#define STATUS_UNDERRUN 0xC0050003u     /* an isochronous IN packet shorter than asked: fine */

volatile BTUSB_STATS btusb_stats;
u8 btusb_config[512];                   /* the device's configuration, as read (xectl) */
u32 btusb_config_length;

u32 btusb_find(const u8 *c, u32 length, BTUSB_EPS *e)
{
    u32 at = 0, in_hci = 0, found = 0, i;

    for (i = 0; i < sizeof(*e); i++)
        ((u8 *)e)[i] = 0;
    while (at + 2 <= length) {
        const u8 *d = c + at;
        u32 n = d[0];

        if (n < 2 || at + n > length)
            break;
        if (d[1] == DT_INTERFACE && n >= 9) {
            /* the first HCI interface's first setting: class e0, subclass 01, protocol 01 */
            in_hci = !found && d[3] == 0 && d[5] == 0xE0 && d[6] == 0x01 && d[7] == 0x01;
            if (in_hci) {
                found = 1;
                e->interface = d[2];
            }
        } else if (d[1] == DT_ENDPOINT && n >= 7 && in_hci) {
            u32 type = d[3] & 3u, max = (d[4] | (u32)d[5] << 8) & 0x7FFu;

            if (type == EP_INTERRUPT && (d[2] & EP_IN) && !e->event) {
                e->event = d[2];
                e->event_max = max;
                e->event_interval = d[6];
            } else if (type == EP_BULK && (d[2] & EP_IN) && !e->acl_in) {
                e->acl_in = d[2];
                e->acl_max = max;
            } else if (type == EP_BULK && !(d[2] & EP_IN) && !e->acl_out) {
                e->acl_out = d[2];
            }
        }
        at += n;
    }
    return found && e->event && e->acl_in && e->acl_out;
}

u32 btusb_find_sco(const u8 *c, u32 length, u32 iface, u32 alt, u32 *in, u32 *out, u32 *max)
{
    u32 at = 0, here = 0;

    *in = *out = *max = 0;
    while (at + 2 <= length) {
        const u8 *d = c + at;
        u32 n = d[0];

        if (n < 2 || at + n > length)
            break;
        if (d[1] == DT_INTERFACE && n >= 9)
            here = d[2] == iface && d[3] == alt;
        else if (d[1] == DT_ENDPOINT && n >= 7 && here && (d[3] & 3u) == 1u) {
            if (d[2] & EP_IN)
                *in = d[2];
            else
                *out = d[2];
            *max = (d[4] | (u32)d[5] << 8) & 0x7FFu;
        }
        at += n;
    }
    return *in && *out && *max;
}

/* --- receiving ------------------------------------------------------------------------- */

static void event_ask(BTUSB *b);
static void acl_ask(BTUSB *b);

static void event_done(URB *urb, u32 status)
{
    BTUSB *b = (BTUSB *)urb->Owner;
    u32 n = urb->Actual, i;

    if (!b->running || !b->dev->active)
        return;
    if (!NT_SUCCESS(status)) {
        b->st->event_errors++;
        b->st->last_error = status;
        b->event_have = 0;
        if (++b->errors < 16)
            event_ask(b);
        return;
    }
    b->errors = 0;
    for (i = 0; i < n && b->event_have < BTUSB_EVENT_MAX; i++)
        b->event[b->event_have++] = b->event_buf[i];
    if (b->event_have >= 2 && b->event_have >= 2u + b->event[1]) {
        b->st->events++;
        bt_event(b->event, 2u + b->event[1]);
        b->event_have = 0;
        usb_wake();
    }
    event_ask(b);
}

static void event_ask(BTUSB *b)
{
    usb_prepare(&b->event_urb, b->dev, b->ep_event, event_done, b->event_buf,
                b->eps.event_max ? b->eps.event_max : 64, 0);
    b->event_urb.Owner = b;
    usb_submit(&b->event_urb);
}

static void acl_done(URB *urb, u32 status)
{
    BTUSB *b = (BTUSB *)urb->Owner;
    u32 n = urb->Actual, i, need;

    if (!b->running || !b->dev->active || !b->acl_receiving) {
        b->acl_receiving = 0;
        return;
    }
    if (!NT_SUCCESS(status)) {
        b->st->last_error = status;
        b->acl_have = 0;
        if (++b->errors < 16)
            acl_ask(b);
        else
            b->acl_receiving = 0;
        return;
    }
    for (i = 0; i < n && b->acl_have < BTUSB_ACL_MAX; i++)
        b->acl[b->acl_have++] = b->acl_buf[i];
    need = b->acl_have >= 4 ? 4u + (b->acl[2] | (u32)b->acl[3] << 8) : 0xFFFFFFFFu;
    if (need <= b->acl_have) {
        b->st->acl_in_packets++;
        bt_acl_in(b->acl, need);
        b->acl_have = 0;
        usb_wake();
    } else if (need != 0xFFFFFFFFu && need > BTUSB_ACL_MAX) {
        b->acl_have = 0;                /* larger than we take: dropped whole */
    }
    acl_ask(b);
}

static void acl_ask(BTUSB *b)
{
    usb_prepare(&b->acl_urb, b->dev, b->ep_acl_in, acl_done, b->acl_buf, BTUSB_ACL_MAX, 0);
    b->acl_urb.Owner = b;
    usb_submit(&b->acl_urb);
}

static void acl_receive(void *ctx, u32 on)
{
    BTUSB *b = (BTUSB *)ctx;

    if (on && !b->acl_receiving && b->running) {
        if (!usb_wait_idle(&b->acl_urb, 100))
            return;                     /* the last one not back yet: next tick */
        b->acl_receiving = 1;
        b->acl_have = 0;
        acl_ask(b);
    } else if (!on && b->acl_receiving) {
        b->acl_receiving = 0;
        usb_cancel(&b->acl_urb);
    }
}

/* --- the voice link (SCO) over the isochronous interface ------------------------------ */

static void sco_in_ask(BTUSB *b, u32 slot)
{
    ISOCH *t = &b->sco_in[slot];
    u32 i;

    for (i = 0; i < b->sco_packets; i++)
        b->sco_in_len[slot][i] = i % b->sco_period != b->sco_period - 1 ? 0 : (u16)b->sco_max;
    t->Length = b->sco_packets / b->sco_period * b->sco_max;
    usb_isoch(b->dev, t, b->sco_in_len[slot]);
}

static void sco_in_done(ISOCH *t, u32 *statuses, u16 *lengths)
{
    BTUSB *b = (BTUSB *)t->Owner;
    u32 slot = (u32)(t - b->sco_in), i, at = 0;
    const u8 *base = (const u8 *)t->Buffer;

    b->st->sco_in_requests++;
    for (i = 0; statuses && lengths && i < b->sco_packets; i++) {
        u32 asked = b->sco_in_len[slot][i];

        if (!asked)
            continue;
        if ((statuses[i] == 0 || statuses[i] == STATUS_UNDERRUN) && lengths[i] <= asked) {
            if (lengths[i]) {
                b->st->sco_in_bytes += lengths[i];
                bt_sco_take(base + at, lengths[i]);
            }
        } else {
            b->st->sco_in_bad++;
            b->st->sco_last_status = statuses[i];
        }
        at += asked;
    }
    if (b->sco_running && b->running && b->dev->active)
        sco_in_ask(b, slot);
}

static void sco_out_send(BTUSB *b, u32 slot)
{
    ISOCH *t = &b->sco_out[slot];
    u32 i, at = 0;

    for (i = 0; i < b->sco_packets; i++) {
        u32 n = i % b->sco_period != b->sco_period - 1 ? 0 : b->sco_max;

        b->sco_out_len[slot][i] = (u16)n;
        if (n)
            bt_sco_fill((u8 *)t->Buffer + at, n);
        at += n;
    }
    t->Length = at;
    usb_isoch(b->dev, t, b->sco_out_len[slot]);
}

static void sco_out_done(ISOCH *t, u32 *statuses, u16 *lengths)
{
    BTUSB *b = (BTUSB *)t->Owner;

    (void)statuses;
    (void)lengths;
    b->st->sco_out_requests++;
    if (b->sco_running && b->running && b->dev->active)
        sco_out_send(b, (u32)(t - b->sco_out));
}

static u32 sco(void *ctx, u32 on)
{
    BTUSB *b = (BTUSB *)ctx;
    u8 setup[8] = { 0x01, 0x0B, 0, 0, 0, 0, 0, 0 };     /* SET_INTERFACE */
    u32 slot, room, hs = !USB_ON_COMPANION(b->dev);

    if (!on) {
        b->sco_running = 0;
        return 0;
    }
    if (b->sco_running)
        return 0;
    b->st->sco_starts++;
    if (!btusb_find_sco(btusb_config, btusb_config_length, b->eps.interface + 1, BTUSB_SCO_ALT,
                        &b->sco_in_ep, &b->sco_out_ep, &b->sco_max))
        return b->st->sco_step = 1;
    setup[2] = BTUSB_SCO_ALT;
    setup[4] = (u8)(b->eps.interface + 1);
    if (!NT_SUCCESS(usb_control(b->dev, setup, b->cmd_buf, 0, 0, 1000)))
        return b->st->sco_step = 2;
    /* high speed: 32 microframes a request, data in every 8th (bInterval 4: 1 ms), the
     * endpoint opened at interval 1 as the kernel takes it (as uac does); full: 4 frames */
    b->sco_packets = hs ? 32 : 4;
    b->sco_period = hs ? 8 : 1;
    if (!b->ep_sco_in) {
        b->ep_sco_in = usb_open(b->dev, USB_TYPE_ISOCH, b->sco_in_ep, b->sco_max, 1);
        b->ep_sco_out = usb_open(b->dev, USB_TYPE_ISOCH, b->sco_out_ep, b->sco_max, 1);
    }
    if (!b->ep_sco_in || !b->ep_sco_out)
        return b->st->sco_step = 3;
    room = 4 * b->sco_max;
    if (!b->sco_buf)
        b->sco_buf = (u8 *)MmAllocatePhysicalMemory(2, 2 * BTUSB_SCO_SLOTS * room, 4);
    if (!b->sco_buf)
        return b->st->sco_step = 4;
    b->sco_running = 1;
    mem_sync();
    for (slot = 0; slot < BTUSB_SCO_SLOTS; slot++) {
        ISOCH *in = &b->sco_in[slot], *out = &b->sco_out[slot];

        in->Context = in->Endpoint = b->ep_sco_in;
        in->Done = sco_in_done;
        in->Buffer = b->sco_buf + slot * room;
        in->PacketCount = (u8)b->sco_packets;
        in->Owner = b;
        out->Context = out->Endpoint = b->ep_sco_out;
        out->Done = sco_out_done;
        out->Buffer = b->sco_buf + (BTUSB_SCO_SLOTS + slot) * room;
        out->PacketCount = (u8)b->sco_packets;
        out->Owner = b;
        sco_in_ask(b, slot);
        sco_out_send(b, slot);
    }
    b->st->sco_step = 0;
    return 0;
}

/* --- sending --------------------------------------------------------------------------- */

static u32 send_command(void *ctx, const u8 *packet, u32 length)
{
    BTUSB *b = (BTUSB *)ctx;
    u8 setup[8] = { 0x20, 0x00, 0, 0, 0, 0, 0, 0 };     /* class, to the device */
    u32 i;

    if (!b->running || length > 258)
        return 1;
    for (i = 0; i < length; i++)
        b->cmd_buf[i] = packet[i];
    setup[4] = (u8)b->eps.interface;
    setup[6] = (u8)length;
    setup[7] = (u8)(length >> 8);
    b->st->commands++;
    if (!NT_SUCCESS(usb_control(b->dev, setup, b->cmd_buf, length, 0, 1000))) {
        b->st->command_errors++;
        return 1;
    }
    return 0;
}

static void out_done(URB *urb, u32 status)
{
    BTUSB *b = (BTUSB *)urb->Owner;

    if (!NT_SUCCESS(status))
        b->st->last_error = status;
    b->out_busy[urb - b->out_urb] = 0;
}

static u32 send_acl(void *ctx, const u8 *packet, u32 length)
{
    BTUSB *b = (BTUSB *)ctx;
    u32 s, i;

    if (!b->running || length > BTUSB_ACL_MAX)
        return 1;
    for (s = 0; s < BTUSB_OUT; s++)
        if (!b->out_busy[s])
            break;
    if (s == BTUSB_OUT) {
        b->st->acl_out_full++;
        return 1;
    }
    b->out_busy[s] = 1;
    for (i = 0; i < length; i++)
        b->out_buf[s][i] = packet[i];
    usb_prepare(&b->out_urb[s], b->dev, b->ep_acl_out, out_done, b->out_buf[s], length, 0);
    b->out_urb[s].Owner = b;
    b->st->acl_out_packets++;
    usb_submit(&b->out_urb[s]);
    return 0;
}

/* --- bring-up -------------------------------------------------------------------------- */

static u32 start(BTUSB *b, USB_DEV *dev, const u8 *taken);

u32 btusb_start(BTUSB *b, USB_DEV *dev, const u8 *taken)
{
    u32 status = start(b, dev, taken);

    btusb_stats.start_status = status;
    return status;
}

static u32 start(BTUSB *b, USB_DEV *dev, const u8 *taken)
{
    u8 setup[8] = { 0x80, 0x06, 0x00, DT_CONFIG, 0, 0, 9, 0 };
    u32 actual = 0, total, s;
    u8 *block;

    b->dev = dev;
    b->st = &btusb_stats;
    b->running = 0;
    b->errors = 0;
    b->event_have = b->acl_have = 0;
    if (!b->cmd_buf) {
        /* one block: config (1 KB), command, event, ACL in, BTUSB_OUT ACL out */
        block = (u8 *)MmAllocatePhysicalMemory(2, CONFIG_MAX + 512 + 512 +
                                                BTUSB_ACL_MAX * (1 + BTUSB_OUT) + 64, 4);
        if (!block)
            return BTUSB_START_MEMORY;
        b->cmd_buf = block + CONFIG_MAX;
        b->event_buf = b->cmd_buf + 512;
        b->acl_buf = b->event_buf + 512;
        for (s = 0; s < BTUSB_OUT; s++)
            b->out_buf[s] = b->acl_buf + BTUSB_ACL_MAX * (1 + s);
    }
    block = b->cmd_buf - CONFIG_MAX;
    if (!NT_SUCCESS(usb_control(dev, setup, block, 9, &actual, 1000)) || actual < 9)
        return BTUSB_START_CONFIG;
    total = block[2] | (u32)block[3] << 8;
    if (total > CONFIG_MAX)
        total = CONFIG_MAX;
    setup[6] = (u8)total;
    setup[7] = (u8)(total >> 8);
    if (!NT_SUCCESS(usb_control(dev, setup, block, total, &actual, 1000)) || actual < 9)
        return BTUSB_START_CONFIG;
    btusb_config_length = actual > total ? total : actual;
    if (btusb_config_length > sizeof(btusb_config))
        btusb_config_length = sizeof(btusb_config);
    for (s = 0; s < btusb_config_length; s++)
        btusb_config[s] = block[s];
    if (!btusb_find(block, actual > total ? total : actual, &b->eps))
        return BTUSB_START_NOT_HCI;
    b->st->interface = b->eps.interface;
    b->st->ep_event = b->eps.event;
    b->st->ep_acl_in = b->eps.acl_in;
    b->st->ep_acl_out = b->eps.acl_out;
    b->st->acl_max = b->eps.acl_max;
    for (s = 0; taken && taken[s]; s++)
        if (taken[s] == b->eps.event || taken[s] == b->eps.acl_in || taken[s] == b->eps.acl_out)
            return BTUSB_START_SHARED;
    if (!b->ep_event) {
        b->ep_event = usb_open(dev, USB_TYPE_INTERRUPT, b->eps.event, b->eps.event_max,
                               b->eps.event_interval);
        b->ep_acl_in = usb_open(dev, USB_TYPE_BULK, b->eps.acl_in, b->eps.acl_max, 0);
        b->ep_acl_out = usb_open(dev, USB_TYPE_BULK, b->eps.acl_out, b->eps.acl_max, 0);
    }
    if (!b->ep_event || !b->ep_acl_in || !b->ep_acl_out)
        return BTUSB_START_ENDPOINTS;
    for (s = 0; s < BTUSB_OUT; s++)
        b->out_busy[s] = 0;
    b->transport.ctx = b;
    b->transport.command = send_command;
    b->transport.acl = send_acl;
    b->transport.acl_receive = acl_receive;
    b->transport.sco = sco;
    b->acl_receiving = 0;
    b->running = 1;
    mem_sync();
    event_ask(b);
    bt_attach(&b->transport);
    b->st->started++;
    return 0;
}

void btusb_stop(BTUSB *b)
{
    b->sco_running = 0;
    b->running = 0;
    b->acl_receiving = 0;
    mem_sync();
    bt_detach();
}
