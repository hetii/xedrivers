/* The ASIX AX88178 (0b95:1780 and the adapters built on it): USB 2.0 gigabit
 * Ethernet, the older ASIX protocol -- one vendor request per operation -- with an
 * external PHY (Marvell 88E1111, Realtek RTL8211CL or another) behind software MII.
 *
 * From Linux asix_devices.c and asix_common.c (GPL-2.0): ax88178_bind,
 * ax88178_reset, ax88178_link_reset, marvell_phy_init, rtl8211cl_phy_init,
 * asix_rx_fixup_internal, asix_tx_fixup; checked against CUsbEnet's Ax88178.cpp.
 * Written without the card (2026-11-04): ax178_stats and the device's step say how
 * far a card got.
 *
 * Bring-up, from the worker, each control transfer waited for:
 *   the configuration descriptor, SET_CONFIGURATION
 *   the MAC address (READ_NODE_ID), the PHY's address (READ_PHY_ID), a software
 *     reset of the MAC (RESET 0, then PRL | IPPD)
 *   the EEPROM's word 0x17: which PHY (bits 6..0) and LED mode (bits 15..8); the
 *     PHY powered up through GPIO 1 the way that word asks, its id read back
 *   the MAC's PHY interface selected, another software reset, receive stopped
 *   the PHY's own set-up (Marvell: RGMII delays; RTL8211CL: its page-5 write),
 *     a PHY reset with autonegotiation, 10/100/1000 and pause advertised
 *   the medium mode, the MAC address written back, receive started (promiscuous,
 *     as for the CDC driver: the console's address is not the card's)
 *   bulk IN, bulk OUT and interrupt IN opened, the receive ring queued
 * The link comes on the interrupt pipe (byte 2, bit 0) and is polled from the PHY
 * every second while down; the worker then resolves speed and duplex from the MII
 * registers (common/asix: asix_mii_resolve) and sets the medium mode for them.
 *
 * Data: receive transfers of up to 2048 bytes (MFB 2048, as Linux and CUsbEnet run
 * it), a stream of 4-byte-header frames that may continue from one transfer into
 * the next (ax88178_frames.c); transmit one frame a transfer behind its header.
 */

#include "asix.h"
#include "ax88178_frames.h"
#include "driver.h"
#include "net.h"
#include "usb.h"

/* Vendor requests (bRequest). */
#define AX_CMD_SET_SW_MII        0x06u
#define AX_CMD_READ_MII_REG      0x07u  /* wValue PHY id, wIndex register */
#define AX_CMD_WRITE_MII_REG     0x08u
#define AX_CMD_STATMNGSTS_REG    0x09u
#define AX_CMD_SET_HW_MII        0x0Au
#define AX_CMD_READ_EEPROM       0x0Bu  /* wValue word address */
#define AX_CMD_WRITE_ENABLE      0x0Du
#define AX_CMD_WRITE_DISABLE     0x0Eu
#define AX_CMD_WRITE_RX_CTL      0x10u  /* wValue the mode */
#define AX_CMD_READ_NODE_ID      0x13u
#define AX_CMD_WRITE_NODE_ID     0x14u
#define AX_CMD_READ_PHY_ID       0x19u
#define AX_CMD_WRITE_MEDIUM_MODE 0x1Bu  /* wValue the mode */
#define AX_CMD_READ_GPIOS        0x1Eu
#define AX_CMD_WRITE_GPIOS       0x1Fu  /* wValue the pins */
#define AX_CMD_SW_RESET          0x20u  /* wValue the flags */
#define AX_CMD_SW_PHY_SELECT     0x22u

#define AX_HOST_EN               0x01u
#define AX_HOST_EN_TRIES         30

#define AX_SWRESET_PRL           0x08u
#define AX_SWRESET_IPPD          0x40u

#define AX_GPIO_GPO1EN           0x04u
#define AX_GPIO_GPO_1            0x08u
#define AX_GPIO_RSE              0x80u

#define AX_RX_CTL_PRO            0x0001u
#define AX_RX_CTL_AMALL          0x0002u
#define AX_RX_CTL_AB             0x0008u
#define AX_RX_CTL_SO             0x0080u
#define AX_RX_CTL_MFB_2048       0x0000u

