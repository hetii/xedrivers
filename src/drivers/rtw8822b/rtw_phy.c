/* The RTL8822B's baseband and radio: the tables, the radio's registers, the paths
 * and channels, as rtw88's phy.c and rtw8822b.c do them.
 */

#include "rtw.h"
#include "rtw8822b_defs.h"
#include "rtw8822b_txpwr.h"

#define RFREG_MASK          0xFFFFFu
#define MASKBYTE0           0x000000FFu
#define MASKBYTE1           0x0000FF00u
#define MASKLWORD           0x0000FFFFu
#define MASKDWORD           0xFFFFFFFFu
#define BB_PATH_A           1u
#define BB_PATH_B           2u
#define BB_PATH_AB          3u
#define RF_PATH_A           0
#define RF_PATH_B           1
#define DESC_RATE6M         0x04
#define INTF_USB            2u              /* rtw_phy_cond.intf */
#define BRANCH_ELSE         2
#define BRANCH_ENDIF        3

#define IS_CH_2G(ch)        ((ch) <= 14)
#define IS_CH_5G_1(ch)      ((ch) >= 36 && (ch) <= 48)
#define IS_CH_5G_2(ch)      ((ch) >= 52 && (ch) <= 64)
#define IS_CH_5G_3(ch)      ((ch) >= 100 && (ch) <= 144)
#define IS_CH_5G_4(ch)      ((ch) >= 149 && (ch) <= 177)
#define IS_CH_5G_MID(ch)    ((ch) >= 100 && (ch) <= 144)

static const u32 rf_base[2] = { 0x2800, 0x2C00 };   /* rtw8822b rf_base_addr */
static const u32 rf_sipi[2] = { 0xC90, 0xE90 };     /* rf_sipi_addr */

/* --- the radio's registers ------------------------------------------------------- */

