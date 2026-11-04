/* The Realtek RTL8153 / RTL8153B / RTL8153C (0bda:8153 and the adapters built on
 * them): the CDC driver, plus what only a Realtek answers -- as rtl8156, a
 * different chip with a gigabit PHY.
 *
 * Linux runs these in Realtek's own configuration (r8152, r8153_init and
 * r8153b_init); their CDC ECM configuration is run by the card's firmware instead,
 * and Linux's cdc_ether drives it for 0bda:8153 when r8152 is not there. So
 * drivers/cdc does the frames, as for the RTL8156B. What this adds:
 *   - the match by VID:PID, the kernel offering the card in its first
 *     configuration, where nothing says CDC;
 *   - the chip version (PLA_TCR0 >> 16 & 0x7CF0: 0x5C00-0x5C30 RTL8153, 0x6000 /
 *     0x6010 RTL8153B, 0x6400 RTL8153C);
 *   - the advertisement through Realtek's PHY window (drivers/common/realtek), gigabit at
 *     most: the 2.5G register of the RTL8156 does not exist here;
 *   - the host commands for registers and the PHY, numbered as rtl8156's.
 *
 * Written without the card (2026-11-04), from r8152 and cdc_ether: what it has
 * not met is the PHY's advertisement in CDC mode (the RTL8156B's firmware
 * advertised 10/100 only there) and whether a given adapter has an ECM
 * configuration at all; cdc_stats' step and rtl_stats say how far it got.
 */

#include "rtl.h"
#include "cdc_net.h"
#include "net.h"
#include "config.h"

#define VERSION_MASK   0x7CF0u

/* Host commands this driver answers (tools/xectl.py advertise / reg / phy). */
#define CMD_ADVERTISE  7u   /* [1] RTL_ADV_* bits; renegotiates now */
#define CMD_REG_READ   8u   /* [1] mcu, [2] address -> [5] value, [7] status */
#define CMD_REG_WRITE  9u   /* [1] mcu, [2] address, [3] word */
#define CMD_PHY_READ  10u   /* [1] OCP address -> [5] value, [7] status */
#define CMD_PHY_WRITE 11u   /* [1] OCP address, [2] value */

/* Adapters built on the RTL8153 family, from r8152's rtl8152_table without
 * Realtek's own ids of other chips (0x8050, 0x8053, 0x8152, 0x8155, 0x8156). An id
 * here whose chip turns out to be something else still runs: the version read at
 * start decides whether there is a 2.5G register. */
static const u16 adapters[][2] = {
    { 0x0BDA, 0x8153 },                         /* Realtek */
    { 0x045E, 0x07AB }, { 0x045E, 0x07C6 },     /* Microsoft */
    { 0x045E, 0x0927 }, { 0x045E, 0x0C5E },
    { 0x04E8, 0xA101 },                         /* Samsung */
    { 0x17EF, 0x304F }, { 0x17EF, 0x3054 },     /* Lenovo */
    { 0x17EF, 0x3062 }, { 0x17EF, 0x3069 },
    { 0x17EF, 0x3082 }, { 0x17EF, 0x3098 },
    { 0x17EF, 0x7205 }, { 0x17EF, 0x720C },
    { 0x17EF, 0x7214 }, { 0x17EF, 0x721E },
    { 0x17EF, 0xA387 },
    { 0x13B1, 0x0041 },                         /* Linksys */
    { 0x0955, 0x09FF },                         /* Nvidia */
    { 0x2357, 0x0601 },                         /* TP-Link */
    { 0x2001, 0xB301 },                         /* D-Link */
    { 0x0B05, 0x1976 },                         /* ASUS */
};

static u32 rtl8153_match(const u8 *device, const u8 *interface)
{
    u32 vid = (u32)(device[8] | device[9] << 8), pid = (u32)(device[10] | device[11] << 8);
    u32 i;

    (void)interface;
    for (i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++)
        if (adapters[i][0] == vid && adapters[i][1] == pid)
            return XE_MATCH_VENDOR;
    return XE_MATCH_NONE;
}

/* The RTL8156 family (0x70xx, 0x74xx) has the 2.5G register; every RTL8153 not. */
static u32 has_2500(volatile RTL_STATS *rs)
{
    u32 v = rs->chip_version & VERSION_MASK;

    return v >= 0x7000u && v < 0x7800u;
}

static u32 advertise(NET_DEV *nd, u32 bits)
{
    volatile RTL_STATS *rs = &rtl_stats[nd->usb->index];

    xe_config.advertise = bits;
    if (!nd->st->ready)
        return STATUS_UNSUCCESSFUL;
    rs->advertise_status = bits ? rtl_set_advertise(nd->usb, bits, has_2500(rs))
                                : STATUS_SUCCESS;
    return rs->advertise_status;
}

static u32 rtl8153_start(NET_DEV *nd)
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
        rs->advertise_status = rtl_set_advertise(nd->usb, xe_config.advertise,
                                                 has_2500(rs));
    return 0;
}

static u32 rtl8153_command(NET_DEV *nd, u32 what, volatile u32 *args)
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

const XE_DRIVER xe_driver_rtl8153 = {
    .name = "rtl8153",
    .kind = XE_KIND_WIRED,
    .match = rtl8153_match,
    .start = rtl8153_start,
    .detached = cdc_detached,
    .xmit = cdc_xmit,
    .tx_space = cdc_tx_space,
    .command = rtl8153_command,
    .tick = cdc_tick,
};