#define AX_MEDIUM_GM             0x0001u
#define AX_MEDIUM_FD             0x0002u
#define AX_MEDIUM_AC             0x0004u
#define AX_MEDIUM_ENCK           0x0008u
#define AX_MEDIUM_RFC            0x0010u
#define AX_MEDIUM_TFC            0x0020u
#define AX_MEDIUM_JFE            0x0040u
#define AX_MEDIUM_RE             0x0100u
#define AX_MEDIUM_PS             0x0200u
#define AX88178_MEDIUM_DEFAULT   (AX_MEDIUM_PS | AX_MEDIUM_FD | AX_MEDIUM_AC | \
                                  AX_MEDIUM_RFC | AX_MEDIUM_TFC | AX_MEDIUM_JFE | \
                                  AX_MEDIUM_RE)

#define PHY_MODE_MARVELL         0x00u
#define PHY_MODE_RTL8211CL       0x0Cu
#define MII_MARVELL_CTRL         0x14u
#define MII_MARVELL_LED_CTRL     0x18u
#define MII_MARVELL_STATUS       0x1Bu
#define MARVELL_CTRL_TXDELAY     0x0002u
#define MARVELL_CTRL_RXDELAY     0x0080u
#define MII_PHYSID1              0x02u
#define MII_PHYSID2              0x03u

#define ASIX_VENDOR              0x0B95u

#define RX_COUNT    16
#define RX_SIZE     2048            /* MFB 2048 */
#define TX_COUNT    32
#define TX_SIZE     2048
#define INTR_SIZE   16
#define DESC_SIZE   512
#define DMA_BYTES   (RX_COUNT * RX_SIZE + TX_COUNT * TX_SIZE)

#define RX_ERROR_LIMIT   64
#define INTR_ERROR_LIMIT 16
#define POLL_TICKS       30         /* ~1 s: the PHY polled while the link is down */
#define AX178_MAX        2
#define NONE             0xFFFFFFFFu

enum {
    AX178_STEP_OK = 0,
    AX178_STEP_INSTANCE,
    AX178_STEP_DMA,
    AX178_STEP_CONFIG_DESC,
    AX178_STEP_LAYOUT,
    AX178_STEP_SET_CONFIG,
    AX178_STEP_MAC,
    AX178_STEP_PHY_ADDRESS,
    AX178_STEP_RESET,
    AX178_STEP_EEPROM,
    AX178_STEP_GPIO,
    AX178_STEP_PHY_SELECT,
    AX178_STEP_PHY,
    AX178_STEP_MEDIUM,
    AX178_STEP_NODE_ID,
    AX178_STEP_RX_CTL,
    AX178_STEP_ENDPOINTS,
};

/* Read by tools/xectl.py; field order is its STRUCTS["ax178_stats"]. */
typedef struct {
    u32 product;
    u32 eeprom_17;          /* PHY mode (bits 6..0) and LED mode (15..8); 0xFFFF none */
    u32 phy_address;
    u32 phy_id;             /* PHYSID1 << 16 | PHYSID2, 0 if it never answered */
    u32 bmsr;               /* at the last link check */
    u32 lpa;
    u32 stat1000;
    u32 medium;             /* as last written */
    u32 link_checks;
    u32 rx_transfers;
    u32 rx_resyncs;         /* headers that did not check */
    u32 intr_events;
    u32 intr_errors;
    u32 tx_transfers;
    u32 tx_too_big;
    u32 mii_errors;         /* MII accesses that failed or found no host access */
} AX178_STATS;

volatile AX178_STATS ax178_stats[AX178_MAX];

typedef struct AX178 {
    u32 in_use;
    NET_DEV *nd;
    USB_DEV *dev;
    volatile AX178_STATS *st;
    ASIX_LAYOUT layout;
    void *ep_in, *ep_out, *ep_intr;
    URB rx_urb[RX_COUNT];
    u8 *rx_buf[RX_COUNT];
    u32 rx_consecutive_errors;
    AX178_RX rx;
    URB tx_urb[TX_COUNT];
    u8 *tx_buf[TX_COUNT];
    u32 tx_next;
    u32 tx_inflight;
    KSPIN_LOCK tx_lock;
    URB intr_urb;
    u8 *intr_buf;
    u32 intr_consecutive_errors;
    u8 *desc_buf;
    u32 phy;                    /* the PHY's MII address */
    volatile u32 link_check;
    u32 link_up;
    u32 poll_ticks;
    volatile u32 running;
} AX178;

