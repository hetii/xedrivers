/* The RTL8822B's efuse: the card's own settings, the address among them, as rtw88's
 * efuse.c reads them (rtw_parse_efuse_map) and rtw8822b_read_efuse picks them out.
 *
 * 1024 physical bytes, each a write of its address and a poll for the flag, are
 * packed into a 768-byte logical map: a header names a block of 8 and which of its
 * four words follow.
 */

#include "rtw.h"

#define EFUSE_BANK_WIFI 0

static u32 read_physical(RTW *r, u8 *map)
{
    u32 ctl, addr, tries;

    /* switch_efuse_bank, then cfg_ldo25(false): no 2.5 V while reading */
    rtw_write32_mask(r, REG_LDO_EFUSE_CTRL, BIT_MASK_EFUSE_BANK_SEL, EFUSE_BANK_WIFI);
    rtw_write8(r, REG_LDO_EFUSE_CTRL + 3,
               rtw_read8(r, REG_LDO_EFUSE_CTRL + 3) & (u8)~BIT_LDO25_EN);

    ctl = rtw_read32(r, REG_EFUSE_CTRL);
    for (addr = 0; addr < RTW_EFUSE_PHYSICAL; addr++) {
        ctl &= ~(BIT_MASK_EF_DATA | BITS_EF_ADDR);
        ctl |= (addr & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR;
        rtw_write32(r, REG_EFUSE_CTRL, ctl & ~BIT_EF_FLAG);
        for (tries = 0;; tries++) {
            ctl = rtw_read32(r, REG_EFUSE_CTRL);
            if (ctl & BIT_EF_FLAG)
                break;
            if (tries > 100 || !r->dev->active)
                return STATUS_UNSUCCESSFUL;
        }
        map[addr] = (u8)(ctl & BIT_MASK_EF_DATA);
    }
    return 0;
}

/* rtw_dump_logical_efuse_map */
static u32 to_logical(const u8 *phy, u8 *log)
{
    u32 p = 0, l, i, block, words;
    u8 h1, h2;

    for (i = 0; i < RTW_EFUSE_LOGICAL; i++)
        log[i] = 0xFF;
    while (p < RTW_EFUSE_PHYSICAL - RTW_EFUSE_PROTECT) {
        h1 = phy[p];
        h2 = phy[p + 1];
        if (h1 == 0xFF || ((h1 & 0x1F) == 0xF && h2 == 0xFF))
            break;
        if ((h1 & 0x1F) == 0xF) {               /* two-byte header */
            block = ((h2 & 0xF0) >> 1) | ((h1 >> 5) & 0x07);
            words = h2 & 0xF;
            p += 2;
        } else {
            block = (h1 & 0xF0) >> 4;
            words = h1 & 0xF;
            p += 1;
        }
        for (i = 0; i < 4; i++) {
            if (words & (1u << i))              /* a set bit: that word is absent */
                continue;
            l = (block << 3) + (i << 1);
            if (p + 1 > RTW_EFUSE_PHYSICAL - RTW_EFUSE_PROTECT || l + 1 > RTW_EFUSE_LOGICAL)
                return STATUS_UNSUCCESSFUL;
            log[l] = phy[p];
            log[l + 1] = phy[p + 1];
            p += 2;
        }
    }
    return 0;
}

/* Reads the map into r->efuse and the parts the driver uses into the stats: the
 * address into nd->mac. `phy` is scratch for the physical bytes. */
u32 rtw_parse_efuse_map(RTW *r)
{
    u8 *phy = r->bulk_buf, *log = r->efuse;
    u32 status, i;

    status = read_physical(r, phy);
    if (!status)
        status = to_logical(phy, log);
    if (status)
        return status;

    /* rtw8822b_read_efuse and rtw8822bu_efuse_parsing */
    r->st->rfe_option = log[0xCA];
    r->st->crystal_cap = log[0xB9] == 0xFF ? 0 : log[0xB9];
    for (i = 0; i < 6; i++)
        r->nd->mac[i] = log[RTW_EFUSE_MAC + i];
    r->st->mac_lo = (u32)log[RTW_EFUSE_MAC] << 24 | (u32)log[RTW_EFUSE_MAC + 1] << 16 |
                    (u32)log[RTW_EFUSE_MAC + 2] << 8 | log[RTW_EFUSE_MAC + 3];
    r->st->mac_hi = (u32)log[RTW_EFUSE_MAC + 4] << 8 | log[RTW_EFUSE_MAC + 5];
    return 0;
}
