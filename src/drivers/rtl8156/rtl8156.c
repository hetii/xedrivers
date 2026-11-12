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
#include "file.h"

#define RTL_VENDOR  0x0BDAu
#define RTL_PRODUCT 0x8156u

/* Host commands this driver answers (tools/xectl.py advertise / reg / phy). */
#define CMD_ADVERTISE  7u   /* [1] RTL_ADV_* bits; renegotiates now */
#define CMD_REG_READ   8u   /* [1] mcu, [2] address -> [5] value, [7] status */
#define CMD_REG_WRITE  9u   /* [1] mcu, [2] address, [3] word */
#define CMD_PHY_READ  10u   /* [1] OCP address -> [5] value, [7] status */
#define CMD_PHY_WRITE 11u   /* [1] OCP address, [2] value */

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
    rs->advertise_status = bits ? rtl_set_advertise(nd->usb, bits, 1) : STATUS_SUCCESS;
    return rs->advertise_status;
}

/* xedrivers.ini, read at each bring-up (worker thread):
 *   rtl_pause = on | off   pause frames (802.3x) advertised by the PHY, both ways; unset:
 *                          the card's own (it advertises them). Off, a card whose
 *                          receive FIFO fills drops rather than stopping its link partner
 *                          -- a pause that reaches the PC through a switch stops all it
 *                          sends, to the other card as well.
 *   rtl_fifo_full = N, rtl_fifo_empty = N   the receive FIFO's pause on / pause off
 *                          thresholds (PLA_RX_FIFO_FULL 0xc0a6 and _EMPTY 0xc0aa), in
 *                          16-byte units; the card's own 0x200 / 0x300, Linux's 512 / 1024. */
static u32 setting(const char *key, u32 *value)
{
    char v[16];
    u32 n = 0, i = 0, base = 10;

    if (!file_setting("xedrivers.ini", key, v, sizeof(v)) || !v[0])
        return 0;
    if (v[0] == 'o') {                  /* on / off */
        *value = v[1] == 'n';
        return 1;
    }
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        base = 16;
        i = 2;
    }
    for (; v[i]; i++) {
        u32 d = v[i] >= '0' && v[i] <= '9' ? (u32)(v[i] - '0') :
                v[i] >= 'a' && v[i] <= 'f' ? (u32)(v[i] - 'a' + 10) :
                v[i] >= 'A' && v[i] <= 'F' ? (u32)(v[i] - 'A' + 10) : 99;
        if (d >= base)
            break;
        n = n * base + d;
    }
    *value = n;
    return 1;
}

static void flow_settings(NET_DEV *nd)
{
    volatile RTL_STATS *rs = &rtl_stats[nd->usb->index];
    u32 v, adv;

    if (setting("rtl_fifo_full", &v))
        rtl_write_word(nd->usb, RTL_PLA, 0xC0A6u, v);
    if (setting("rtl_fifo_empty", &v))
        rtl_write_word(nd->usb, RTL_PLA, 0xC0AAu, v);
    if (setting("rtl_pause", &v) && NT_SUCCESS(rtl_phy_read(nd->usb, RTL_OCP_MII_ADVERTISE, &adv))) {
        adv = v ? adv | 0x0C00u : adv & ~0x0C00u;      /* PAUSE, ASYM_PAUSE */
        if (NT_SUCCESS(rtl_phy_write(nd->usb, RTL_OCP_MII_ADVERTISE, adv)) && !xe_config.advertise)
            rtl_phy_write(nd->usb, RTL_OCP_MII_BMCR, 0x1200u);    /* ANENABLE | ANRESTART */
        rs->pause_set = 1 + v;
    }
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
    flow_settings(nd);                  /* before the advertisement's renegotiation */
    if (xe_config.advertise)
        rs->advertise_status = rtl_set_advertise(nd->usb, xe_config.advertise, 1);
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
