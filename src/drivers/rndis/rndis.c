/* RNDIS messages and descriptors; see rndis.h. Offsets from [MS-RNDIS] 2.2: every
 * message starts with MessageType and MessageLength, a request/completion pair
 * shares a RequestId, and an "offset" is counted from the field after
 * MessageLength (byte 8) -- for a PACKET_MSG from its DataOffset field, likewise 8.
 */

#include "rndis.h"

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void wr32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

int rndis_is_control(u32 class_code, u32 subclass, u32 protocol)
{
    return (class_code == 0xE0 && subclass == 0x01 && protocol == 0x03) ||
           (class_code == 0x02 && subclass == 0x02 && protocol == 0xFF) ||
           (class_code == 0xEF && subclass == 0x04 && protocol == 0x01);
}

/* The control interface, its interrupt endpoint, then the first later interface
 * with a bulk pair -- the data interface (class 0A, CDC data, on every device
 * seen; not required). */
int rndis_parse_config(const u8 *config, u32 length, RNDIS_LAYOUT *out)
{
    u32 at, len;
    int in_control = 0, have_control = 0, candidate = 0;
    u8 iface = 0, alt = 0, in = 0, out_ep = 0;
    u16 in_max = 0, out_max = 0;

    if (length < 9 || config[1] != 0x02)
        return 0;
    for (at = 0; at < sizeof(*out); at++)
        ((u8 *)out)[at] = 0;
    out->config_value = config[5];

    for (at = 0; at + 2 <= length; at += len) {
        len = config[at];
        if (len < 2 || at + len > length)
            return 0;
        if (config[at + 1] == 0x04 && len >= 9) {             /* interface */
            if (candidate && in && out_ep)
                break;
            iface = config[at + 2];
            alt = config[at + 3];
            in = out_ep = 0;
            in_control = rndis_is_control(config[at + 5], config[at + 6],
                                          config[at + 7]);
            if (in_control) {
                out->comm_interface = iface;
                have_control = 1;
            }
            candidate = have_control && !in_control;
        } else if (config[at + 1] == 0x05 && len >= 7) {      /* endpoint */
            u8 address = config[at + 2], type = config[at + 3] & 3;
            u16 max = le16(config + at + 4) & 0x7FF;

            if (in_control && type == 3 && (address & 0x80)) {
                out->ep_intr = address;
                out->intr_max = max;
                out->intr_interval = config[at + 6];
            } else if (candidate && type == 2) {
                if (address & 0x80) {
                    in = address;
                    in_max = max;
                } else {
                    out_ep = address;
                    out_max = max;
                }
            }
        }
    }
    if (!candidate || !in || !out_ep)
        return 0;
    out->data_interface = iface;
    out->data_alt = alt;
    out->ep_in = in;
    out->ep_out = out_ep;
    out->in_max = in_max;
    out->out_max = out_max;
    return 1;
}

u32 rndis_build_init(u8 *buf, u32 request, u32 max_transfer)
{
    wr32(buf + 0, RNDIS_MSG_INIT);
    wr32(buf + 4, 24);
    wr32(buf + 8, request);
    wr32(buf + 12, 1);              /* MajorVersion */
    wr32(buf + 16, 0);              /* MinorVersion */
    wr32(buf + 20, max_transfer);   /* the largest transfer we take from it */
    return 24;
}

u32 rndis_build_query(u8 *buf, u32 request, u32 oid)
{
    wr32(buf + 0, RNDIS_MSG_QUERY);
    wr32(buf + 4, 28);
    wr32(buf + 8, request);
    wr32(buf + 12, oid);
    wr32(buf + 16, 0);              /* InformationBufferLength */
    wr32(buf + 20, 0);              /* InformationBufferOffset */
    wr32(buf + 24, 0);              /* DeviceVcHandle */
    return 28;
}

u32 rndis_build_set(u8 *buf, u32 request, u32 oid, const u8 *value, u32 length)
{
    u32 i;

    wr32(buf + 0, RNDIS_MSG_SET);
    wr32(buf + 4, 28 + length);
    wr32(buf + 8, request);
    wr32(buf + 12, oid);
    wr32(buf + 16, length);
    wr32(buf + 20, 20);             /* the value follows, at 8 + 20 */
    wr32(buf + 24, 0);
    for (i = 0; i < length; i++)
        buf[28 + i] = value[i];
    return 28 + length;
}

