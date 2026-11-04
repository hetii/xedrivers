/* The ASIX AX88179, AX88178A and AX88179A/B (0b95:1790, 0b95:178a, and the
 * adapters built on them): USB gigabit Ethernet in ASIX's own protocol.
 *
 * The AX88179 and AX88178A from Linux ax88179_178a.c (GPL-2.0) -- ax88179_reset,
 * ax88179_link_reset, ax88179_status -- checked against CUsbEnet's
 * Ax88179x_178A_772D.cpp. The AX88179A/B (0b95:1790 with bcdDevice 2.00), a
 * firmware-run generation with its own registers and framing, from CUsbEnet's
 * CAx179AFamily alone (start_a and link_check_a below; ax88179_frames.c). Written
 * without the cards (2026-11-04): ax179_stats and the device's step say how far a
 * card got.
 *
 * Bring-up, from the worker, each control transfer waited for:
 *   the configuration descriptor, SET_CONFIGURATION
 *   PHY power: PHYPWR_RSTCTL 0, then IPRL, 500 ms; CLK_SELECT ACS|BCS, 200 ms;
 *     auto-detach if the EEPROM (word 0x43, bit 8) asks for it
 *   the MAC address from NODE_ID
 *   RX_BULKIN_QCTRL for high speed, the pause water levels, no checksum offload
 *     (xnet's frames carry their own), RX_CTL start + promiscuous (the console's
 *     address is not the card's, as for the CDC driver), MEDIUM_STATUS_MODE gigabit
 *   EEE off through the PHY's MMD window, autonegotiation restarted
 *   bulk IN, bulk OUT and interrupt IN opened, the receive ring queued
 * The link comes on the interrupt pipe (an 8-byte status, bit 16 of its first word)
 * and is also polled from the PHY every second while down; either way the worker
 * then reads the PHY's speed and sets the medium mode and the bulk-in
 * aggregation for it (ax88179_link_reset), and reports link and speed.
 *
 * Data: a receive transfer holds up to some tens of frames (ax88179_frames.c); a
 * transmitted one is one frame behind an 8-byte header -- CUsbEnet found the
 * AX88179's multi-frame transmit format unreliable, and Linux sends one too.
 */

#include "asix.h"
#include "ax88179_frames.h"
#include "driver.h"
#include "net.h"
#include "usb.h"

/* Vendor requests (bRequest); wValue / wIndex as noted. */
#define AX_ACCESS_MAC        0x01u  /* wValue register, wIndex length */
#define AX_ACCESS_PHY        0x02u  /* wValue PHY id, wIndex register */
#define AX_ACCESS_EEPROM     0x04u  /* wValue word address, wIndex words */

#define AX_PHY_ID            0x03u

/* MAC registers (AX_ACCESS_MAC). */
#define AX_PHYSICAL_LINK_STATUS 0x02u
#define AX_USB_SS            0x04u
#define AX_USB_HS            0x02u
#define AX_RX_CTL            0x0Bu
#define AX_RX_CTL_DROPCRCERR 0x0100u
#define AX_RX_CTL_IPE        0x0200u
#define AX_RX_CTL_START      0x0080u
#define AX_RX_CTL_AP         0x0020u
#define AX_RX_CTL_AB         0x0008u
#define AX_RX_CTL_AMALL      0x0002u
#define AX_RX_CTL_PRO        0x0001u
#define AX_NODE_ID           0x10u
#define AX_MEDIUM_STATUS_MODE 0x22u
#define AX_MEDIUM_GIGAMODE   0x0001u
#define AX_MEDIUM_FULL_DUPLEX 0x0002u
#define AX_MEDIUM_EN_125MHZ  0x0008u
#define AX_MEDIUM_RXFLOW     0x0010u
#define AX_MEDIUM_TXFLOW     0x0020u
#define AX_MEDIUM_RECEIVE_EN 0x0100u
#define AX_MEDIUM_PS         0x0200u
#define AX_PHYPWR_RSTCTL     0x26u
#define AX_PHYPWR_IPRL       0x0020u
#define AX_PHYPWR_AT         0x1000u
#define AX_RX_BULKIN_QCTRL   0x2Eu
#define AX_CLK_SELECT        0x33u
#define AX_CLK_BCS           0x01u
#define AX_CLK_ACS           0x02u
#define AX_CLK_ULR           0x08u
#define AX_RXCOE_CTL         0x34u
#define AX_TXCOE_CTL         0x35u
#define AX_PAUSE_LOW         0x55u
#define AX_PAUSE_HIGH        0x54u