static AX178 ax178_inst[AX178_MAX];

static u32 fail(AX178 *a, u32 step, u32 status)
{
    a->nd->st->step = step;
    a->nd->st->step_status = status;
    return step;
}

/* --- requests and the MII ----------------------------------------------------- */

static u32 cmd(AX178 *a, u32 request, u32 value)
{
    return asix_write(a->dev, (u8)request, (u16)value, 0, 0, 0);
}

/* asix_check_host_enable: software MII on, until the MAC says the host has it. */
static u32 host_enable(AX178 *a)
{
    u32 i, status = STATUS_UNSUCCESSFUL;
    u8 smsr;

    for (i = 0; i < AX_HOST_EN_TRIES; i++) {
        status = cmd(a, AX_CMD_SET_SW_MII, 0);
        if (!NT_SUCCESS(status))
            continue;
        usb_sleep(1);
        smsr = 0;
        status = asix_read(a->dev, AX_CMD_STATMNGSTS_REG, 0, 0, &smsr, 1);
        if (NT_SUCCESS(status) && (smsr & AX_HOST_EN))
            return STATUS_SUCCESS;
    }
    return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;
}

static u32 mii_read(AX178 *a, u32 reg, u32 *value)
{
    u8 b[2] = { 0, 0 };
    u32 status = host_enable(a);

    if (NT_SUCCESS(status))
        status = asix_read(a->dev, AX_CMD_READ_MII_REG, (u16)a->phy, (u16)reg, b, 2);
    cmd(a, AX_CMD_SET_HW_MII, 0);
    if (!NT_SUCCESS(status))
        a->st->mii_errors++;
    *value = asix_le16(b);
    return status;
}

static u32 mii_write(AX178 *a, u32 reg, u32 value)
{
    u8 b[2];
    u32 status = host_enable(a);

    asix_put16(b, value);
    if (NT_SUCCESS(status))
        status = asix_write(a->dev, AX_CMD_WRITE_MII_REG, (u16)a->phy, (u16)reg, b, 2);
    cmd(a, AX_CMD_SET_HW_MII, 0);
    if (!NT_SUCCESS(status))
        a->st->mii_errors++;
    return status;
}

static u32 gpio(AX178 *a, u32 pins, u32 ms)
{
    u32 status = cmd(a, AX_CMD_WRITE_GPIOS, pins);

    if (ms)
        usb_sleep(ms);
    return status;
}

/* --- receive ------------------------------------------------------------------ */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(URB *urb)
{
    AX178 *a = (AX178 *)urb->Owner;

    usb_prepare(urb, a->dev, a->ep_in, rx_complete, a->rx_buf[urb->Tag], RX_SIZE, 0);
    usb_submit(urb);
}

static void deliver(const u8 *frame, u32 length, void *context)
{
    AX178 *a = (AX178 *)context;

    net_frame_in(a->nd, frame, length);
}

/* The stream runs across transfers, so they are taken in the order they complete,
 * which is the order they were queued on the one pipe. */
static void rx_complete(URB *urb, u32 status)
{
    AX178 *a = (AX178 *)urb->Owner;

    if (!a->running || !a->dev->active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        a->rx_consecutive_errors = 0;
        a->st->rx_transfers++;
        if (urb->Actual)
            ax178_rx_feed(&a->rx, a->rx_buf[urb->Tag], urb->Actual, deliver, a);
        a->st->rx_resyncs = a->rx.resyncs;
    } else {
        ax178_rx_reset(&a->rx);             /* whatever was split is lost */
        a->nd->st->rx_errors++;
        a->nd->st->rx_last_error = status;
        if (++a->rx_consecutive_errors > RX_ERROR_LIMIT) {
            a->nd->st->rx_stopped++;
            urb->Busy = 0;
            return;
        }
    }
    rx_queue(urb);
}

/* --- transmit ----------------------------------------------------------------- */