static u32 shift_of(u32 mask)
{
    u32 s = 0;

    while (mask && !(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

/* rtw_phy_read_rf: the radio's registers are mirrored, four bytes each, from
 * rf_base. */
u32 rtw_read_rf(RTW *r, u32 path, u32 addr, u32 mask)
{
    return rtw_read32_mask(r, rf_base[path] + ((addr & 0xFF) << 2), mask & RFREG_MASK);
}

/* rtw_phy_write_rf_reg_sipi: address and 20 bits of data in one word to the
 * path's serial interface. */
void rtw_write_rf(RTW *r, u32 path, u32 addr, u32 mask, u32 data)
{
    mask &= RFREG_MASK;
    addr &= 0xFF;
    if (mask != RFREG_MASK) {
        u32 old = rtw_read_rf(r, path, addr, RFREG_MASK);

        data = (old & ~mask) | (data << shift_of(mask));
    }
    rtw_write32(r, rf_sipi[path], ((addr << 20) | (data & 0xFFFFF)) & 0x0FFFFFFF);
}

/* rtw_write32s_mask: 0xC00-0xCFF and 0xE00-0xEFF, path A and B, are laid out alike. */
static void write32s_mask(RTW *r, u32 addr, u32 mask, u32 data)
{
    rtw_write32_mask(r, addr, mask, data);
    rtw_write32_mask(r, addr + 0x200, mask, data);
}

/* --- the tables -------------------------------------------------------------------- */

enum { CFG_MAC, CFG_BB, CFG_AGC, CFG_RF_A, CFG_RF_B };

static void cfg(RTW *r, u32 kind, u32 addr, u32 data)
{
    switch (kind) {
    case CFG_MAC:                               /* rtw_phy_cfg_mac */
        rtw_write8(r, addr, (u8)data);
        break;
    case CFG_AGC:                               /* rtw_phy_cfg_agc */
        rtw_write32(r, addr, data);
        break;
    case CFG_BB:                                /* rtw_phy_cfg_bb */
        if (addr == 0xFE)
            usb_sleep(50);
        else if (addr == 0xFD)
            usb_sleep(5);
        else if (addr == 0xFC)
            usb_sleep(1);
        else if (addr < 0xF9 || addr > 0xFB)    /* 0xf9-0xfb: microseconds */
            rtw_write32(r, addr, data);
        break;
    default:                                    /* rtw_phy_cfg_rf */
        if (addr == 0xFFE)
            usb_sleep(50);
        else if (addr != 0xFE)                  /* 0xfe: 100 us */
            rtw_write_rf(r, kind == CFG_RF_A ? RF_PATH_A : RF_PATH_B, addr, RFREG_MASK,
                         data);
        break;
    }
}

/* check_positive against rtw_phy_setup_phy_cond's conditions: cut, package 15 (none
 * read), USB, the efuse's RFE option. A condition word is rtw_phy_cond read as
 * the little-endian bit fields it is: rfe 7:0, intf 11:8, pkg 15:12, cut 27:24. */
static u32 positive(RTW *r, u32 cond)
{
    u32 cut = (cond >> 24) & 0xF, pkg = (cond >> 12) & 0xF, intf = (cond >> 8) & 0xF;
    u32 my_cut = r->cut ? r->cut : 15;

    if (cut && cut != my_cut)
        return 0;
    if (pkg && pkg != 15)
        return 0;
    if (intf && intf != INTF_USB)
        return 0;
    return (cond & 0xFF) == r->st->rfe_option;
}

/* rtw_parse_tbl_phy_cond: pairs of words, an address and its value, between
 * if/elif/else/endif pairs whose top bit marks them. */
static void load_table(RTW *r, const RTW_TABLE *t, u32 kind)
{
    const u32 *p = t->data, *end = t->data + t->size;
    u32 matched = 1, skipped = 0, pos = 0;

    for (; p + 1 < end + 1 && p < end; p += 2) {
        u32 a = p[0];

        if (a & BIT(31)) {                      /* pos: if, elif, else, endif */
            u32 branch = (a >> 28) & 3;

            if (branch == BRANCH_ENDIF) {
                matched = 1;
                skipped = 0;
            } else if (branch == BRANCH_ELSE) {
                matched = !skipped;
            } else {
                pos = a;
            }
        } else if (a & BIT(30)) {               /* neg: the condition just named */
            if (!skipped) {
                matched = positive(r, pos);
                skipped = matched;
            } else {
                matched = 0;
            }
        } else if (matched) {
            cfg(r, kind, a, p[1]);
        }
    }
}

/* --- phy_set_param ---------------------------------------------------------------- */

/* cca_ifem_ccut_ext, the table of RFE options 3 and 5: rows 82c, 830, 838, columns
 * 1R-2G, 2R-2G, 1R-5G, 2R-5G. */
static const u32 cca_ifem_ext[3][4] = {
    { 0x75da8010, 0x75da8010, 0x75da8010, 0x75da8010 },
    { 0x79a0eaaa, 0x97A0EAAC, 0x79a0eaaa, 0x79a0eaaa },
    { 0x87765541, 0x86666341, 0x87765561, 0x86666361 },
};

/* rtw8822b_set_channel_cca, for an internal front end (RFE 3 and 5, the only
 * ones known here) and both receive paths. */
static void set_channel_cca(RTW *r, u32 channel, u32 bw)
{
    u32 col = IS_CH_2G(channel) ? 1 : 3;
    u32 r82c = cca_ifem_ext[0][col], r830 = cca_ifem_ext[1][col], r838 = cca_ifem_ext[2][col];

    if ((r->cut == 1 && bw == 1) || (r->st->rfe_option == 5 && col == 3))
        r830 = 0x79a0ea28;                      /* cut B 2R at 40 MHz; RFE 5 2R-5G */
    rtw_write32_mask(r, REG_CCASEL, MASKDWORD, r82c);
    rtw_write32_mask(r, REG_PDMFTH, MASKDWORD, r830);
    rtw_write32_mask(r, REG_CCA2ND, MASKDWORD, r838);
    if (bw == 0 && IS_CH_5G_MID(channel))
        rtw_write32_mask(r, REG_CCA2ND, 0xF0, 0x4);
}

/* rtw8822b_set_channel_rfe_ifem, both paths in both directions. */
static void set_channel_rfe(RTW *r, u32 channel)
{
    if (IS_CH_2G(channel)) {
        write32s_mask(r, REG_RFESEL0, 0xFFFFFF, 0x745774);
        write32s_mask(r, REG_RFESEL8, MASKBYTE1, 0x57);
    } else {
        write32s_mask(r, REG_RFESEL0, 0xFFFFFF, 0x477547);
        write32s_mask(r, REG_RFESEL8, MASKBYTE1, 0x75);
    }
    write32s_mask(r, REG_RFEINV, BIT(11) | BIT(10) | 0x3F, 0x0);
    write32s_mask(r, REG_TRSW, MASKLWORD, IS_CH_2G(channel) ? 0xA501 : 0xA5A5);
}

static void toggle_igi(RTW *r)
{
    u32 igi = rtw_read32_mask(r, REG_RXIGI_A, 0x7F);

    rtw_write32_mask(r, REG_RXIGI_A, 0x7F, igi - 2);
    rtw_write32_mask(r, REG_RXIGI_A, 0x7F, igi);
    rtw_write32_mask(r, REG_RXIGI_B, 0x7F, igi - 2);
    rtw_write32_mask(r, REG_RXIGI_B, 0x7F, igi);
    rtw_write32_mask(r, REG_RXPSEL, MASKBYTE0, 0x0);
    rtw_write32_mask(r, REG_RXPSEL, MASKBYTE0, BB_PATH_AB | (BB_PATH_AB << 4));
}

/* rtw8822b_config_trx_mode, both paths each way, not the 2-path transmit. */
static u32 config_trx_mode(RTW *r)
{
    u32 tx = BB_PATH_AB, rx = BB_PATH_AB, n;

    rtw_write32_mask(r, REG_AGCTR_A, MASKLWORD, 0x3231);
    rtw_write32_mask(r, REG_AGCTR_B, MASKLWORD, 0x3231);
    rtw_write32_mask(r, REG_CDDTXP, BIT(19) | BIT(18), 0x3);
    rtw_write32_mask(r, REG_TXPSEL, BIT(29) | BIT(28), 0x1);
    rtw_write32_mask(r, REG_TXPSEL, BIT(30), 0x1);
    rtw_write32_mask(r, REG_CDDTXP, 0xFFF00000, 0x001);    /* tx & A */
    rtw_write32_mask(r, REG_ADCINI, 0xF0000000, 0x8);
    rtw_write32_mask(r, REG_TXPSEL1, 0xFFF0, 0x43);         /* neither A nor B alone */
    rtw_write32_mask(r, REG_TXPSEL, MASKBYTE0, (tx << 4) | tx);
    rtw_write32_mask(r, REG_RXDESC, BIT(22), 0x0);
    rtw_write32_mask(r, REG_RXDESC, BIT(18), 0x0);
    rtw_write32_mask(r, REG_ADCINI, 0x0F000000, 0x0);       /* rx & A */
    rtw_write32_mask(r, REG_RXPSEL, MASKBYTE0, (rx << 4) | rx);
    rtw_write32_mask(r, REG_ANTWT, BIT(16), 0x1);
    rtw_write32_mask(r, REG_HTSTFWT, BIT(28), 0x1);
    rtw_write32_mask(r, REG_MRC, BIT(23), 0x1);

    for (n = 100; n; n--) {
        rtw_write_rf(r, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x80000);
        rtw_write_rf(r, RF_PATH_A, RF_LUTWA, RFREG_MASK, 0x00001);
        if (rtw_read_rf(r, RF_PATH_A, 0x33, RFREG_MASK) == 0x00001)
            break;
    }
    if (!n)
        return RTW_STEP_RF_MODE;
    rtw_write_rf(r, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x80000);
    rtw_write_rf(r, RF_PATH_A, RF_LUTWA, RFREG_MASK, 0x00001);
    rtw_write_rf(r, RF_PATH_A, RF_LUTWD1, RFREG_MASK, 0x00034);
    rtw_write_rf(r, RF_PATH_A, RF_LUTWD0, RFREG_MASK, 0x4080C);
    rtw_write_rf(r, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x00000);
    rtw_write_rf(r, RF_PATH_A, RF_LUTWE, RFREG_MASK, 0x00000);

    toggle_igi(r);
    set_channel_cca(r, 1, 0);                   /* channel 1, 20 MHz, as rtw88 */
    set_channel_rfe(r, r->channel);
    return 0;
}

/* rtw8822b_phy_set_param */
u32 rtw_phy_set_param(RTW *r)
{
    u32 cap = r->st->crystal_cap & 0x3F, step, tmp32;
    u8 tmp8;

    /* power on BB/RF domain */
    rtw_write8_set(r, REG_SYS_FUNC_EN, BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST);
    rtw_write8_set(r, REG_RF_CTRL, BIT_RF_EN | BIT_RF_RSTB | BIT_RF_SDM_RSTB);
    rtw_write32_set(r, REG_WLRF1, BIT_WLRF1_BBRF_EN);

    /* pre init before header files config */
    rtw_write32_clr(r, REG_RXPSEL, BIT_RX_PSEL_RST);

    /* rtw_phy_load_tables: no AGC table of a BT path and no RF calibration table
     * on this chip */
    load_table(r, &rtw8822b_mac_tbl, CFG_MAC);
    load_table(r, &rtw8822b_bb_tbl, CFG_BB);
    load_table(r, &rtw8822b_agc_tbl, CFG_AGC);
    load_table(r, &rtw8822b_rf_a_tbl, CFG_RF_A);
    load_table(r, &rtw8822b_rf_b_tbl, CFG_RF_B);

    rtw_write32_mask(r, 0x24, 0x7E000000, cap);
    rtw_write32_mask(r, 0x28, 0x7E, cap);

    /* post init after header files config */
    rtw_write32_set(r, REG_RXPSEL, BIT_RX_PSEL_RST);

    step = config_trx_mode(r);
    if (step)
        return step;

    /* rtw_phy_init: the DIG's starting gain, and adaptivity (EDCCA) */
    r->igi = rtw_read32_mask(r, 0xC50, 0x7F);
    rtw_write32_mask(r, 0x8A4, MASKBYTE0, RTW8822B_EDCCA_MAX);
    rtw_write32_mask(r, 0x8A4, MASKBYTE1, RTW8822B_EDCCA_MAX);
    rtw_write32_clr(r, REG_TX_PTCL_CTRL, BIT_DIS_EDCCA);
    rtw_write32_set(r, REG_RD_CTRL, BIT_EDCCA_MSK_CNTDOWN_EN);
    rtw_write32_mask(r, REG_EDCCA_SOURCE, BIT_SOURCE_OPTION, RTW8822B_EDCCA_SRC_DEF);
    rtw_write32_mask(r, REG_EDCCA_POW_MA, BIT_MA_LEVEL, 0);
    rtw_write32_set(r, REG_EDCCA_DECISION, BIT_EDCCA_OPTION);

    /* rtw8822b_phy_rfe_init: chip top mux, from s0 or s1, input or output */
    rtw_write32_mask(r, 0x64, BIT(29) | BIT(28), 0x3);
    rtw_write32_mask(r, 0x4C, BIT(26) | BIT(25), 0x0);
    /* rtw8822b_coex_cfg_ant_switch, BB software control: the Wi-Fi side drives the
     * antenna switch (DPDT) from RFE pins 8 and 9, which set_channel_rfe and
     * set_channel_cca point at 2.4 or 5 GHz. rtw88 does it from its coex init; left
     * at the power-on 0 the switch is not ours and the access point heard us 7-8 dB
     * weaker on path A at 2.4 GHz. A/B live (2026-11-03, ch 10, 4 streams): out
     * 30 -> 85-96 Mbit/s, the AP's rate MCS 9 -> 13-14. */
    rtw_write32_mask(r, REG_LED_CFG, BIT_DPDT_SEL_EN | BIT_DPDT_WL_SEL, 0x2);
    rtw_write32_mask(r, REG_RFE_CTRL8, 0xFF, DPDT_CTRL_PIN);
    rtw_write32_mask(r, 0x40, BIT(2), 0x1);
    rtw_write32_mask(r, 0x1990, 0x3F, 0x30);
    rtw_write32_mask(r, 0x1990, BIT(11) | BIT(10), 0x3);
    rtw_write32_mask(r, 0x974, 0x3F, 0x3F);
    rtw_write32_mask(r, 0x974, BIT(11) | BIT(10), 0x3);

    /* rtw8822b_phy_bf_init with rtw_bf_phy_init */
    tmp32 = rtw_read32(r, REG_MU_TX_CTL);
    tmp32 |= BIT_MU_P1_WAIT_STATE_EN;
    tmp32 &= ~BIT_MASK_R_MU_RL;
    tmp32 |= (0xAu << BIT_SHIFT_R_MU_RL) & BIT_MASK_R_MU_RL;
    tmp32 &= ~BIT_EN_MU_MIMO;
    tmp32 &= ~BIT_MASK_R_MU_TABLE_VALID;
    rtw_write32(r, REG_MU_TX_CTL, tmp32);
    tmp8 = (u8)(3 << BIT_SHIFT_WMAC_TXMU_ACKPOLICY | BIT_WMAC_TXMU_ACKPOLICY_EN);
    rtw_write8(r, REG_WMAC_MU_BF_OPTION, tmp8);
    rtw_write16(r, REG_WMAC_MU_BF_CTL, 0);
    rtw_write32_set(r, REG_TXBF_CTRL, BIT_USE_NDPA_PARAMETER);
    rtw_write8(r, REG_NDPA_OPT_CTRL, 0x10);
    rtw_write32_mask(r, REG_BBPSF_CTRL, BIT_MASK_CSI_RATE, DESC_RATE6M);
    rtw_write32(r, 0x1C94, 0xAFFFAFFF);
    return 0;
}

/* --- channels ---------------------------------------------------------------------- */

static const u8 low_band[15] = { 0x7, 0x6, 0x6, 0x5, 0x0, 0x0, 0x7, 0xff, 0x6,
                                 0x5, 0x0, 0x0, 0x7, 0x6, 0x6 };
static const u8 middle_band[23] = { 0x6, 0x5, 0x0, 0x0, 0x7, 0x6, 0x6, 0xff, 0x0,
                                    0x0, 0x7, 0x6, 0x6, 0x5, 0x0, 0xff, 0x7, 0x6,
                                    0x6, 0x5, 0x0, 0x0, 0x7 };
static const u8 high_band[15] = { 0x5, 0x5, 0x0, 0x7, 0x7, 0x6, 0x5, 0xff, 0x0,
                                  0x7, 0x7, 0x6, 0x5, 0x5, 0x0 };

/* rtw8822b_set_channel_bb; `bw` 0 20 MHz, 1 40, 2 80. */
static void set_channel_bb(RTW *r, u32 channel, u32 bw, u32 primary)
{
    u32 v;

    if (IS_CH_2G(channel)) {
        rtw_write32_mask(r, REG_RXPSEL, BIT(28), 0x1);
        rtw_write32_mask(r, REG_CCK_CHECK, BIT(7), 0x0);
        rtw_write32_mask(r, REG_ENTXCCK, BIT(18), 0x0);
        rtw_write32_mask(r, REG_RXCCAMSK, 0x0000FC00, 15);
        rtw_write32_mask(r, REG_ACGG2TBL, 0x1F, 0x0);
        rtw_write32_mask(r, REG_CLKTRK, 0x1FFE0000, 0x96A);
        if (channel == 14) {
            rtw_write32_mask(r, REG_TXSF2, MASKDWORD, 0x00006577);
            rtw_write32_mask(r, REG_TXSF6, MASKLWORD, 0x0000);
        } else {
            rtw_write32_mask(r, REG_TXSF2, MASKDWORD, 0x384f6577);
            rtw_write32_mask(r, REG_TXSF6, MASKLWORD, 0x1525);
        }
        rtw_write32_mask(r, REG_RFEINV, 0x300, 0x2);
    } else {
        rtw_write32_mask(r, REG_ENTXCCK, BIT(18), 0x1);
        rtw_write32_mask(r, REG_CCK_CHECK, BIT(7), 0x1);
        rtw_write32_mask(r, REG_RXPSEL, BIT(28), 0x0);
        rtw_write32_mask(r, REG_RXCCAMSK, 0x0000FC00, 34);
        if (IS_CH_5G_1(channel) || IS_CH_5G_2(channel))
            rtw_write32_mask(r, REG_ACGG2TBL, 0x1F, 0x1);
        else if (IS_CH_5G_3(channel))
            rtw_write32_mask(r, REG_ACGG2TBL, 0x1F, 0x2);
        else if (IS_CH_5G_4(channel))
            rtw_write32_mask(r, REG_ACGG2TBL, 0x1F, 0x3);
        if (IS_CH_5G_1(channel))
            rtw_write32_mask(r, REG_CLKTRK, 0x1FFE0000, 0x494);
        else if (IS_CH_5G_2(channel))
            rtw_write32_mask(r, REG_CLKTRK, 0x1FFE0000, 0x453);
        else if (channel >= 100 && channel <= 116)
            rtw_write32_mask(r, REG_CLKTRK, 0x1FFE0000, 0x452);
        else if (channel >= 118 && channel <= 177)
            rtw_write32_mask(r, REG_CLKTRK, 0x1FFE0000, 0x412);
        rtw_write32_mask(r, 0xCBC, 0x300, 0x1);
    }

    v = rtw_read32_mask(r, REG_ADCCLK, MASKDWORD);
    if (bw == 1) {
        if (primary == 1)                       /* RTW_SC_20_UPPER */
            rtw_write32_set(r, REG_RXSB, BIT(4));
        else
            rtw_write32_clr(r, REG_RXSB, BIT(4));
        v = (v & 0xFF3FF300) | ((primary & 0xF) << 2) | 1;
    } else if (bw == 2) {
        v = (v & 0xFCEFCF00) | ((primary & 0xF) << 2) | 2;
    } else {
        v = (v & 0xFFCFFC00) | 0;
    }
    rtw_write32_mask(r, REG_ADCCLK, MASKDWORD, v);
    rtw_write32_mask(r, REG_ADC160, BIT(30), 0x1);
    if (bw == 2 && (r->st->rfe_option == 2 || r->st->rfe_option == 3)) {
        rtw_write32_mask(r, REG_L1PKWT, 0x0000F000, 0x6);
        rtw_write32_mask(r, REG_ADC40, BIT(10), 0x1);
    }
}

/* rtw_set_channel_mac */
static void set_channel_mac(RTW *r, u32 channel, u32 bw, u32 primary)
{
    u32 txsc40 = 0, v;
    u8 v8;

    if (bw == 2)                                /* RTW_SC_40_UPPER 9, RTW_SC_40_LOWER 10 */
        txsc40 = primary == 1 || primary == 3 ? 9 : 10;
    rtw_write8(r, REG_DATA_SC, (u8)(BIT_TXSC_20M(primary) | BIT_TXSC_40M(txsc40)));

    v = rtw_read32(r, REG_WMAC_TRXPTCL_CTL) & ~BIT_RFMOD;
    if (bw == 2)
        v |= BIT_RFMOD_80M;
    else if (bw == 1)
        v |= BIT_RFMOD_40M;
    rtw_write32(r, REG_WMAC_TRXPTCL_CTL, v);

    v = rtw_read32(r, REG_AFE_CTRL1) & ~BIT_MAC_CLK_SEL;
    v |= MAC_CLK_HW_DEF_80M << BIT_SHIFT_MAC_CLK_SEL;
    rtw_write32(r, REG_AFE_CTRL1, v);
    rtw_write8(r, REG_USTIME_TSF, MAC_CLK_SPEED);
    rtw_write8(r, REG_USTIME_EDCA, MAC_CLK_SPEED);

    v8 = rtw_read8(r, REG_CCK_CHECK) & (u8)~BIT_CHECK_CCK_EN;
    if (!IS_CH_2G(channel))
        v8 |= BIT_CHECK_CCK_EN;
    rtw_write8(r, REG_CCK_CHECK, v8);
}

/* rtw8822b_set_channel_rf */
static void set_channel_rf(RTW *r, u32 channel, u32 bw)
{
    u32 rf18 = rtw_read_rf(r, RF_PATH_A, 0x18, RFREG_MASK), be;

    rf18 &= ~((BIT(16) | BIT(9) | BIT(8)) | MASKBYTE0 | (BIT(18) | BIT(17)) |
              (BIT(11) | BIT(10)));
    rf18 |= IS_CH_2G(channel) ? 0 : BIT(16) | BIT(8);
    rf18 |= channel & MASKBYTE0;
    if (channel > 144)
        rf18 |= BIT(18);
    else if (channel >= 80)
        rf18 |= BIT(17);
    rf18 |= bw == 1 ? BIT(11) : bw == 2 ? BIT(10) : BIT(11) | BIT(10);

    if (IS_CH_2G(channel))
        be = 0;
    else if (IS_CH_5G_1(channel) || IS_CH_5G_2(channel))
        be = low_band[(channel - 36) >> 1];
    else if (IS_CH_5G_3(channel))
        be = middle_band[(channel - 100) >> 1];
    else if (IS_CH_5G_4(channel))
        be = high_band[(channel - 149) >> 1];
    else
        return;

    rtw_write_rf(r, RF_PATH_A, RF_MALSEL, BIT(17) | BIT(16) | BIT(15), be);
    rtw_write_rf(r, RF_PATH_A, RF_LUTDBG, BIT(18), channel == 144);
    rtw_write_rf(r, RF_PATH_A, 0x18, RFREG_MASK, rf18);
    rtw_write_rf(r, RF_PATH_B, 0x18, RFREG_MASK, rf18);
    rtw_write_rf(r, RF_PATH_A, RF_XTALX2, BIT(19), 0);
    rtw_write_rf(r, RF_PATH_A, RF_XTALX2, BIT(19), 1);
}

static void set_channel_rxdfir(RTW *r, u32 bw)
{
    if (bw == 1) {
        rtw_write32_mask(r, REG_ACBB0, BIT(29) | BIT(28), 0x1);
        rtw_write32_mask(r, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x0);
        write32s_mask(r, REG_TXDFIR, BIT(31), 0x0);
    } else if (bw == 2) {
        rtw_write32_mask(r, REG_ACBB0, BIT(29) | BIT(28), 0x2);
        rtw_write32_mask(r, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x1);
        write32s_mask(r, REG_TXDFIR, BIT(31), 0x0);
    } else {
        rtw_write32_mask(r, REG_ACBB0, BIT(29) | BIT(28), 0x2);
        rtw_write32_mask(r, REG_ACBBRXFIR, BIT(29) | BIT(28), 0x2);
        write32s_mask(r, REG_TXDFIR, BIT(31), 0x1);
    }
}

/* rtw8822b_set_channel. `bw` 0 is 20 MHz, 1 40, 2 80; `primary` the 20 MHz
 * channel's place in the wider one (rtw88's RTW_SC_*), 0 at 20 MHz. */
/* --- transmit power (phy.c's rtw_phy_set_tx_power_level) ------------------------
 *
 * Each rate's power index is the efuse's calibrated base for the path, the band's
 * channel group and the bandwidth (rtw_phy_get_2g/5g_tx_power_index), plus the
 * rate's offset from the chip's table for its RFE type (rtw8822b_txpwr.c). rtw88
 * then takes the smaller of that offset and the regulatory limit for the channel;
 * here there is no limit (the bench's rule: what the hardware gives). Indices go
 * four to a register, 0x1D00 for path A and 0x1D80 for B
 * (rtw8822b_set_tx_power_index_by_rate). */
#define EFUSE_TXPWR         0x10        /* rtw8822b_efuse.txpwr_idx_table[4] */
#define EFUSE_TXPWR_PATH    42          /* rtw_txpwr_idx: 18 bytes 2G, 24 bytes 5G */
#define EFUSE_TXPWR_5G      18
#define TXAGC_MAX           0x3F        /* max_power_index */
#define RATE_MCS0           0x0C
#define RATE_MCS8           0x14
#define RATE_VHT1SS_MCS0    0x2C
#define RATE_VHT2SS_MCS0    0x36

/* A signed nibble of an efuse byte: the low one is the struct's first bit field. */
static s32 nibble(u8 v, u32 high)
{
    s32 n = high ? v >> 4 : v & 15;

    return n >= 8 ? n - 16 : n;
}

/* rtw_get_channel_group */
static u32 channel_group(u32 ch, u32 rate)
{
    if (ch <= 14)
        return ch <= 2 ? 0 : ch <= 5 ? 1 : ch <= 8 ? 2 : ch <= 11 ? 3 : ch <= 13 ? 4 :
               rate <= 3 ? 5 : 4;
    if (ch <= 42) return 0;
    if (ch <= 50) return 1;
    if (ch <= 58) return 2;
    if (ch <= 64) return 3;
    if (ch <= 106) return 4;
    if (ch <= 114) return 5;
    if (ch <= 122) return 6;
    if (ch <= 130) return 7;
    if (ch <= 138) return 8;
    if (ch <= 144) return 9;
    if (ch <= 155) return 10;
    if (ch <= 161) return 11;
    if (ch <= 171) return 12;
    return 13;
}

static s32 base_index(const u8 *e, u32 ch, u32 bw, u32 rate)
{
    u32 group = channel_group(ch, rate);
    u32 mcs = rate >= RATE_MCS0, two = (rate >= RATE_MCS8 && rate < RATE_VHT1SS_MCS0) ||
              rate >= RATE_VHT2SS_MCS0;
    s32 p;

    if (ch <= 14) {                             /* rtw_phy_get_2g_tx_power_index */
        p = rate <= 3 ? e[group] : e[6 + group];
        if (rate >= 4 && rate < RATE_MCS0)
            p += nibble(e[11], 0);
        if (!mcs)
            return p;
        if (bw == 0)
            p += nibble(e[11], 1) + (two ? nibble(e[12], 0) : 0);
        else if (two)
            p += nibble(e[12], 1);
        return p;
    }
    e += EFUSE_TXPWR_5G;                        /* rtw_phy_get_5g_tx_power_index */
    p = e[group];
    if (!mcs)
        return p + nibble(e[14], 0);
    if (bw == 0)
        return p + nibble(e[14], 1) + (two ? nibble(e[15], 0) : 0);
    if (bw == 1)
        return p + (two ? nibble(e[15], 1) : 0);
    /* 80 MHz: the average of the two 40 MHz halves */
    p = (e[group] + e[group + 1 < 14 ? group + 1 : group]) / 2;
    return p + nibble(e[20], 1) + (two ? nibble(e[21], 1) : 0);
}

static void set_tx_power(RTW *r, u32 ch, u32 bw)
{
    static const u8 words[] = { 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18,
                                0x2C, 0x30, 0x34, 0x38, 0x3C };
    s32 type = r->st->rfe_option < RTW8822B_RFE_MAX ?
               rtw8822b_txpwr_type_of_rfe[r->st->rfe_option] : -1;
    u32 path, w, i, rate, word;
    s32 p;

    if (type < 0)
        type = 0;
    for (path = 0; path < 2; path++) {
        const u8 *e = r->efuse + EFUSE_TXPWR + path * EFUSE_TXPWR_PATH;
        const s8 *offset = rtw8822b_txpwr_by_rate[type][ch > 14][path];

        for (w = ch > 14 ? 1 : 0; w < sizeof(words); w++) {
            word = 0;
            for (i = 0; i < 4; i++) {
                rate = words[w] + i;
                p = base_index(e, ch, bw, rate) + offset[rate];
                p = p < 0 ? 0 : p > TXAGC_MAX ? TXAGC_MAX : p;
                word |= (u32)p << (i * 8);
                if (path == 0 && rate == 0x04)
                    r->st->txpwr_ofdm6 = (u32)p;
                if (path == 0 && rate == RATE_VHT2SS_MCS0 + 7)
                    r->st->txpwr_vht2_mcs7 = (u32)p;
            }
            rtw_write32(r, (path ? 0x1D80 : 0x1D00) + words[w], word);
        }
    }
}

/* rtw8822b_do_iqk: the firmware calibrates the paths' I/Q on the channel set, and
 * says it is done by writing 0xABCDE to RF 0x08. Thread. */
void rtw_iqk(RTW *r)
{
    u32 i;

    rtw_fw_iqk(r);
    for (i = 0; i < 300; i++) {
        if (rtw_read_rf(r, RF_PATH_A, RF_DTXLOK, RFREG_MASK) == 0xABCDE)
            break;
        usb_sleep(20);
    }
    rtw_write_rf(r, RF_PATH_A, RF_DTXLOK, RFREG_MASK, 0x0);
    r->st->iqk_ms = i * 20;
    r->st->iqk_runs++;
}

/* rtw_phy_dig, linked: the receive gain index (IGI) from the false alarms of the
 * last 2 s, bounded by the access point's RSSI (100 + dBm): no lower than it up
 * to 0x40 and no higher than 15 above. The tables leave 0x20, the most sensitive,
 * where the radio takes noise for frames, holds its own sends for a busy channel
 * and loses the access point's acknowledgements. A/B live (2026-11-03, 2.4 GHz
 * ch 10, the AP at -11 dBm, 4 streams): 0x20 out 26 / in 8 Mbit/s, 0x30-0x5a
 * out 146-160 / in 143-152 Mbit/s; rtw88 sat at 0x44 there. Thread, every 2 s. */
#define DIG_PERF_FA_TH_LOW          250
#define DIG_PERF_FA_TH_HIGH         500
#define DIG_PERF_FA_TH_EXTRA_HIGH   750
#define DIG_PERF_MAX                0x5A
#define DIG_PERF_MID                0x40
#define DIG_MIN                     0x1C        /* rtw8822b's dig_min */
#define DIG_RSSI_GAIN_OFFSET        15

static void dig_write(RTW *r, u32 igi)
{
    rtw_write32_mask(r, REG_RXIGI_A, 0x7F, igi);
    rtw_write32_mask(r, REG_RXIGI_B, 0x7F, igi);
}

/* rtw8822b_false_alarm_statistics: OFDM and, when on, CCK false alarms since the
 * last call, then the counters reset. */
static u32 false_alarms(RTW *r)
{
    u32 fa = rtw_read16(r, 0xF48);

    if (rtw_read32(r, 0x808) & BIT(28))
        fa += rtw_read16(r, 0xA5C);
    rtw_write32_set(r, 0x9A4, BIT(17));
    rtw_write32_clr(r, 0x9A4, BIT(17));
    rtw_write32_clr(r, 0xA2C, BIT(15));
    rtw_write32_set(r, 0xA2C, BIT(15));
    rtw_write32_set(r, 0xB58, BIT(0));
    rtw_write32_clr(r, 0xB58, BIT(0));
    return fa;
}

void rtw_dig(RTW *r)
{
    s32 dbm = r->ap_signal_x8 / 8;
    u32 rssi = dbm <= -100 ? 0 : dbm >= 0 ? 100 : (u32)(100 + dbm);
    u32 fa = false_alarms(r), igi = r->dig_igi ? r->dig_igi : r->igi;
    u32 min_rssi = rssi > DIG_MIN ? rssi : DIG_MIN;
    u32 max = min_rssi + DIG_RSSI_GAIN_OFFSET < DIG_PERF_MAX ?
              min_rssi + DIG_RSSI_GAIN_OFFSET : DIG_PERF_MAX;
    u32 lower = min_rssi < DIG_PERF_MID ? min_rssi : DIG_PERF_MID;
    u32 upper = lower + DIG_RSSI_GAIN_OFFSET;

    if (upper > max)
        upper = max;
    if (fa > DIG_PERF_FA_TH_EXTRA_HIGH)
        igi += 4;
    else if (fa > DIG_PERF_FA_TH_HIGH)
        igi += 3;
    else if (fa > DIG_PERF_FA_TH_LOW)
        igi += 2;
    igi -= 2;
    igi = igi < lower ? lower : igi > upper ? upper : igi;
    if (igi != r->dig_igi)
        dig_write(r, igi);
    r->dig_igi = igi;
    r->st->dig_igi = igi;
    r->st->dig_fa = fa;
    r->st->dig_rssi = rssi;
}

/* Unlinked (scanning): back to the tables' gain, as rtw88's coverage range. */
void rtw_dig_reset(RTW *r)
{
    if (r->dig_igi)
        dig_write(r, r->igi);
    r->dig_igi = 0;
    r->dig_ticks = 0;
    r->ap_signal_x8 = -60 * 8;
    r->st->dig_igi = r->igi;
}

void rtw_set_channel(RTW *r, u32 channel, u32 bw, u32 primary)
{
    set_channel_bb(r, channel, bw, primary);
    set_channel_mac(r, channel, bw, primary);
    set_channel_rf(r, channel, bw);
    set_channel_rxdfir(r, bw);
    toggle_igi(r);
    set_channel_cca(r, channel, bw);
    set_channel_rfe(r, channel);
    set_tx_power(r, channel, bw);
    r->channel = channel;
    r->st->channel = channel;
}