/* PHY registers (AX_ACCESS_PHY). */
#define GMII_PHYSR           0x11u
#define PHYSR_SPEED_MASK     0xC000u
#define PHYSR_GIGA           0x8000u
#define PHYSR_100            0x4000u
#define PHYSR_FULL           0x2000u
#define PHYSR_LINK           0x0400u
#define PHY_MMD_CTRL         0x0Du
#define PHY_MMD_DATA         0x0Eu

#define INT_LINK             0x00010000u

/* The AX88179A/B (CUsbEnet's AX179A_*): requests, MAC registers and values. */
#define A_RELOAD_EEPROM_EFUSE 0x06u
#define A_FW_MODE            0x08u
#define A_ACCESS_BL          0x2Au
#define A_PHY_POWER          0x31u
#define A_CHIP_STATUS        0x05u
#define A_ETH_TX_GAP         0x0Du
#define A_BFM_DATA           0x0Eu
#define A_MONITOR_MODE       0x24u
#define A_VLAN_ID_CONTROL    0x2Bu
#define A_BM_INT_MASK        0x41u
#define A_BM_RX_DMA_CTL      0x43u
#define A_BM_TX_DMA_CTL      0x46u
#define A_PAUSE_LOW          0x54u      /* the 16-bit pause value starts here */
#define A_PAUSE_HIGH         0x55u
#define A_RX_STATUS_CDC      0x6Du
#define A_ARC_CTRL           0x9Eu
#define A_SWP_CTRL           0xB1u
#define A_CDC_DELAY_TX       0xB5u
#define A_MAC_PATH           0xB7u
#define A_NEW_PAUSE_CTRL     0xB8u
#define A_BULK_OUT_CTRL      0xB9u
#define A_RX_DATA_CDC_CNT    0xC0u
#define A_TX_HDR_CKSUM       0xCCu
#define A_EP5_EHR            0xF9u
#define A_CHECKSUMS          0x67u      /* IP, TCP, UDP, TCPv6, UDPv6 */
#define A_PHY_ADVERTISE      0x0DE1u    /* 10/100 both duplexes, pause both ways */
#define A_BMCR_KEEP          0xB3FFu    /* power-down and isolate cleared */
#define A_PHY_READY_TRIES    31
#define A_PHY_READY_MS       50

#define ASIX_VENDOR          0x0B95u
#define BCD_179A             0x0200u

#define RX_COUNT    8
#define RX_SIZE     (26 * 1024)     /* 1024 * (BULKIN[3] + 2) at the largest, 100M */
#define TX_COUNT    32
#define TX_SIZE     2048
#define INTR_SIZE   16
#define DESC_SIZE   512
#define DMA_BYTES   (RX_COUNT * RX_SIZE + TX_COUNT * TX_SIZE)

#define RX_ERROR_LIMIT   64
#define INTR_ERROR_LIMIT 16
#define POLL_TICKS       30         /* ~1 s: the PHY polled while the link is down */
#define AX179_MAX        2
#define NONE             0xFFFFFFFFu

enum {
    AX179_STEP_OK = 0,
    AX179_STEP_INSTANCE,
    AX179_STEP_DMA,
    AX179_STEP_CONFIG_DESC,
    AX179_STEP_LAYOUT,
    AX179_STEP_SET_CONFIG,
    AX179_STEP_A_FIRMWARE,      /* AX88179A/B: firmware mode, reload, PHY power */
    AX179_STEP_POWER,
    AX179_STEP_CLOCK,
    AX179_STEP_MAC,
    AX179_STEP_BULKIN,
    AX179_STEP_RX_CTL,
    AX179_STEP_MEDIUM,
    AX179_STEP_PHY,
    AX179_STEP_ENDPOINTS,
    AX179_STEP_A_MAC,           /* AX88179A/B: the MAC set-up writes */
};

/* The bulk-in aggregation for each link speed (RX_BULKIN_QCTRL, 5 bytes), as Linux's
 * AX88179_BULKIN_SIZE: [0] super speed, [1] gigabit on high speed, [2] 100M on
 * high speed, [3] full speed or 10M. The console's port is high speed. */
static const u8 bulkin[4][5] = {
    { 7, 0x4F, 0x00, 0x12, 0xFF },
    { 7, 0x20, 0x03, 0x16, 0xFF },
    { 7, 0xAE, 0x07, 0x18, 0xFF },
    { 7, 0xCC, 0x4C, 0x18, 0x08 },
};

/* The AX88179A/B's, on high speed (CUsbEnet's Ax88179ABBulkIn*): gigabit, 100M full
 * and half duplex, 10M. */