static void tx_complete(URB *urb, u32 status)
{
    AX178 *a = (AX178 *)urb->Owner;
    u32 irql, length = urb->Length;

    irql = KfAcquireSpinLock(&a->tx_lock);
    if (NT_SUCCESS(status)) {
        a->nd->st->tx_frames++;
        net_add64(&a->nd->st->tx_bytes, &a->nd->st->tx_bytes_hi, length);
    } else {
        a->nd->st->tx_errors++;
        a->nd->st->tx_last_error = status;
    }
    a->tx_inflight--;
    a->nd->st->tx_inflight = a->tx_inflight;
    urb->Busy = 0;
    KfReleaseSpinLock(&a->tx_lock, irql);
    net_tx_done(a->nd);
}

static u32 ax178_tx_space(NET_DEV *nd)
{
    AX178 *a = (AX178 *)nd->priv;

    return a && a->running && a->link_up && !a->tx_urb[a->tx_next].Busy;
}

static u32 ax178_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    AX178 *a = (AX178 *)nd->priv;
    u32 irql, slot = NONE, n;

    if (!a || !a->running) {
        nd->st->tx_drops++;
        return 0;
    }
    irql = KfAcquireSpinLock(&a->tx_lock);
    if (!a->tx_urb[a->tx_next].Busy) {
        slot = a->tx_next;
        a->tx_urb[slot].Busy = 1;
        a->tx_next = (slot + 1) % TX_COUNT;
        a->tx_inflight++;
        a->nd->st->tx_inflight = a->tx_inflight;
    }
    KfReleaseSpinLock(&a->tx_lock, irql);
    if (slot == NONE) {
        nd->st->tx_drops++;
        return 0;
    }
    n = ax178_tx_build(a->tx_buf[slot], TX_SIZE, frame, length, a->layout.out_max);
    if (!n) {
        a->st->tx_too_big++;
        nd->st->tx_drops++;
        irql = KfAcquireSpinLock(&a->tx_lock);
        a->tx_urb[slot].Busy = 0;
        a->tx_inflight--;
        a->nd->st->tx_inflight = a->tx_inflight;
        KfReleaseSpinLock(&a->tx_lock, irql);
        return 0;
    }
    a->st->tx_transfers++;
    usb_prepare(&a->tx_urb[slot], a->dev, a->ep_out, tx_complete, a->tx_buf[slot], n, 0);
    usb_submit(&a->tx_urb[slot]);
    return 1;
}

/* --- the interrupt pipe ------------------------------------------------------- */

static void intr_complete(URB *urb, u32 status);

static void intr_queue(AX178 *a)
{
    usb_prepare(&a->intr_urb, a->dev, a->ep_intr, intr_complete, a->intr_buf,
                a->layout.intr_max < INTR_SIZE ? a->layout.intr_max : INTR_SIZE, 0);
    usb_submit(&a->intr_urb);
}

/* asix_status: an 8-byte status, the link in byte 2's bit 0. */
static void intr_complete(URB *urb, u32 status)
{
    AX178 *a = (AX178 *)urb->Owner;

    if (!a->running || !a->dev->active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        a->intr_consecutive_errors = 0;
        a->st->intr_events++;
        if (urb->Actual >= 8 && ((a->intr_buf[2] & 0x01) != 0) != (a->link_up != 0)) {
            a->link_check = 1;
            usb_wake();
        }
    } else {
        a->st->intr_errors++;
        if (++a->intr_consecutive_errors > INTR_ERROR_LIMIT) {
            urb->Busy = 0;
            return;
        }
    }
    intr_queue(a);
}

/* --- the link ----------------------------------------------------------------- */

/* ax88178_link_reset: speed and duplex from the PHY, the medium mode for them. The
 * status register is read twice, its link bit latching low. Thread. */
