/* Realtek RTL815x registers over vendor control requests, and the PHY behind them.
 *
 * Linux r8152's get_registers/set_registers: bmRequestType 0xC0 (read) / 0x40
 * (write), bRequest 0x05, wValue = register address (dword aligned), wIndex = MCU
 * type (PLA 0x0100, USB 0x0000) | byte enables. The PHY's registers are reached
 * through the OCP window: the 4 KB page goes into PLA 0xE86C, the register is then
 * PLA 0xB000 | (addr & 0xFFF). MII register n is OCP 0xA400 + 2n.
 *
 * The card answers these in every configuration -- the control endpoint is served
 * by its MCU whatever personality the bulk endpoints have -- which is what lets the
 * CDC-ECM transport still choose the link speed. Checked from the host with
 * tests/rtlreg.py against the twin card under Linux r8152.
 *
 * Thread-only: every call waits for its control transfer.
 */

#ifndef XE_RTL_H
#define XE_RTL_H

#include "kernel.h"
#include "usb.h"

#define RTL_PLA 0x0100u
#define RTL_USB 0x0000u

#define RTL_PLA_TCR0          0xE610u   /* chip version in bits 30..20 */
#define RTL_PLA_OCP_GPHY_BASE 0xE86Cu

#define RTL_OCP_MII_BMCR      0xA400u
#define RTL_OCP_MII_BMSR      0xA402u
#define RTL_OCP_MII_ADVERTISE 0xA408u
#define RTL_OCP_MII_LPA       0xA40Au
#define RTL_OCP_MII_CTRL1000  0xA412u
#define RTL_OCP_MII_STAT1000  0xA414u
#define RTL_OCP_PHY_STATUS    0xA420u
#define RTL_OCP_10GBT_CTRL    0xA5D4u   /* bit 7: advertise 2.5GBASE-T */
#define RTL_OCP_10GBT_STAT    0xA5D6u

/* What to advertise, as r8152's RTL_ADVERTISED_* bits. */
#define RTL_ADV_10_HALF   0x01u
#define RTL_ADV_10_FULL   0x02u
#define RTL_ADV_100_HALF  0x04u
#define RTL_ADV_100_FULL  0x08u
#define RTL_ADV_1000_FULL 0x20u
#define RTL_ADV_2500_FULL 0x40u
#define RTL_ADV_ALL       0x6Fu

u32 rtl_read_dword(USB_DEV *dev, u32 mcu, u32 addr, u32 *value);
u32 rtl_read_word(USB_DEV *dev, u32 mcu, u32 addr, u32 *value);
u32 rtl_write_word(USB_DEV *dev, u32 mcu, u32 addr, u32 value);
u32 rtl_phy_read(USB_DEV *dev, u32 addr, u32 *value);
u32 rtl_phy_write(USB_DEV *dev, u32 addr, u32 value);

/* Program the PHY's advertisement from RTL_ADV_* bits and restart negotiation.
 * Returns STATUS_SUCCESS or the first failing transfer's status. */
u32 rtl_set_advertise(USB_DEV *dev, u32 advertise);

/* Pure: the three advertisement register values for `advertise`, given what they
 * hold now (only the speed bits are changed). For tests/. */
void rtl_advert_regs(u32 advertise, u32 *adv, u32 *ctrl1000, u32 *ctrl10g);

void rtl_forget_page(USB_DEV *dev);     /* after a reset the window is the card's default */

/* Read by tests/xectl.py. */
typedef struct {
    u32 chip_version;       /* PLA_TCR0 >> 16, e.g. 0x7710 for an RTL8156B */
    u32 advertise_status;   /* of the last advertisement write */
} RTL_STATS;

extern volatile RTL_STATS rtl_stats[USB_MAX_DEVICES];   /* by device slot */

#endif /* XE_RTL_H */