static const u8 bulkin_a[4][5] = {
    { 0x05, 0xC0, 0x02, 0x06, 0x0F },
    { 0x05, 0xC0, 0x04, 0x06, 0x0F },
    { 0x07, 0xC0, 0x04, 0x06, 0x0F },
    { 0x07, 0x00, 0x00, 0x03, 0x3F },
};

/* Read by tools/xectl.py; field order is its STRUCTS["ax179_stats"]. */
typedef struct {
    u32 bcd_device;         /* 0x0100 AX88179, 0x0200 AX88179A/B */
    u32 product;
    u32 eeprom_43;          /* the auto-detach word, 0xFFFF if none */
    u32 phy_status;         /* GMII PHYSR at the last link check */
    u32 medium;             /* MEDIUM_STATUS_MODE as last written */
    u32 usb_speed;          /* PHYSICAL_LINK_STATUS */
    u32 link_resets;
    u32 rx_transfers;
    u32 rx_bad;             /* transfers that did not parse */
    u32 rx_bad_frames;      /* frames the chip marked bad */
    u32 intr_events;
    u32 intr_errors;
    u32 tx_transfers;
    u32 tx_too_big;
    u32 family_a;           /* 1: an AX88179A/B */
    u32 chip_status;        /* AX88179A/B: CHIP_STATUS (version in bits 7..4, 6) */
    u32 firmware;           /* AX88179A/B: the three firmware bytes, 0xFD..0xFF */
    u32 intr_link;          /* the link the interrupt pipe last reported */
} AX179_STATS;

volatile AX179_STATS ax179_stats[AX179_MAX];

typedef struct AX179 {
    u32 in_use;
    NET_DEV *nd;
    USB_DEV *dev;
    volatile AX179_STATS *st;
    ASIX_LAYOUT layout;
    void *ep_in, *ep_out, *ep_intr;
    URB rx_urb[RX_COUNT];
    u8 *rx_buf[RX_COUNT];
    u32 rx_consecutive_errors;
    URB tx_urb[TX_COUNT];
    u8 *tx_buf[TX_COUNT];
    u32 tx_next;
    u32 tx_inflight;
    KSPIN_LOCK tx_lock;
    URB intr_urb;
    u8 *intr_buf;
    u32 intr_consecutive_errors;
    u8 *desc_buf;
    volatile u32 link_check;    /* set from the interrupt pipe: the worker looks */
    volatile u32 intr_seen;     /* the interrupt pipe has reported the link */
    volatile u32 intr_link;
    u32 family_a;               /* an AX88179A/B: other registers and framing */
    u32 rx_ctl;
    u32 link_up;
    u32 poll_ticks;
    volatile u32 running;
} AX179;

static AX179 ax179_inst[AX179_MAX];

static u32 fail(AX179 *a, u32 step, u32 status)
{
    a->nd->st->step = step;
    a->nd->st->step_status = status;
    return step;
}

/* --- registers ---------------------------------------------------------------- */

static u32 mac_read(AX179 *a, u32 reg, u8 *data, u32 length)
{
    return asix_read(a->dev, AX_ACCESS_MAC, (u16)reg, (u16)length, data, (u16)length);
}

static u32 mac_write(AX179 *a, u32 reg, const u8 *data, u32 length)
{
    return asix_write(a->dev, AX_ACCESS_MAC, (u16)reg, (u16)length, data, (u16)length);
}

static u32 mac_write8(AX179 *a, u32 reg, u32 value)
{
    u8 b = (u8)value;

    return mac_write(a, reg, &b, 1);
}

static u32 mac_write16(AX179 *a, u32 reg, u32 value)
{
    u8 b[2];

    asix_put16(b, value);
    return mac_write(a, reg, b, 2);
}

static u32 mac_read8(AX179 *a, u32 reg, u32 *value)
{
    u8 b = 0;
    u32 status = mac_read(a, reg, &b, 1);

    *value = b;
    return status;
}

static u32 mac_read16(AX179 *a, u32 reg, u32 *value)
{
    u8 b[2] = { 0, 0 };
    u32 status = mac_read(a, reg, b, 2);

    *value = asix_le16(b);
    return status;
}

static u32 phy_read(AX179 *a, u32 reg, u32 *value)
{
    u8 b[2] = { 0, 0 };
    u32 status = asix_read(a->dev, AX_ACCESS_PHY, AX_PHY_ID, (u16)reg, b, 2);

    *value = asix_le16(b);
    return status;
}

static u32 phy_write(AX179 *a, u32 reg, u32 value)
{
    u8 b[2];

    asix_put16(b, value);
    return asix_write(a->dev, AX_ACCESS_PHY, AX_PHY_ID, (u16)reg, b, 2);
}

/* --- receive ------------------------------------------------------------------ */

