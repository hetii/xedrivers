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
