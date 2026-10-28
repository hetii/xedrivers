/* The RTL8822BU's registers and bulk endpoints over USB, as rtw88's usb.c.
 *
 * A register is vendor request 0x05 with its address in wValue: 0x40 writes 1, 2 or
 * 4 bytes, 0xc0 reads them, little-endian as the chip keeps them. Every access to
 * the always-powered sections (0x00-0xff, 0x1000-0x10ff) is followed by a write of
 * the same first byte to 0x4e0 (rtw_usb_reg_sec): rtw88 does it on the 8822B,
 * 8822C and 8821C, and the recording of it bringing this card up has every one.
 * Worker thread only: each access is a control transfer waited for.
 */

#include "rtw.h"
#include "bench.h"

#define VENDOR_REQUEST  0x05
#define REG_SEC         0x4E0
#define CONTROL_MS      500
#define BULK_MS         1000

static u32 transfer(RTW *r, u32 in, u32 address, u32 length)
{
    u8 setup[8];
    u32 status, actual = 0;

    setup[0] = in ? 0xC0 : 0x40;
    setup[1] = VENDOR_REQUEST;
    setup[2] = (u8)address;
    setup[3] = (u8)(address >> 8);
    setup[4] = 0;
    setup[5] = 0;
    setup[6] = (u8)length;
    setup[7] = 0;
    status = usb_control(r->dev, setup, r->reg_buf, length, &actual, CONTROL_MS);
    if (NT_SUCCESS(status) && actual != length)
        status = STATUS_UNSUCCESSFUL;
    if (!NT_SUCCESS(status)) {
        r->st->reg_errors++;
        r->st->reg_last_error = status;
        r->st->reg_last_address = address;
    }
    return status;
}

/* rtw_usb_reg_sec: the first byte of what was read or written goes to 0x4e0. */
static void reg_sec(RTW *r, u32 address)
{
    if (address > 0xFF && (address < 0x1000 || address > 0x10FF))
        return;
    transfer(r, 0, REG_SEC, 1);
}

static u32 read_n(RTW *r, u32 address, u32 length)
{
    u32 value;

    r->reg_buf[0] = r->reg_buf[1] = r->reg_buf[2] = r->reg_buf[3] = 0;
    r->st->reg_reads++;
    transfer(r, 1, address, length);
    value = (u32)r->reg_buf[0] | (u32)r->reg_buf[1] << 8 | (u32)r->reg_buf[2] << 16 |
            (u32)r->reg_buf[3] << 24;
    reg_sec(r, address);
    return value;
}

static void write_n(RTW *r, u32 address, u32 value, u32 length)
{
    r->reg_buf[0] = (u8)value;
    r->reg_buf[1] = (u8)(value >> 8);
    r->reg_buf[2] = (u8)(value >> 16);
    r->reg_buf[3] = (u8)(value >> 24);
    r->st->reg_writes++;
    transfer(r, 0, address, length);
    reg_sec(r, address);
}

u8  rtw_read8(RTW *r, u32 address)  { return (u8)read_n(r, address, 1); }
u16 rtw_read16(RTW *r, u32 address) { return (u16)read_n(r, address, 2); }
u32 rtw_read32(RTW *r, u32 address) { return read_n(r, address, 4); }

void rtw_write8(RTW *r, u32 address, u8 value)   { write_n(r, address, value, 1); }
void rtw_write16(RTW *r, u32 address, u16 value) { write_n(r, address, value, 2); }
void rtw_write32(RTW *r, u32 address, u32 value) { write_n(r, address, value, 4); }

void rtw_write8_set(RTW *r, u32 address, u8 bits)
{
    rtw_write8(r, address, rtw_read8(r, address) | bits);
}

void rtw_write8_clr(RTW *r, u32 address, u8 bits)
{
    rtw_write8(r, address, rtw_read8(r, address) & (u8)~bits);
}

void rtw_write16_set(RTW *r, u32 address, u16 bits)
{
    rtw_write16(r, address, rtw_read16(r, address) | bits);
}

void rtw_write16_clr(RTW *r, u32 address, u16 bits)
{
    rtw_write16(r, address, rtw_read16(r, address) & (u16)~bits);
}

void rtw_write32_set(RTW *r, u32 address, u32 bits)
{
    rtw_write32(r, address, rtw_read32(r, address) | bits);
}

void rtw_write32_clr(RTW *r, u32 address, u32 bits)
{
    rtw_write32(r, address, rtw_read32(r, address) & ~bits);
}