static void rx_complete(URB *urb, u32 status);

static void rx_queue(URB *urb)
{
    AX179 *a = (AX179 *)urb->Owner;

    usb_prepare(urb, a->dev, a->ep_in, rx_complete, a->rx_buf[urb->Tag], RX_SIZE, 0);
    usb_submit(urb);
}

static void deliver(const u8 *frame, u32 length, void *context)
{
    AX179 *a = (AX179 *)context;

    net_frame_in(a->nd, frame, length);
}

static void rx_complete(URB *urb, u32 status)
{
    AX179 *a = (AX179 *)urb->Owner;
    u32 bad = 0;

    if (!a->running || !a->dev->active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        a->rx_consecutive_errors = 0;
        a->st->rx_transfers++;
        if (urb->Actual &&
            (a->family_a ? ax179a_rx_split(a->rx_buf[urb->Tag], urb->Actual, deliver, a, &bad)
                         : ax179_rx_split(a->rx_buf[urb->Tag], urb->Actual, deliver, a, &bad)) < 0)
            a->st->rx_bad++;
        a->st->rx_bad_frames += bad;
    } else {
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
    AX179 *a = (AX179 *)urb->Owner;
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

static u32 ax179_tx_space(NET_DEV *nd)
{
    AX179 *a = (AX179 *)nd->priv;

    return a && a->running && a->link_up && !a->tx_urb[a->tx_next].Busy;
}

/* One frame a transfer; the submit after the lock, a completion at once would
 * re-enter tx_complete. */
static u32 ax179_xmit(NET_DEV *nd, const u8 *frame, u32 length)
{
    AX179 *a = (AX179 *)nd->priv;
    u32 irql, slot = NONE, n, i;
    u8 *b;

    if (!a || !a->running) {
        nd->st->tx_drops++;
        return 0;
    }
    if (length + 2 * AX179_TX_HEADER + 8 > TX_SIZE) {
        a->st->tx_too_big++;
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
    b = a->tx_buf[slot];
    if (a->family_a) {
        n = ax179a_tx_build(b, TX_SIZE, frame, length, a->layout.out_max);
    } else {
        ax179_tx_header(b, length, a->layout.out_max);
        for (i = 0; i < length; i++)
            b[AX179_TX_HEADER + i] = frame[i];
        n = AX179_TX_HEADER + length;
        if (a->layout.out_max && n % a->layout.out_max == 0)
            b[n++] = 0;             /* the padding the header announced */
    }
    a->st->tx_transfers++;
    usb_prepare(&a->tx_urb[slot], a->dev, a->ep_out, tx_complete, b, n, 0);
    usb_submit(&a->tx_urb[slot]);
    return 1;
}

/* --- the interrupt pipe ------------------------------------------------------- */

static void intr_complete(URB *urb, u32 status);

static void intr_queue(AX179 *a)
{
    usb_prepare(&a->intr_urb, a->dev, a->ep_intr, intr_complete, a->intr_buf,
                a->layout.intr_max < INTR_SIZE ? a->layout.intr_max : INTR_SIZE, 0);
    usb_submit(&a->intr_urb);
}

static void intr_complete(URB *urb, u32 status)
{
    AX179 *a = (AX179 *)urb->Owner;

    if (!a->running || !a->dev->active) {
        urb->Busy = 0;
        return;
    }
    if (NT_SUCCESS(status)) {
        a->intr_consecutive_errors = 0;
        a->st->intr_events++;
        if (urb->Actual >= 8) {
            u32 up = (asix_le32(a->intr_buf) & INT_LINK) != 0;

            a->st->intr_link = up;
            if (!a->intr_seen || up != a->intr_link || up != a->link_up) {
                a->intr_link = up;
                a->intr_seen = 1;
                a->link_check = 1;
                usb_wake();
            }
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

/* ax88179_link_reset: the PHY's speed sets the medium mode and the bulk-in
 * aggregation; then the link goes up to the network layer. Thread. */
static void link_check(AX179 *a)
{
    u32 physr = 0, usb = 0, mode, mbps, which;

    a->st->link_resets++;
    if (!NT_SUCCESS(phy_read(a, GMII_PHYSR, &physr)))
        return;
    a->st->phy_status = physr;
    if (!(physr & PHYSR_LINK)) {
        if (a->link_up) {
            a->link_up = 0;
            net_link_report(a->nd, 0);
        }
        return;
    }
    mac_read8(a, AX_PHYSICAL_LINK_STATUS, &usb);
    a->st->usb_speed = usb;
    mode = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW | AX_MEDIUM_RXFLOW;
    if ((physr & PHYSR_SPEED_MASK) == PHYSR_GIGA) {
        mode |= AX_MEDIUM_GIGAMODE | AX_MEDIUM_EN_125MHZ;
        which = usb & AX_USB_SS ? 0 : usb & AX_USB_HS ? 1 : 3;
        mbps = 1000;
    } else if ((physr & PHYSR_SPEED_MASK) == PHYSR_100) {
        mode |= AX_MEDIUM_PS;
        which = usb & (AX_USB_SS | AX_USB_HS) ? 2 : 3;
        mbps = 100;
    } else {
        which = 3;
        mbps = 10;
    }
    if (physr & PHYSR_FULL)
        mode |= AX_MEDIUM_FULL_DUPLEX;
    mac_write(a, AX_RX_BULKIN_QCTRL, bulkin[which], 5);
    mac_write16(a, AX_MEDIUM_STATUS_MODE, mode);
    a->st->medium = mode;
    net_speed_report(a->nd, mbps);
    if (!a->link_up) {
        a->link_up = 1;
        net_link_report(a->nd, 1);
    }
}

/* The AX88179A/B's link reset (CUsbEnet's QueueNextLinkResetTransfer): receive and
 * the MAC path stopped, the CDC timings and pause levels, the bulk-in aggregation
 * and medium mode for the speed, then receive and the MAC path started again. The
 * interrupt pipe is the link's authority here: PHYSR can carry the speed without
 * its link bit (0x9000 at gigabit). Thread. */
static void link_check_a(AX179 *a)
{
    u32 physr = 0, up, mode, mbps, full, which;
    u8 rx_status[3], rx_count;

    a->st->link_resets++;
    if (!NT_SUCCESS(phy_read(a, GMII_PHYSR, &physr)))
        return;
    a->st->phy_status = physr;
    up = a->intr_seen ? a->intr_link : (physr & PHYSR_LINK) != 0;
    if (!up) {
        if (a->link_up) {
            a->link_up = 0;
            net_link_report(a->nd, 0);
        }
        return;
    }
    mode = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW | AX_MEDIUM_RXFLOW;
    full = (physr & PHYSR_FULL) != 0;
    if ((physr & PHYSR_SPEED_MASK) == PHYSR_GIGA || (physr & PHYSR_SPEED_MASK) == 0xC000u) {
        mode |= AX_MEDIUM_GIGAMODE;
        full = 1;                       /* gigabit is full duplex here */
        which = 0;
        mbps = 1000;
    } else if ((physr & PHYSR_SPEED_MASK) == PHYSR_100) {
        mode |= AX_MEDIUM_PS;
        which = full ? 1 : 2;
        mbps = 100;
    } else {
        which = 3;
        mbps = 10;
    }
    if (full)
        mode |= AX_MEDIUM_FULL_DUPLEX;
    rx_status[0] = mbps == 10 ? 0xFA : 0x78;
    rx_status[1] = 0x70;
    rx_status[2] = mbps == 10 ? 0xFF : 0x00;
    rx_count = mbps == 10 ? 0xFA : 0x40;

    mac_write16(a, AX_RX_CTL, 0);
    mac_write8(a, A_MAC_PATH, 0);
    mac_write8(a, A_CDC_DELAY_TX, 0xA5);
    mac_write16(a, A_PAUSE_LOW, 0x0410);
    mac_write8(a, A_ETH_TX_GAP, 0);
    mac_write8(a, A_EP5_EHR, 0x07);
    mac_write8(a, A_NEW_PAUSE_CTRL, 0x29);
    mac_write(a, A_RX_STATUS_CDC, rx_status, 3);
    mac_write(a, A_RX_DATA_CDC_CNT, &rx_count, 1);
    mac_write8(a, A_BFM_DATA, 0);
    mac_write(a, AX_RX_BULKIN_QCTRL, bulkin_a[which], 5);
    mac_write16(a, AX_MEDIUM_STATUS_MODE, mode);
    mac_write16(a, AX_RX_CTL, a->rx_ctl);
    mac_write8(a, A_MAC_PATH, 0x03);
    a->st->medium = mode;
    net_speed_report(a->nd, mbps);
    if (!a->link_up) {
        a->link_up = 1;
        net_link_report(a->nd, 1);
    }
}

static void ax179_tick(NET_DEV *nd)
{
    AX179 *a = (AX179 *)nd->priv;

    if (!a || !a->running)
        return;
    if (a->link_check || (!a->link_up && ++a->poll_ticks >= POLL_TICKS)) {
        a->link_check = 0;
        a->poll_ticks = 0;
        if (a->family_a)
            link_check_a(a);
        else
            link_check(a);
    }
}

/* --- bring-up ----------------------------------------------------------------- */

static u32 alloc_buffers(AX179 *a)
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

static AX179 *take_instance(NET_DEV *nd)
{
    u32 i;

    for (i = 0; i < AX179_MAX; i++) {
        AX179 *a = &ax179_inst[i];

        if (a->in_use && a->nd != nd)
            continue;
        a->in_use = 1;
        a->nd = nd;
        a->dev = nd->usb;
        a->st = &ax179_stats[i];
        a->intr_urb.Owner = a;
        nd->priv = a;
        return a;
    }
    return 0;
}

/* ax88179_disable_eee: EEE advertisement (MMD 7, register 0x3C) cleared through the
 * PHY's MMD window, then autonegotiation restarted. */
static u32 phy_setup(AX179 *a)
{
    u32 status = phy_write(a, PHY_MMD_CTRL, 0x0007);

    if (NT_SUCCESS(status))
        status = phy_write(a, PHY_MMD_DATA, 0x003C);
    if (NT_SUCCESS(status))
        status = phy_write(a, PHY_MMD_CTRL, 0x4007);
    if (NT_SUCCESS(status))
        status = phy_write(a, PHY_MMD_DATA, 0x0000);
    if (NT_SUCCESS(status))
        status = phy_write(a, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    return status;
}

/* A vendor request with no data or one byte, as the AX88179A/B's firmware set-up
 * uses them. */
static u32 vendor_out(AX179 *a, u32 request, u32 value, const u8 *data, u32 length)
{
    return asix_write(a->dev, (u8)request, (u16)value, 0, data, (u16)length);
}

/* CUsbEnet's CAx179AFamily::AdvanceInitStage, waited for step by step: the
 * chip status and firmware version, firmware mode on, the EEPROM / efuse reloaded,
 * the PHY powered down and up again, the MAC's DMA, VLAN, ARC and header checksum
 * off, the MAC address, checksum offload and the monitor mode, the default medium,
 * the PHY waited for (its id register answering), EEE off, BMCR's power-down and
 * isolate cleared, the advertisement written, autonegotiation restarted, receive
 * started (promiscuous, as for the AX88179). Returns 0 or the step that failed. */
static u32 start_a(AX179 *a)
{
    NET_DEV *nd = a->nd;
    u32 status, value = 0, i, bmcr = 0;
    u8 fw[3], mac[6], power;

    if (NT_SUCCESS(mac_read8(a, A_CHIP_STATUS, &value)))
        a->st->chip_status = value;
    for (i = 0; i < 3; i++) {
        fw[i] = 0;
        asix_read(a->dev, A_ACCESS_BL, (u16)(0xFD + i), 1, &fw[i], 1);
    }
    a->st->firmware = (u32)fw[0] << 16 | (u32)fw[1] << 8 | fw[2];
    status = vendor_out(a, A_FW_MODE, 1, 0, 0);
    if (NT_SUCCESS(status))
        status = vendor_out(a, A_RELOAD_EEPROM_EFUSE, 0, 0, 0);
    power = 0;
    if (NT_SUCCESS(status))
        status = vendor_out(a, A_PHY_POWER, 0, &power, 1);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_A_FIRMWARE, status);
    usb_sleep(250);
    power = 0x02;
    status = vendor_out(a, A_PHY_POWER, 0, &power, 1);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_A_FIRMWARE, status);
    usb_sleep(250);

    status = mac_write8(a, A_BULK_OUT_CTRL, 0x02);
    if (NT_SUCCESS(status))
        status = mac_write16(a, AX_RX_CTL, 0);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_PAUSE_LOW, 0x04);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_PAUSE_HIGH, 0x10);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_VLAN_ID_CONTROL, 0);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_BM_INT_MASK, 0xFF);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_BM_RX_DMA_CTL, 0);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_BM_TX_DMA_CTL, 0);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_ARC_CTRL, 0);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_SWP_CTRL, 0);
    if (NT_SUCCESS(status))
        status = mac_write8(a, A_TX_HDR_CKSUM, 0);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_A_MAC, status);

    status = mac_read(a, AX_NODE_ID, mac, 6);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_MAC, status);
    for (i = 0; i < 6; i++)
        nd->mac[i] = mac[i];

    mac_write8(a, AX_RXCOE_CTL, A_CHECKSUMS);
    mac_write8(a, AX_TXCOE_CTL, A_CHECKSUMS);
    mac_write8(a, A_MONITOR_MODE, 0x04);
    value = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW | AX_MEDIUM_RXFLOW |
            AX_MEDIUM_GIGAMODE | AX_MEDIUM_FULL_DUPLEX;
    status = mac_write16(a, AX_MEDIUM_STATUS_MODE, value);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_MEDIUM, status);
    a->st->medium = value;

    for (i = 0; i < A_PHY_READY_TRIES; i++) {
        if (NT_SUCCESS(phy_read(a, 2, &value)) && value && value != 0xFFFF)
            break;
        usb_sleep(A_PHY_READY_MS);
    }
    status = phy_write(a, PHY_MMD_CTRL, 0x0007);
    if (NT_SUCCESS(status))
        status = phy_write(a, PHY_MMD_DATA, 0x003C);
    if (NT_SUCCESS(status))
        status = phy_write(a, PHY_MMD_CTRL, 0x4007);
    if (NT_SUCCESS(status))
        status = phy_write(a, PHY_MMD_DATA, 0x0000);
    if (NT_SUCCESS(status) && NT_SUCCESS(phy_read(a, MII_BMCR, &value))) {
        bmcr = value & A_BMCR_KEEP;
        if (bmcr != value)
            phy_write(a, MII_BMCR, bmcr);
    }
    usb_sleep(60);
    if (NT_SUCCESS(status))
        status = phy_write(a, MII_ADVERTISE, A_PHY_ADVERTISE);
    if (NT_SUCCESS(status))
        status = phy_write(a, MII_CTRL1000, ADVERTISE_1000FULL);
    if (NT_SUCCESS(status))
        status = phy_write(a, MII_BMCR, bmcr | BMCR_ANENABLE | BMCR_ANRESTART);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_PHY, status);

    a->rx_ctl = AX_RX_CTL_DROPCRCERR | AX_RX_CTL_START | AX_RX_CTL_AP | AX_RX_CTL_AB |
                AX_RX_CTL_AMALL | AX_RX_CTL_PRO;
    status = mac_write16(a, AX_RX_CTL, a->rx_ctl);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_RX_CTL, status);
    return 0;
}

