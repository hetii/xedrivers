/* ASIX USB Ethernet: code shared by the drivers of ASIX's chips (ax88179, ax88178),
 * not a driver itself (src/drivers/common/).
 *
 * What the families have in common is little: vendor control requests to the
 * device (bmRequestType 0xC0 in, 0x40 out; each family its own request codes and
 * meaning of wValue / wIndex), one data interface with a bulk pipe each way and an
 * interrupt pipe for the link, and an MII PHY behind them with the standard
 * registers. Shapes from Linux asix_common.c and ax88179_178a.c (GPL-2.0).
 */

#ifndef XE_ASIX_H
#define XE_ASIX_H

#include "kernel.h"
#include "usb.h"

/* The standard MII registers and bits the drivers use. */
#define MII_BMCR            0x00u
#define MII_BMSR            0x01u
#define MII_ADVERTISE       0x04u
#define MII_LPA             0x05u
#define MII_CTRL1000        0x09u
#define MII_STAT1000        0x0Au
#define BMCR_ANENABLE       0x1000u
#define BMCR_ANRESTART      0x0200u
#define BMCR_RESET          0x8000u
#define BMSR_LSTATUS        0x0004u
#define ADVERTISE_CSMA      0x0001u
#define ADVERTISE_10HALF    0x0020u
#define ADVERTISE_10FULL    0x0040u
#define ADVERTISE_100HALF   0x0080u
#define ADVERTISE_100FULL   0x0100u
#define ADVERTISE_PAUSE     0x0400u
#define ADVERTISE_PAUSE_ASYM 0x0800u
#define ADVERTISE_1000FULL  0x0200u
#define ADVERTISE_1000HALF  0x0100u

/* The pipes of the device's first configuration, from its descriptor. */
typedef struct {
    u8  config_value;
    u8  interface;
    u8  ep_in;                  /* bulk IN address */
    u8  ep_out;                 /* bulk OUT address */
    u8  ep_intr;                /* interrupt IN address, 0 if none */
    u8  intr_interval;
    u16 in_max;
    u16 out_max;
    u16 intr_max;
} ASIX_LAYOUT;

/* Pure: the first interface with a bulk IN and a bulk OUT endpoint in the
 * configuration descriptor `d` (`length` bytes, the whole of it). 1 if found. */
u32 asix_parse_config(const u8 *d, u32 length, ASIX_LAYOUT *out);

/* A vendor request with up to ASIX_CONTROL_MAX bytes of data, read into or written
 * from `data`. Thread-only: waits for the transfer. STATUS_SUCCESS or the status. */
#define ASIX_CONTROL_MAX 64u
u32 asix_read(USB_DEV *dev, u8 request, u16 value, u16 index, u8 *data, u16 length);
u32 asix_write(USB_DEV *dev, u8 request, u16 value, u16 index, const u8 *data,
               u16 length);

/* The configuration descriptor, whole, into `out` (up to `size` bytes); its length
 * in *length. Thread-only. */
u32 asix_get_config(USB_DEV *dev, u8 *out, u32 size, u32 *length);

/* SET_CONFIGURATION. Thread-only. */
u32 asix_set_config(USB_DEV *dev, u8 value);

static inline u32 asix_le16(const u8 *p) { return (u32)p[0] | (u32)p[1] << 8; }
static inline u32 asix_le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}
static inline void asix_put16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void asix_put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}

#endif /* XE_ASIX_H */