static u32 shift_of(u32 mask)
{
    u32 s = 0;

    while (mask && !(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

/* rtw_write32_mask: `value` into the field `mask` covers, the rest kept. */
void rtw_write32_mask(RTW *r, u32 address, u32 mask, u32 value)
{
    u32 old = rtw_read32(r, address);

    rtw_write32(r, address, (old & ~mask) | ((value << shift_of(mask)) & mask));
}

void rtw_write8_mask(RTW *r, u32 address, u32 mask, u8 value)
{
    u8 old = rtw_read8(r, address);

    mask &= 0xFF;
    rtw_write8(r, address, (u8)((old & ~mask) | ((value << shift_of(mask)) & mask)));
}

u32 rtw_read32_mask(RTW *r, u32 address, u32 mask)
{
    return (rtw_read32(r, address) & mask) >> shift_of(mask);
}

/* check_hw_ready. 1000 reads 10 us apart there; a read here is a transfer of
 * its own and takes longer than that, so the reads alone are the wait. */
u32 rtw_check_hw_ready(RTW *r, u32 address, u32 mask, u32 target)
{
    u32 i;

    for (i = 0; i < 1000; i++) {
        if (rtw_read32_mask(r, address, mask) == target)
            return 1;
        if (!r->dev->active)
            break;
    }
    r->st->poll_timeouts++;
    r->st->last_poll_address = address;
    return 0;
}

/* --- bulk ----------------------------------------------------------------------- */

static void bulk_done(URB *urb, u32 status)
{
    RTW *r = (RTW *)urb->Owner;

    r->tx_status = status;
    urb->Busy = 0;
    mem_sync();
    r->tx_done = 1;
    usb_wake();
}

u32 rtw_bulk_out(RTW *r, u32 ep, const u8 *data, u32 length)
{
    u32 waited, i;

    if (ep >= r->out_count || !r->ep_out[ep] || length > RTW_FW_MAX)
        return STATUS_UNSUCCESSFUL;
    /* as usb_control: a transfer that timed out is not prepared again, nor its
     * buffer written, until the controller has given it back */
    for (waited = 0; r->tx_urb.Busy; waited++) {
        if (waited >= BULK_MS || !r->dev->active) {
            r->st->bulk_late++;
            bench_trace("[bulk late]");
            return STATUS_UNSUCCESSFUL;
        }
        usb_wait(1);
    }
    if (data != r->bulk_buf)
        for (i = 0; i < length; i++)
            r->bulk_buf[i] = data[i];
    r->tx_done = 0;
    mem_sync();
    usb_prepare(&r->tx_urb, r->dev, r->ep_out[ep], bulk_done, r->bulk_buf, length, 0);
    r->tx_urb.Owner = r;
    usb_submit(&r->tx_urb);
    for (waited = 0; !r->tx_done; waited++) {
        if (waited >= BULK_MS || !r->dev->active) {
            bench_trace("[bulk timeout]");
            r->st->bulk_errors++;
            r->st->bulk_last_error = STATUS_UNSUCCESSFUL;
            return STATUS_UNSUCCESSFUL;
        }
        usb_wait(1);
    }
    r->st->bulk_out++;
    if (!NT_SUCCESS(r->tx_status)) {
        r->st->bulk_errors++;
        r->st->bulk_last_error = r->tx_status;
    }
    return r->tx_status;
}

/* rtw_usb_parse: the one bulk IN, the interrupt IN and the bulk OUTs in their
 * order, which is the order of priority (high, normal, low). The 8822BU has
 * 0x84 in, 0x87 interrupt, 0x05, 0x06 and 0x08 out. */
u32 rtw_usb_open(RTW *r)
{
    static const u8 outs[] = { 0x05, 0x06, 0x08 };
    u32 i;

    r->out_count = 0;
    for (i = 0; i < sizeof(outs); i++) {
        r->ep_out[i] = usb_open(r->dev, USB_TYPE_BULK, outs[i], 512, 0);
        if (!r->ep_out[i])
            return RTW_STEP_ENDPOINTS;
        r->st->out_ep[i] = outs[i];
        r->out_count++;
    }
    r->ep_in = usb_open(r->dev, USB_TYPE_BULK, 0x84, 512, 0);
    r->ep_int = usb_open(r->dev, USB_TYPE_INTERRUPT, 0x87, 64, 3);
    if (!r->ep_in)
        return RTW_STEP_ENDPOINTS;
    r->st->in_ep = 0x84;
    r->st->int_ep = r->ep_int ? 0x87 : 0;
    return 0;
}