u32 rndis_build_keepalive_reply(u8 *buf, u32 request)
{
    wr32(buf + 0, RNDIS_MSG_KEEPALIVE | RNDIS_MSG_COMPLETION);
    wr32(buf + 4, 16);
    wr32(buf + 8, request);
    wr32(buf + 12, RNDIS_STATUS_SUCCESS);
    return 16;
}

int rndis_parse_reply(const u8 *buf, u32 length, RNDIS_REPLY *out)
{
    u32 msg_length;

    out->type = out->request = out->status = 0;
    out->info = 0;
    out->info_length = 0;
    out->max_packets = out->max_transfer = out->alignment = 0;
    if (length < 12)
        return 0;
    out->type = le32(buf);
    msg_length = le32(buf + 4);
    if (msg_length < 12 || msg_length > length)
        return 0;

    if (out->type == RNDIS_MSG_INDICATE) {          /* no RequestId: Status at 8 */
        out->status = le32(buf + 8);
        return 1;
    }
    out->request = le32(buf + 8);
    if (out->type == RNDIS_MSG_KEEPALIVE)
        return 1;
    if (msg_length < 16)
        return 0;
    out->status = le32(buf + 12);

    if (out->type == (RNDIS_MSG_INIT | RNDIS_MSG_COMPLETION) && msg_length >= 44) {
        out->max_packets = le32(buf + 32);
        out->max_transfer = le32(buf + 36);
        out->alignment = le32(buf + 40);
    } else if (out->type == (RNDIS_MSG_QUERY | RNDIS_MSG_COMPLETION) &&
               msg_length >= 24) {
        u32 info_length = le32(buf + 16), offset = le32(buf + 20);

        if (info_length) {
            if (8 + offset + info_length > msg_length)
                return 0;
            out->info = buf + 8 + offset;
            out->info_length = info_length;
        }
    }
    return 1;
}

u32 rndis_wrap(u8 *buf, u32 buf_size, const u8 *frame, u32 length)
{
    u32 total = RNDIS_PACKET_HEADER + length, i;

    if (total > buf_size)
        return 0;
    wr32(buf + 0, RNDIS_MSG_PACKET);
    wr32(buf + 4, total);
    wr32(buf + 8, RNDIS_PACKET_HEADER - 8);         /* DataOffset, from byte 8 */
    wr32(buf + 12, length);
    for (i = 16; i < RNDIS_PACKET_HEADER; i += 4)   /* no OOB data, no per-packet */
        wr32(buf + i, 0);                           /* info, no VC */
    xe_copy(buf + RNDIS_PACKET_HEADER, frame, length);
    return total;
}

u32 rndis_wrap_aligned(u8 *buf, u32 buf_size, const u8 *frame, u32 length,
                       u32 align)
{
    u32 total = RNDIS_PACKET_HEADER + length, padded, i;

    if (!align)
        align = 1;
    padded = (total + align - 1) & ~(align - 1);
    if (padded > buf_size || !rndis_wrap(buf, buf_size, frame, length))
        return 0;
    for (i = total; i < padded; i++)
        buf[i] = 0;
    wr32(buf + 4, padded);
    return padded;
}

int rndis_unwrap(const u8 *buf, u32 length, RNDIS_FRAME each, void *context)
{
    u32 at = 0, frames = 0;

    while (at + 8 <= length) {
        u32 type = le32(buf + at), msg_length = le32(buf + at + 4);
        u32 offset, data_length;

        if (type == 0 && msg_length == 0)
            break;                                  /* padding after the last */
        if (type != RNDIS_MSG_PACKET || msg_length < RNDIS_PACKET_HEADER ||
            at + msg_length > length)
            return -1;
        offset = le32(buf + at + 8);
        data_length = le32(buf + at + 12);
        if (8 + offset + data_length > msg_length)
            return -1;
        each(buf + at + 8 + offset, data_length, context);
        frames++;
        at += msg_length;
    }
    return (int)frames;
}