static u32 ax179_start(NET_DEV *nd)
{
    AX179 *a = take_instance(nd);
    const u8 *device = UsbdGetDeviceDescriptor(nd->usb->device);
    u32 status, length = 0, value = 0, i;
    u8 mac[6], e43[2];

    if (!a) {
        nd->st->step = AX179_STEP_INSTANCE;
        return AX179_STEP_INSTANCE;
    }
    a->running = 0;
    a->link_up = 0;
    a->link_check = 0;
    a->poll_ticks = POLL_TICKS;         /* a first look at once */
    a->rx_consecutive_errors = a->intr_consecutive_errors = 0;
    a->ep_in = a->ep_out = a->ep_intr = 0;
    if (device) {
        a->st->product = asix_le16(device + 10);
        a->st->bcd_device = asix_le16(device + 12);
    }
    if (!alloc_buffers(a))
        return fail(a, AX179_STEP_DMA, 0);

    status = asix_get_config(a->dev, a->desc_buf, DESC_SIZE, &length);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_CONFIG_DESC, status);
    if (!asix_parse_config(a->desc_buf, length, &a->layout))
        return fail(a, AX179_STEP_LAYOUT, length);
    status = asix_set_config(a->dev, a->layout.config_value);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_SET_CONFIG, status);
    a->family_a = a->st->product == 0x1790 && a->st->bcd_device >= BCD_179A;
    a->st->family_a = a->family_a;
    a->intr_seen = a->intr_link = 0;
    if (a->family_a) {
        status = start_a(a);
        if (status)
            return status;
        goto endpoints;
    }

    /* ax88179_reset */
    status = mac_write16(a, AX_PHYPWR_RSTCTL, 0);
    if (NT_SUCCESS(status))
        status = mac_write16(a, AX_PHYPWR_RSTCTL, AX_PHYPWR_IPRL);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_POWER, status);
    usb_sleep(500);
    status = mac_write8(a, AX_CLK_SELECT, AX_CLK_ACS | AX_CLK_BCS);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_CLOCK, status);
    usb_sleep(200);

    /* ax88179_auto_detach: only if the EEPROM asks for it */
    a->st->eeprom_43 = 0xFFFF;
    if (NT_SUCCESS(asix_read(a->dev, AX_ACCESS_EEPROM, 0x43, 1, e43, 2))) {
        a->st->eeprom_43 = asix_le16(e43);
        if (a->st->eeprom_43 != 0xFFFF && (a->st->eeprom_43 & 0x0100)) {
            if (NT_SUCCESS(mac_read8(a, AX_CLK_SELECT, &value)))
                mac_write8(a, AX_CLK_SELECT, value | AX_CLK_ULR);
            if (NT_SUCCESS(mac_read16(a, AX_PHYPWR_RSTCTL, &value)))
                mac_write16(a, AX_PHYPWR_RSTCTL, value | AX_PHYPWR_AT);
        }
    }

    status = mac_read(a, AX_NODE_ID, mac, 6);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_MAC, status);
    for (i = 0; i < 6; i++)
        nd->mac[i] = mac[i];

    status = mac_write(a, AX_RX_BULKIN_QCTRL, bulkin[1], 5);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_BULKIN, status);
    mac_write8(a, AX_PAUSE_LOW, 0x34);
    mac_write8(a, AX_PAUSE_HIGH, 0x52);
    mac_write8(a, AX_RXCOE_CTL, 0);
    mac_write8(a, AX_TXCOE_CTL, 0);
    status = mac_write16(a, AX_RX_CTL, AX_RX_CTL_DROPCRCERR | AX_RX_CTL_IPE |
                                       AX_RX_CTL_START | AX_RX_CTL_AP | AX_RX_CTL_AMALL |
                                       AX_RX_CTL_AB | AX_RX_CTL_PRO);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_RX_CTL, status);
    value = AX_MEDIUM_RECEIVE_EN | AX_MEDIUM_TXFLOW | AX_MEDIUM_RXFLOW |
            AX_MEDIUM_FULL_DUPLEX | AX_MEDIUM_GIGAMODE;
    status = mac_write16(a, AX_MEDIUM_STATUS_MODE, value);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_MEDIUM, status);
    a->st->medium = value;
    status = phy_setup(a);
    if (!NT_SUCCESS(status))
        return fail(a, AX179_STEP_PHY, status);

