/* ASIX shared code; see asix.h. */

#include "asix.h"
#include "net.h"

#define DESC_INTERFACE  4u
#define DESC_ENDPOINT   5u
#define EP_BULK         2u
#define EP_INTERRUPT    3u

u32 asix_parse_config(const u8 *d, u32 length, ASIX_LAYOUT *out)
{
    u32 at = 0, in_iface = 0, iface = 0;
    ASIX_LAYOUT l;

    if (length < 9 || d[1] != 2)
        return 0;
    l.config_value = d[5];
    l.interface = 0;
    l.ep_in = l.ep_out = l.ep_intr = 0;
    l.in_max = l.out_max = l.intr_max = 0;
    l.intr_interval = 0;
    while (at + 2 <= length) {
        u32 n = d[at], type = d[at + 1];

        if (n < 2 || at + n > length)
            break;
        if (type == DESC_INTERFACE && n >= 9) {
            if (l.ep_in && l.ep_out)
                break;                          /* the first complete one wins */
            iface = d[at + 2];
            in_iface = 1;
            l.ep_in = l.ep_out = l.ep_intr = 0;
        } else if (type == DESC_ENDPOINT && n >= 7 && in_iface) {
            u32 address = d[at + 2], kind = d[at + 3] & 3u, max = asix_le16(d + at + 4);

            if (kind == EP_BULK && (address & 0x80u) && !l.ep_in) {
                l.ep_in = (u8)address;
                l.in_max = (u16)(max & 0x7FFu);
            } else if (kind == EP_BULK && !(address & 0x80u) && !l.ep_out) {
                l.ep_out = (u8)address;
                l.out_max = (u16)(max & 0x7FFu);
            } else if (kind == EP_INTERRUPT && (address & 0x80u) && !l.ep_intr) {
                l.ep_intr = (u8)address;
                l.intr_max = (u16)(max & 0x7FFu);
                l.intr_interval = d[at + 6];
            }
            l.interface = (u8)iface;
        }
        at += n;
    }
    if (!l.ep_in || !l.ep_out)
        return 0;
    *out = l;
    return 1;
}

u32 asix_mii_resolve(u32 bmsr, u32 advertise, u32 lpa, u32 ctrl1000, u32 stat1000,
                     u32 *full)
{
    u32 common = advertise & lpa;

    *full = 0;
    if (!(bmsr & BMSR_LSTATUS))
        return 0;
    if ((ctrl1000 & ADVERTISE_1000FULL) && (stat1000 & LPA_1000FULL)) {
        *full = 1;
        return 1000;
    }
    if (common & ADVERTISE_100FULL) {
        *full = 1;
        return 100;
    }
    if (common & ADVERTISE_100HALF)
        return 100;
    if (common & ADVERTISE_10FULL)
        *full = 1;
    return 10;
}

/* Per device slot: DMA memory from the core's scratch block. */
static u8 *scratch_of[USB_MAX_DEVICES];

static u8 *scratch(USB_DEV *dev)
{
    if (!scratch_of[dev->index])
        scratch_of[dev->index] = (u8 *)net_dma_scratch(ASIX_CONTROL_MAX);
    return scratch_of[dev->index];
}

static u32 control(USB_DEV *dev, u8 type, u8 request, u16 value, u16 index, u8 *buf,
                   u16 length, u32 *actual)
{
    u8 setup[8];

    setup[0] = type;
    setup[1] = request;
    setup[2] = (u8)value;
    setup[3] = (u8)(value >> 8);
    setup[4] = (u8)index;
    setup[5] = (u8)(index >> 8);
    setup[6] = (u8)length;
    setup[7] = (u8)(length >> 8);
    return usb_control(dev, setup, buf, length, actual, 500);
}

u32 asix_read(USB_DEV *dev, u8 request, u16 value, u16 index, u8 *data, u16 length)
{
    u8 *buf = scratch(dev);
    u32 status, i, actual = 0;

    if (!buf || length > ASIX_CONTROL_MAX)
        return STATUS_UNSUCCESSFUL;
    status = control(dev, 0xC0, request, value, index, buf, length, &actual);
    if (NT_SUCCESS(status) && actual < length)
        status = STATUS_UNSUCCESSFUL;
    if (NT_SUCCESS(status))
        for (i = 0; i < length; i++)
            data[i] = buf[i];
    return status;
}

u32 asix_write(USB_DEV *dev, u8 request, u16 value, u16 index, const u8 *data,
               u16 length)
{
    u8 *buf = scratch(dev);
    u32 i;

    if (!buf || length > ASIX_CONTROL_MAX)
        return STATUS_UNSUCCESSFUL;
    for (i = 0; i < length; i++)
        buf[i] = data[i];
    return control(dev, 0x40, request, value, index, buf, length, 0);
}

u32 asix_get_config(USB_DEV *dev, u8 *out, u32 size, u32 *length)
{
    u32 status, actual = 0, total;

    status = control(dev, 0x80, 0x06, 0x0200, 0, out, 9, &actual);
    if (!NT_SUCCESS(status) || actual < 9)
        return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
    total = asix_le16(out + 2);
    if (total > size)
        total = size;
    status = control(dev, 0x80, 0x06, 0x0200, 0, out, (u16)total, &actual);
    *length = actual;
    return status;
}

u32 asix_set_config(USB_DEV *dev, u8 value)
{
    return control(dev, 0x00, 0x09, value, 0, 0, 0, 0);
}
