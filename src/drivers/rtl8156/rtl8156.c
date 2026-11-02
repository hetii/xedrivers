/* The Realtek RTL8156 / RTL8156B (0bda:8156): the CDC driver, plus what only a
 * Realtek answers.
 *
 * The card's first configuration is Realtek's own protocol, which would need the
 * whole of Linux r8152's initialisation; its CDC ECM (3) and NCM (2) configurations
 * are run by the card's firmware, so drivers/cdc does the frames. What this adds:
 *   - the match by VID:PID, since the kernel offers the card in its first
 *     configuration, where nothing says CDC;
 *   - 2.5G: in CDC mode the firmware advertises 10/100 only, and the PHY is
 *     reprogrammed through Realtek's vendor requests after every bring-up (rtl.c);
 *   - the chip version, and host commands for registers and the PHY.
 */

#include "rtl.h"
#include "cdc_net.h"
#include "net.h"
#include "config.h"

#define RTL_VENDOR  0x0BDAu
#define RTL_PRODUCT 0x8156u

/* Host commands this driver answers (tools/xectl.py advertise / reg / phy). */
#define CMD_ADVERTISE  7u   /* [1] RTL_ADV_* bits; renegotiates now */
#define CMD_REG_READ   8u   /* [1] mcu, [2] address -> [5] value, [7] status */
#define CMD_REG_WRITE  9u   /* [1] mcu, [2] address, [3] word */
#define CMD_PHY_READ  10u   /* [1] OCP address -> [5] value, [7] status */
#define CMD_PHY_WRITE 11u   /* [1] OCP address, [2] value */

volatile RTL_STATS rtl_stats[USB_MAX_DEVICES];

static u32 rtl8156_match(const u8 *device, const u8 *interface)
{
    (void)interface;
    if ((u32)(device[8] | device[9] << 8) == RTL_VENDOR &&
        (u32)(device[10] | device[11] << 8) == RTL_PRODUCT)
        return XE_MATCH_VENDOR;
    return XE_MATCH_NONE;
}

static u32 advertise(NET_DEV *nd, u32 bits)
{
    volatile RTL_STATS *rs = &rtl_stats[nd->usb->index];

    xe_config.advertise = bits;
    if (!nd->st->ready)
        return STATUS_UNSUCCESSFUL;
    rs->advertise_status = bits ? rtl_set_advertise(nd->usb, bits) : STATUS_SUCCESS;
    return rs->advertise_status;
}

static u32 rtl8156_start(NET_DEV *nd)
{
    volatile RTL_STATS *rs = &rtl_stats[nd->usb->index];
    u32 step, tcr0 = 0;

    rtl_forget_page(nd->usb);
    step = cdc_start(nd);
    if (step)
        return step;

    if (NT_SUCCESS(rtl_read_dword(nd->usb, RTL_PLA, RTL_PLA_TCR0, &tcr0)))
        rs->chip_version = tcr0 >> 16;
    if (xe_config.advertise)
        rs->advertise_status = rtl_set_advertise(nd->usb, xe_config.advertise);
    return 0;
}

static u32 rtl8156_command(NET_DEV *nd, u32 what, volatile u32 *args)
{
    u32 value = 0;

    switch (what) {
    case CMD_ADVERTISE:
        args[5] = advertise(nd, args[1]);
        return 1;
    case CMD_REG_READ:
        args[7] = rtl_read_word(nd->usb, args[1], args[2], &value);
        args[5] = value;
        return 1;
    case CMD_REG_WRITE:
        args[5] = rtl_write_word(nd->usb, args[1], args[2], args[3]);
        return 1;
    case CMD_PHY_READ:
        args[7] = rtl_phy_read(nd->usb, args[1], &value);
        args[5] = value;
        return 1;
    case CMD_PHY_WRITE:
        args[5] = rtl_phy_write(nd->usb, args[1], args[2]);
        return 1;
    default:
        return 0;
    }
}

const XE_DRIVER xe_driver_rtl8156 = {
    .name = "rtl8156",
    .kind = XE_KIND_WIRED,
    .match = rtl8156_match,
    .start = rtl8156_start,
    .detached = cdc_detached,
    .xmit = cdc_xmit,
    .tx_space = cdc_tx_space,
    .command = rtl8156_command,
    .tick = cdc_tick,
};