endpoints:
    a->ep_in = usb_open(a->dev, USB_TYPE_BULK, a->layout.ep_in, a->layout.in_max, 0);
    a->ep_out = usb_open(a->dev, USB_TYPE_BULK, a->layout.ep_out, a->layout.out_max, 0);
    if (a->layout.ep_intr)
        a->ep_intr = usb_open(a->dev, USB_TYPE_INTERRUPT, a->layout.ep_intr,
                              a->layout.intr_max, a->layout.intr_interval);
    if (!a->ep_in || !a->ep_out)
        return fail(a, AX179_STEP_ENDPOINTS, usb_stats.last_open_status);

    a->tx_next = 0;
    a->tx_inflight = 0;
    a->running = 1;
    mem_sync();
    for (i = 0; i < RX_COUNT; i++)
        rx_queue(&a->rx_urb[i]);
    if (a->ep_intr)
        intr_queue(a);
    nd->st->step = AX179_STEP_OK;
    return 0;
}

static void ax179_detached(NET_DEV *nd)
{
    AX179 *a = (AX179 *)nd->priv;

    if (!a)
        return;
    a->running = 0;
    a->link_up = 0;
    a->in_use = 0;
    mem_sync();
}

/* The AX88179 / AX88178A adapters of Linux's ax88179_178a table. */
static const u16 adapters[][2] = {
    { ASIX_VENDOR, 0x1790 },                    /* ASIX AX88179 (and A/B) */
    { ASIX_VENDOR, 0x178A },                    /* ASIX AX88178A */
    { 0x04B4, 0x3610 },                         /* Cypress GX3 */
    { 0x2001, 0x4A00 },                         /* D-Link DUB-1312 */
    { 0x0DF6, 0x0072 },                         /* Sitecom */
    { 0x04E8, 0xA100 },                         /* Samsung */
    { 0x17EF, 0x304B },                         /* Lenovo */
    { 0x050D, 0x0128 },                         /* Belkin */
    { 0x0930, 0x0A13 },                         /* Toshiba */
    { 0x0711, 0x0179 },                         /* Magic Control Technology */
    { 0x07C9, 0x000E }, { 0x07C9, 0x000F },     /* Allied Telesis */
    { 0x07C9, 0x0010 },
};

static u32 ax179_match(const u8 *device, const u8 *interface)
{
    u32 vid = asix_le16(device + 8), pid = asix_le16(device + 10), i;

    (void)interface;
    for (i = 0; i < sizeof(adapters) / sizeof(adapters[0]); i++)
        if (adapters[i][0] == vid && adapters[i][1] == pid)
            return XE_MATCH_VENDOR;
    return XE_MATCH_NONE;
}

const XE_DRIVER xe_driver_ax88179 = {
    .name = "ax88179",
    .kind = XE_KIND_WIRED,
    .match = ax179_match,
    .start = ax179_start,
    .detached = ax179_detached,
    .xmit = ax179_xmit,
    .tx_space = ax179_tx_space,
    .tick = ax179_tick,
};
