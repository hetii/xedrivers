/* CDC ECM/NCM descriptors and notifications (USB CDC 1.2, ECM 1.2, NCM 1.0).
 *
 * The RTL8156B offers three configurations (read off the card with lsusb):
 *   1  vendor class FF/FF -- what Linux r8152 drives, needing the whole Realtek
 *      register bring-up
 *   2  CDC NCM: comm interface 0 (interrupt 0x83, 16 bytes, bInterval 11), data
 *      interface 1, alt 1 with bulk 0x81 IN / 0x02 OUT, 512 bytes
 *   3  CDC ECM: the same shape with subclass 6
 * In 2 and 3 the card's firmware runs its own MAC and PHY, so there is no register
 * sequence to port -- only descriptor reading, a filter write, and frame movement.
 *
 * Everything here is a pure function over bytes; tests/ builds this file natively.
 */

#include "cdc.h"

#define DT_INTERFACE     0x04
#define DT_ENDPOINT      0x05
#define DT_CS_INTERFACE  0x24
#define CDC_FD_ETHERNET  0x0F

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | (p[1] << 8));
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static int hex_nibble(u8 c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

int cdc_parse_config(const u8 *config, u32 length, CDC_LAYOUT *out)
{
    u32 at;
    u8 cur_iface = 0xFF;
    u8 cur_alt = 0;
    int on_comm = 0;
    int on_data = 0;
    u16 total;

    out->config_value = 0;
    out->subclass = 0;
    out->comm_interface = 0xFF;
    out->data_interface = 0xFF;
    out->data_alt = 0;
    out->ep_in = 0;
    out->ep_out = 0;
    out->ep_intr = 0;
    out->in_max = 0;
    out->out_max = 0;
    out->intr_max = 0;
    out->intr_interval = 0;
    out->mac_string = 0;
    out->max_segment = 0;

    if (length < 9 || config[1] != 0x02)
        return 0;
    total = le16(config + 2);
    if (total < length)
        length = total;
    out->config_value = config[5];

    /* Walk the descriptors. A descriptor's first byte is its length; a zero length
     * would never advance, so it ends the walk. */
    at = config[0];
    while (at + 2 <= length) {
        u8 dlen = config[at];
        u8 dtype = config[at + 1];

        if (dlen < 2 || at + dlen > length)
            break;

        if (dtype == DT_INTERFACE && dlen >= 9) {
            u8 cls = config[at + 5];
            u8 sub = config[at + 6];

            cur_iface = config[at + 2];
            cur_alt = config[at + 3];
            on_comm = 0;
            on_data = 0;

            if (cls == 0x02 && (sub == CDC_SUBCLASS_ECM || sub == CDC_SUBCLASS_NCM)) {
                /* The communications interface of the function we want. */
                out->subclass = sub;
                out->comm_interface = cur_iface;
                on_comm = 1;
            } else if (cls == 0x0A && cur_iface == out->comm_interface + 1) {
                /* The data interface that pairs with it; alt 1 carries the bulk
                 * endpoints, alt 0 is the zero-bandwidth default. */
                out->data_interface = cur_iface;
                on_data = 1;
            }
        } else if (dtype == DT_CS_INTERFACE && on_comm && dlen >= 3 &&
                   config[at + 2] == CDC_FD_ETHERNET && dlen >= 13) {
            /* Ethernet Networking functional descriptor: iMACAddress at +3,
             * wMaxSegmentSize at +8. */
            out->mac_string = config[at + 3];
            out->max_segment = le16(config + at + 8);
        } else if (dtype == DT_ENDPOINT && dlen >= 7) {
            u8 addr = config[at + 2];
            u8 attr = config[at + 3] & 0x03;
            u16 mps = le16(config + at + 4);

            if (on_comm && attr == 0x03) {
                out->ep_intr = addr;
                out->intr_max = mps;
                out->intr_interval = config[at + 6];
            } else if (on_data && attr == 0x02) {
                if (addr & 0x80) {
                    out->ep_in = addr;
                    out->in_max = mps;
                } else {
                    out->ep_out = addr;
                    out->out_max = mps;
                }
                out->data_alt = cur_alt;
            }
        }

        at += dlen;
    }

    return out->comm_interface != 0xFF && out->data_interface != 0xFF &&
           out->ep_in != 0 && out->ep_out != 0;
}

int cdc_parse_mac(const u8 *string, u32 length, u8 mac[6])
{
    u32 i;

    /* A string descriptor: bLength, bDescriptorType 0x03, then UTF-16LE. ECM gives
     * the MAC as twelve uppercase hex characters, one byte per character. */
    if (length < 2 || string[1] != 0x03)
        return 0;
    if (string[0] < 2 + 12 * 2 || string[0] > length)
        return 0;

    for (i = 0; i < 6; i++) {
        int hi = hex_nibble(string[2 + i * 4]);
        int lo = hex_nibble(string[2 + i * 4 + 2]);

        if (hi < 0 || lo < 0)
            return 0;
        mac[i] = (u8)((hi << 4) | lo);
    }
    return 1;
}

u32 cdc_parse_notification(const u8 *data, u32 length, CDC_NOTE *note)
{
    u8 req_type;
    u8 notification;
    u16 value;
    u16 wlength;

    note->kind = CDC_NOTE_NONE;
    note->link_up = 0;
    note->down_bps = 0;
    note->up_bps = 0;

    if (length < 8)
        return 0;

    req_type = data[0];             /* 0xA1: class, interface, device-to-host */
    notification = data[1];
    value = le16(data + 2);
    wlength = le16(data + 6);

    if (req_type != 0xA1)
        return 0;

    if (notification == 0x00) {             /* NETWORK_CONNECTION */
        note->kind = CDC_NOTE_LINK;
        note->link_up = value ? 1 : 0;
        return 8;
    }
    if (notification == 0x2A) {             /* CONNECTION_SPEED_CHANGE */
        if (wlength < 8 || length < (u32)(8 + wlength))
            return 0;
        note->kind = CDC_NOTE_SPEED;
        note->down_bps = le32(data + 8);
        note->up_bps = le32(data + 12);
        return 8 + wlength;
    }

    return 8;                               /* an unknown 8-byte notification */
}

/* --- NCM ------------------------------------------------------------------- */

#define NTH16_SIGNATURE 0x484D434Eu     /* "NCMH" */
#define NDP16_NOCRC     0x304D434Eu     /* "NCM0" */
#define NDP16_CRC       0x314D434Eu     /* "NCM1" */
#define NTH16_LEN       12u
#define NDP16_TX_AT     12u
#define NDP16_TX_LEN    (8u + 4u * (NCM_TX_MAX_DATAGRAMS + 1u))

static void wr16le(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void wr32le(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

int cdc_ncm_parse_params(const u8 *d, u32 length, NCM_PARAMS *out)
{
    if (length < 28 || le16(d) < 28)
        return 0;
    out->formats = le16(d + 2);
    out->in_max = le32(d + 4);
    out->in_divisor = le16(d + 8);
    out->in_remainder = le16(d + 10);
    out->in_alignment = le16(d + 12);
    out->out_max = le32(d + 16);
    out->out_divisor = le16(d + 20);
    out->out_remainder = le16(d + 22);
    out->out_alignment = le16(d + 24);
    out->out_max_datagrams = le16(d + 26);
    return 1;
}

int cdc_ncm_parse_ntb(const u8 *ntb, u32 length, NCM_DATAGRAM each, void *context)
{
    u32 block, ndp, guard, count = 0;

    if (length < NTH16_LEN || le32(ntb) != NTH16_SIGNATURE || le16(ntb + 4) != 12)
        return -1;
    block = le16(ntb + 8);
    if (block && block < length)
        length = block;
    ndp = le16(ntb + 10);

    /* NDPs chain through wNextNdpIndex; a device normally sends one. The guard
     * stops a loop in a corrupt block from spinning at DISPATCH_LEVEL. */
    for (guard = 0; ndp && guard < 8; guard++) {
        u32 sig, ndp_len, at;

        if (ndp + 8 > length || (ndp & 3))
            return -1;
        sig = le32(ntb + ndp);
        ndp_len = le16(ntb + ndp + 4);
        if ((sig != NDP16_NOCRC && sig != NDP16_CRC) || ndp_len < 16 ||
            ndp + ndp_len > length)
            return -1;

        for (at = ndp + 8; at + 4 <= ndp + ndp_len; at += 4) {
            u32 index = le16(ntb + at);
            u32 len = le16(ntb + at + 2);

            if (!index || !len)
                break;                          /* the terminating null entry */
            if (index + len > length)
                return -1;
            each(ntb + index, len, context);
            count++;
        }
        ndp = le16(ntb + ndp + 6);
    }
    return (int)count;
}

/* Where the next datagram may start. wNdpOutPayloadRemainder is about the
 * datagram's PAYLOAD -- what follows its 14-byte Ethernet header -- not about the
 * frame's first byte (NCM 1.0, 3.3.1; Linux cdc_ncm_setup subtracts the header the
 * same way). Read as the frame's own offset, it puts every IP header off its
 * alignment, and the RTL8156B then accepts each NTB and transmits nothing --
 * found by comparing a usbmon capture of cdc_ncm with ours. */
static u32 place(const NCM_TX *tx, u32 at)
{
    u32 d = tx->divisor ? tx->divisor : 4u;
    u32 r = (tx->remainder + d * 16u - 14u) % d;

    while (at % d != r)
        at++;
    return at;
}

void cdc_ncm_tx_begin(NCM_TX *tx, u8 *buf, u32 buf_size, const NCM_PARAMS *p,
                      u16 sequence, u32 ndp_end)
{
    tx->buf = buf;
    tx->capacity = p->out_max && p->out_max < buf_size ? p->out_max : buf_size;
    tx->ndp_end = ndp_end;
    tx->alignment = p->out_alignment ? p->out_alignment : 4u;
    tx->used = ndp_end ? NTH16_LEN : NDP16_TX_AT + NDP16_TX_LEN;
    tx->count = 0;
    tx->divisor = p->out_divisor;
    tx->remainder = p->out_remainder;
    tx->max_datagrams = p->out_max_datagrams && p->out_max_datagrams <
                        NCM_TX_MAX_DATAGRAMS ? p->out_max_datagrams
                                             : NCM_TX_MAX_DATAGRAMS;
    tx->sequence = sequence;
}

static u32 align_up(u32 at, u32 a)
{
    return (at + a - 1) / a * a;
}

int cdc_ncm_tx_add(NCM_TX *tx, const u8 *frame, u32 length)
{
    u32 at, tail;

    if (tx->count >= tx->max_datagrams)
        return 0;
    at = place(tx, tx->used);
    /* Room for the NDP when it goes last, and for the padding byte finish() may
     * need either way. */
    tail = tx->ndp_end ? tx->alignment + 8 + 4 * (tx->count + 2) : 0;
    if (at + length + tail + 1 > tx->capacity)
        return 0;
    xe_copy(tx->buf + at, frame, length);
    tx->entry[tx->count][0] = (u16)at;
    tx->entry[tx->count][1] = (u16)length;
    tx->used = at + length;
    tx->count++;
    return 1;
}

u32 cdc_ncm_tx_finish(NCM_TX *tx, u32 max_packet)
{
    u8 *b = tx->buf;
    u32 ndp, ndp_len = 8 + 4 * (tx->count + 1), len, i;

    if (tx->ndp_end) {
        ndp = align_up(tx->used, tx->alignment);
        for (i = tx->used; i < ndp; i++)
            b[i] = 0;
        len = ndp + ndp_len;
    } else {
        ndp = NDP16_TX_AT;
        len = tx->used;
    }

    wr32le(b + ndp, NDP16_NOCRC);
    wr16le(b + ndp + 4, ndp_len);
    wr16le(b + ndp + 6, 0);
    for (i = 0; i < tx->count; i++) {
        wr16le(b + ndp + 8 + 4 * i, tx->entry[i][0]);
        wr16le(b + ndp + 8 + 4 * i + 2, tx->entry[i][1]);
    }
    wr16le(b + ndp + 8 + 4 * tx->count, 0);     /* the terminating null entry */
    wr16le(b + ndp + 8 + 4 * tx->count + 2, 0);

    if (max_packet && (len % max_packet) == 0 && len < tx->capacity)
        b[len++] = 0;

    wr32le(b, NTH16_SIGNATURE);
    wr16le(b + 4, NTH16_LEN);
    wr16le(b + 6, tx->sequence);
    wr16le(b + 8, len);
    wr16le(b + 10, ndp);
    return len;
}