static void link_check(AX178 *a)
{
    u32 bmsr = 0, adv = 0, lpa = 0, c1000 = 0, s1000 = 0, mbps, full, mode;

    a->st->link_checks++;
    mii_read(a, MII_BMSR, &bmsr);
    if (!NT_SUCCESS(mii_read(a, MII_BMSR, &bmsr)))
        return;
    mii_read(a, MII_ADVERTISE, &adv);
    mii_read(a, MII_LPA, &lpa);
    mii_read(a, MII_CTRL1000, &c1000);
    mii_read(a, MII_STAT1000, &s1000);
    a->st->bmsr = bmsr;
    a->st->lpa = lpa;
    a->st->stat1000 = s1000;
    mbps = asix_mii_resolve(bmsr, adv, lpa, c1000, s1000, &full);
    if (!mbps) {
        if (a->link_up) {
            a->link_up = 0;
            net_link_report(a->nd, 0);
        }
        return;
    }
    mode = AX88178_MEDIUM_DEFAULT;
    if (mbps == 1000)
        mode |= AX_MEDIUM_GM;
    else if (mbps != 100)
        mode &= ~(AX_MEDIUM_PS | AX_MEDIUM_GM);
    mode |= AX_MEDIUM_ENCK;
    if (full)
        mode |= AX_MEDIUM_FD;
    else
        mode &= ~AX_MEDIUM_FD;
    cmd(a, AX_CMD_WRITE_MEDIUM_MODE, mode);
    a->st->medium = mode;
    net_speed_report(a->nd, mbps);
    if (!a->link_up) {
        a->link_up = 1;
        net_link_report(a->nd, 1);
    }
}

static void ax178_tick(NET_DEV *nd)
{
    AX178 *a = (AX178 *)nd->priv;

    if (!a || !a->running)
        return;
    if (a->link_check || (!a->link_up && ++a->poll_ticks >= POLL_TICKS)) {
        a->link_check = 0;
        a->poll_ticks = 0;
        link_check(a);
    }
}

/* --- bring-up ----------------------------------------------------------------- */

static u32 alloc_buffers(AX178 *a)
{
    u32 i;

    if (!a->desc_buf) {
        a->desc_buf = (u8 *)net_dma_scratch(DESC_SIZE);
        a->intr_buf = (u8 *)net_dma_scratch(INTR_SIZE);
    }
    if (!a->desc_buf || !a->intr_buf || !net_dma_begin(a->nd, DMA_BYTES))
        return 0;
    for (i = 0; i < RX_COUNT; i++) {
        a->rx_buf[i] = (u8 *)net_dma_alloc(a->nd, RX_SIZE);
        a->rx_urb[i].Tag = i;
        a->rx_urb[i].Owner = a;
    }
    for (i = 0; i < TX_COUNT; i++) {
        a->tx_buf[i] = (u8 *)net_dma_alloc(a->nd, TX_SIZE);
        a->tx_urb[i].Tag = i;
        a->tx_urb[i].Owner = a;
        a->tx_urb[i].Busy = 0;
    }
    return a->rx_buf[RX_COUNT - 1] && a->tx_buf[TX_COUNT - 1];
}

static AX178 *take_instance(NET_DEV *nd)
{
    u32 i;

    for (i = 0; i < AX178_MAX; i++) {
        AX178 *a = &ax178_inst[i];

        if (a->in_use && a->nd != nd)
            continue;
        a->in_use = 1;
        a->nd = nd;
        a->dev = nd->usb;
        a->st = &ax178_stats[i];
        a->intr_urb.Owner = a;
        nd->priv = a;
        return a;
    }
    return 0;
}

static u32 sw_reset(AX178 *a)
{
    u32 status = cmd(a, AX_CMD_SW_RESET, 0);

    usb_sleep(150);
    if (NT_SUCCESS(status))
        status = cmd(a, AX_CMD_SW_RESET, AX_SWRESET_PRL | AX_SWRESET_IPPD);
    usb_sleep(150);
    return status;
}

/* marvell_phy_init and rtl8211cl_phy_init, then asix_phy_reset and the
 * advertisement, the medium mode and mii_nway_restart. */
static u32 phy_setup(AX178 *a, u32 phymode, u32 ledmode)
{
    u32 value = 0, status;

    if (phymode == PHY_MODE_MARVELL) {
        mii_read(a, MII_MARVELL_STATUS, &value);
        mii_write(a, MII_MARVELL_CTRL, MARVELL_CTRL_RXDELAY | MARVELL_CTRL_TXDELAY);
        if (ledmode && NT_SUCCESS(mii_read(a, MII_MARVELL_LED_CTRL, &value)))
            mii_write(a, MII_MARVELL_LED_CTRL, (value & 0xF8FFu) | 0x0101u);
        usb_sleep(60);
    } else if (phymode == PHY_MODE_RTL8211CL) {
        mii_write(a, 0x1F, 0x0005);
        mii_write(a, 0x0C, 0);
        if (NT_SUCCESS(mii_read(a, 0x01, &value)))
            mii_write(a, 0x01, value | 0x0080);
        mii_write(a, 0x1F, 0);
        if (ledmode == 12) {
            mii_write(a, 0x1F, 0x0002);
            mii_write(a, 0x1A, 0x00CB);
            mii_write(a, 0x1F, 0);
        }
    }
    status = mii_write(a, MII_BMCR, BMCR_RESET | BMCR_ANENABLE);
    if (NT_SUCCESS(status))
        status = mii_write(a, MII_ADVERTISE, ADVERTISE_10HALF | ADVERTISE_10FULL |
                                             ADVERTISE_100HALF | ADVERTISE_100FULL |
                                             ADVERTISE_CSMA | ADVERTISE_PAUSE);
    if (NT_SUCCESS(status))
        status = mii_write(a, MII_CTRL1000, ADVERTISE_1000FULL);
    return status;
}

