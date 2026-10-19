/* Realtek registers and PHY; see rtl.h. Shapes from Linux r8152 (GPL-2.0):
 * generic_ocp_read/write, ocp_read_word/ocp_write_word, ocp_reg_read/write,
 * rtl8152_set_speed. */

#include "rtl.h"
#include "usb.h"
#include "net.h"

#define REQ_REGS      0x05u
#define BYTE_EN_WORD  0x33u

#define BMCR_ANENABLE       0x1000u
#define BMCR_ANRESTART      0x0200u
#define ADVERTISE_SPEEDS    0x01E0u     /* 10H 10F 100H 100F */
#define ADVERTISE_10HALF    0x0020u
#define ADVERTISE_10FULL    0x0040u
#define ADVERTISE_100HALF   0x0080u
#define ADVERTISE_100FULL   0x0100u
#define ADVERTISE_1000FULL  0x0200u
#define ADVERTISE_1000HALF  0x0100u
#define ADV2_5G             0x0080u

#define REG_SIZE 64

static u8 *scratch;             /* DMA memory from the core's scratch block */
static u32 page = 0xFFFFFFFFu;  /* the OCP page last selected, or unknown */

static u32 control(u8 type, u32 addr, u32 index, u32 *actual)
{
    u8 setup[8];

    if (!scratch)
        scratch = (u8 *)net_dma_scratch(REG_SIZE);
    if (!scratch)
        return STATUS_UNSUCCESSFUL;
    setup[0] = type;
    setup[1] = REQ_REGS;
    setup[2] = (u8)(addr & 0xFF);
    setup[3] = (u8)(addr >> 8);
    setup[4] = (u8)(index & 0xFF);
    setup[5] = (u8)(index >> 8);
    setup[6] = 4;
    setup[7] = 0;
    return usb_control(setup, scratch, 4, actual, 500);
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

u32 rtl_read_dword(u32 mcu, u32 addr, u32 *value)
{
    u32 status = control(0xC0, addr & ~3u, mcu, 0);

    if (NT_SUCCESS(status))
        *value = le32(scratch);
    return status;
}

u32 rtl_read_word(u32 mcu, u32 addr, u32 *value)
{
    u32 shift = addr & 2u;
    u32 status = control(0xC0, addr & ~3u, mcu | (BYTE_EN_WORD << shift), 0);

    if (NT_SUCCESS(status))
        *value = (le32(scratch) >> (shift * 8)) & 0xFFFFu;
    return status;
}

u32 rtl_write_word(u32 mcu, u32 addr, u32 value)
{
    u32 shift = addr & 2u;
    u32 v = (value & 0xFFFFu) << (shift * 8);

    if (!scratch)
        scratch = (u8 *)net_dma_scratch(REG_SIZE);
    if (!scratch)
        return STATUS_UNSUCCESSFUL;
    scratch[0] = (u8)v;
    scratch[1] = (u8)(v >> 8);
    scratch[2] = (u8)(v >> 16);
    scratch[3] = (u8)(v >> 24);
    return control(0x40, addr & ~3u, mcu | (BYTE_EN_WORD << shift), 0);
}

void rtl_forget_page(void)
{
    page = 0xFFFFFFFFu;
}

static u32 select_page(u32 addr, u32 *index)
{
    u32 base = addr & 0xF000u;
    u32 status = STATUS_SUCCESS;

    if (base != page) {
        status = rtl_write_word(RTL_PLA, RTL_PLA_OCP_GPHY_BASE, base);
        page = NT_SUCCESS(status) ? base : 0xFFFFFFFFu;
    }
    *index = (addr & 0x0FFFu) | 0xB000u;
    return status;
}

u32 rtl_phy_read(u32 addr, u32 *value)
{
    u32 index, status = select_page(addr, &index);

    return NT_SUCCESS(status) ? rtl_read_word(RTL_PLA, index, value) : status;
}

u32 rtl_phy_write(u32 addr, u32 value)
{
    u32 index, status = select_page(addr, &index);

    return NT_SUCCESS(status) ? rtl_write_word(RTL_PLA, index, value) : status;
}

void rtl_advert_regs(u32 advertise, u32 *adv, u32 *ctrl1000, u32 *ctrl10g)
{
    *adv &= ~ADVERTISE_SPEEDS;
    if (advertise & RTL_ADV_10_HALF)
        *adv |= ADVERTISE_10HALF;
    if (advertise & RTL_ADV_10_FULL)
        *adv |= ADVERTISE_10FULL;
    if (advertise & RTL_ADV_100_HALF)
        *adv |= ADVERTISE_100HALF;
    if (advertise & RTL_ADV_100_FULL)
        *adv |= ADVERTISE_100FULL;

    *ctrl1000 &= ~(ADVERTISE_1000FULL | ADVERTISE_1000HALF);
    if (advertise & RTL_ADV_1000_FULL)
        *ctrl1000 |= ADVERTISE_1000FULL;

    *ctrl10g &= ~ADV2_5G;
    if (advertise & RTL_ADV_2500_FULL)
        *ctrl10g |= ADV2_5G;
}

/* rtl8152_set_speed's autonegotiation branch: rewrite the three advertisement
 * registers, then BMCR = ANENABLE | ANRESTART. */
u32 rtl_set_advertise(u32 advertise)
{
    u32 adv, c1000, c10g, status;

    status = rtl_phy_read(RTL_OCP_MII_ADVERTISE, &adv);
    if (NT_SUCCESS(status))
        status = rtl_phy_read(RTL_OCP_MII_CTRL1000, &c1000);
    if (NT_SUCCESS(status))
        status = rtl_phy_read(RTL_OCP_10GBT_CTRL, &c10g);
    if (!NT_SUCCESS(status))
        return status;

    rtl_advert_regs(advertise, &adv, &c1000, &c10g);

    status = rtl_phy_write(RTL_OCP_MII_ADVERTISE, adv);
    if (NT_SUCCESS(status))
        status = rtl_phy_write(RTL_OCP_MII_CTRL1000, c1000);
    if (NT_SUCCESS(status))
        status = rtl_phy_write(RTL_OCP_10GBT_CTRL, c10g);
    if (NT_SUCCESS(status))
        status = rtl_phy_write(RTL_OCP_MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
    return status;
}