static u32 ax178_start(NET_DEV *nd)
{
    AX178 *a = take_instance(nd);
    const u8 *device = UsbdGetDeviceDescriptor(nd->usb->device);
    u32 status, length = 0, value = 0, i, e17, phymode, ledmode;
    u8 mac[6], b[2];

    if (!a) {
        nd->st->step = AX178_STEP_INSTANCE;
        return AX178_STEP_INSTANCE;
    }
    a->running = 0;
    a->link_up = 0;
    a->link_check = 0;
    a->poll_ticks = POLL_TICKS;
    a->rx_consecutive_errors = a->intr_consecutive_errors = 0;
    a->ep_in = a->ep_out = a->ep_intr = 0;
    ax178_rx_reset(&a->rx);
    if (device)
        a->st->product = asix_le16(device + 10);
    if (!alloc_buffers(a))
        return fail(a, AX178_STEP_DMA, 0);

    status = asix_get_config(a->dev, a->desc_buf, DESC_SIZE, &length);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_CONFIG_DESC, status);
    if (!asix_parse_config(a->desc_buf, length, &a->layout))
        return fail(a, AX178_STEP_LAYOUT, length);
    status = asix_set_config(a->dev, a->layout.config_value);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_SET_CONFIG, status);

    /* ax88178_bind */
    status = asix_read(a->dev, AX_CMD_READ_NODE_ID, 0, 0, mac, 6);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_MAC, status);
    for (i = 0; i < 6; i++)
        nd->mac[i] = mac[i];
    status = asix_read(a->dev, AX_CMD_READ_PHY_ID, 0, 0, b, 2);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_PHY_ADDRESS, status);
    a->phy = b[1] & 0x1Fu;                  /* asix_read_phy_addr(dev, true) */
    a->st->phy_address = a->phy;
    status = sw_reset(a);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_RESET, status);

    /* ax88178_reset */
    asix_read(a->dev, AX_CMD_READ_GPIOS, 0, 0, b, 1);
    cmd(a, AX_CMD_WRITE_ENABLE, 0);
    status = asix_read(a->dev, AX_CMD_READ_EEPROM, 0x0017, 0, b, 2);
    cmd(a, AX_CMD_WRITE_DISABLE, 0);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_EEPROM, status);
    e17 = asix_le16(b);
    a->st->eeprom_17 = e17;
    if (e17 == 0xFFFF) {
        phymode = PHY_MODE_MARVELL;
        ledmode = 0;
    } else {
        phymode = e17 & 0x7Fu;
        ledmode = e17 >> 8;
    }
    status = gpio(a, AX_GPIO_RSE | AX_GPIO_GPO_1 | AX_GPIO_GPO1EN, 40);
    if (NT_SUCCESS(status)) {
        if ((e17 >> 8) != 1) {
            gpio(a, 0x003C, 30);
            gpio(a, 0x001C, 300);
            status = gpio(a, 0x003C, 30);
        } else {
            gpio(a, AX_GPIO_GPO1EN, 30);
            status = gpio(a, AX_GPIO_GPO1EN | AX_GPIO_GPO_1, 30);
        }
    }
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_GPIO, status);

    /* asix_get_phyid: the PHY answers once it is powered */
    for (i = 0; i < 100; i++) {
        if (NT_SUCCESS(mii_read(a, MII_PHYSID1, &value)) && value && value != 0xFFFF)
            break;
        usb_sleep(1);
    }
    if (value && value != 0xFFFF) {
        a->st->phy_id = value << 16;
        if (NT_SUCCESS(mii_read(a, MII_PHYSID2, &value)))
            a->st->phy_id |= value;
    }

    status = cmd(a, AX_CMD_SW_PHY_SELECT, 0);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_PHY_SELECT, status);
    status = sw_reset(a);
    if (NT_SUCCESS(status))
        status = cmd(a, AX_CMD_WRITE_RX_CTL, 0);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_RESET, status);

    status = phy_setup(a, phymode, ledmode);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_PHY, status);
    status = cmd(a, AX_CMD_WRITE_MEDIUM_MODE, AX88178_MEDIUM_DEFAULT);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_MEDIUM, status);
    a->st->medium = AX88178_MEDIUM_DEFAULT;
    if (NT_SUCCESS(mii_read(a, MII_BMCR, &value)) && (value & BMCR_ANENABLE))
        mii_write(a, MII_BMCR, value | BMCR_ANRESTART);     /* mii_nway_restart */

    status = asix_write(a->dev, AX_CMD_WRITE_NODE_ID, 0, 0, mac, 6);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_NODE_ID, status);
    status = cmd(a, AX_CMD_WRITE_RX_CTL, AX_RX_CTL_SO | AX_RX_CTL_AB | AX_RX_CTL_AMALL |
                                         AX_RX_CTL_PRO | AX_RX_CTL_MFB_2048);
    if (!NT_SUCCESS(status))
        return fail(a, AX178_STEP_RX_CTL, status);

    a->ep_in = usb_open(a->dev, USB_TYPE_BULK, a->layout.ep_in, a->layout.in_max, 0);
    a->ep_out = usb_open(a->dev, USB_TYPE_BULK, a->layout.ep_out, a->layout.out_max, 0);
    if (a->layout.ep_intr)
        a->ep_intr = usb_open(a->dev, USB_TYPE_INTERRUPT, a->layout.ep_intr,
                              a->layout.intr_max, a->layout.intr_interval);
    if (!a->ep_in || !a->ep_out)
        return fail(a, AX178_STEP_ENDPOINTS, usb_stats.last_open_status);

    a->tx_next = 0;
    a->tx_inflight = 0;
    a->running = 1;
    mem_sync();
    for (i = 0; i < RX_COUNT; i++)
        rx_queue(&a->rx_urb[i]);
    if (a->ep_intr)
        intr_queue(a);
    nd->st->step = AX178_STEP_OK;
    return 0;
}

static void ax178_detached(NET_DEV *nd)
{
    AX178 *a = (AX178 *)nd->priv;

    if (!a)
        return;
    a->running = 0;
    a->link_up = 0;
    a->in_use = 0;
    mem_sync();
}

/* The AX88178 adapters of Linux's asix_devices table. */
static const u16 adapters[][2] = {
    { ASIX_VENDOR, 0x1780 },                    /* ASIX AX88178 */
    { 0x08DD, 0x0114 },                         /* Billionton GUSB2AM-1G-B */
    { 0x0411, 0x006E },                         /* Buffalo LUA-U2-GT */
    { 0x0DF6, 0x0056 },                         /* Sitecom LN-031 */
    { 0x0DF6, 0x061C },                         /* Sitecom LN-028 */
    { 0x0789, 0x0160 },                         /* Logitec LAN-GTJ/U2A */
    { 0x1737, 0x0039 },                         /* Linksys USB1000 */
    { 0x04BB, 0x0930 },                         /* IO-DATA ETG-US2 */
    { 0x050D, 0x5055 },                         /* Belkin F5D5055 */
    { 0x14EA, 0xAB11 },                         /* ABOCOM */
};

static u32 ax178_match(const u8 *device, const u8 *interface)
{
    u32 vid = asix_le16(device + 8), pid = asix_le16(device + 10), i;

    (void)interface;
    for (i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++)
        if (adapters[i][0] == vid && adapters[i][1] == pid)
            return XE_MATCH_VENDOR;
    return XE_MATCH_NONE;
}

const XE_DRIVER xe_driver_ax88178 = {
    .name = "ax88178",
    .kind = XE_KIND_WIRED,
    .match = ax178_match,
    .start = ax178_start,
    .detached = ax178_detached,
    .xmit = ax178_xmit,
    .tx_space = ax178_tx_space,
    .tick = ax178_tick,
};
