/* The RTL8822B's MAC: power on and off, and its firmware, as rtw88's mac.c does it
 * for a chip with the 3081 CPU over USB (rtw_mac_power_on, __rtw_download_firmware).
 */

#include "rtw.h"
#include "bench.h"
#include "file.h"
#include "rtw8822b_defs.h"

#define PWR_POLL_READS      2000    /* RTW_PWR_POLLING_CNT reads 50 us apart: 1 s */
#define RTW_PWR_CMD_WRITE   0x01
#define RTW_PWR_CMD_POLLING 0x02
#define RTW_PWR_CMD_DELAY   0x03
#define RTW_PWR_CMD_END     0x04
#define RTW_PWR_INTF_USB    0x02
#define RTW_PWR_DELAY_US    0

#define FW_HDR_SIZE         64
#define FW_HDR_CHKSUM_SIZE  8
#define FW_PKT_MAX          0x1000      /* download_firmware_to_mem's max_size */
#define RTW_DMA_MAPPING_HIGH 3
#define BIT_H2CQ_FULL_      BIT(31)

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

/* --- power ---------------------------------------------------------------------- */

/* rtw_mac_pre_system_cfg, the USB case. */
static void pre_system_cfg(RTW *r)
{
    u32 v;

    rtw_write8(r, REG_RSV_CTRL, 0);

    /* config PIN Mux */
    v = rtw_read32(r, REG_PAD_CTRL1);
    rtw_write32(r, REG_PAD_CTRL1, v | BIT_PAPE_WLBT_SEL | BIT_LNAON_WLBT_SEL);
    v = rtw_read32(r, REG_LED_CFG);
    rtw_write32(r, REG_LED_CFG, v & ~(BIT_PAPE_SEL_EN | BIT_LNAON_SEL_EN));
    v = rtw_read32(r, REG_GPIO_MUXCFG);
    rtw_write32(r, REG_GPIO_MUXCFG, v | BIT_WLRFE_4_5_EN);

    /* disable BB/RF */
    rtw_write8(r, REG_SYS_FUNC_EN,
               rtw_read8(r, REG_SYS_FUNC_EN) & (u8)~(BIT_FEN_BB_RSTB | BIT_FEN_BB_GLB_RST));
    rtw_write8(r, REG_RF_CTRL,
               rtw_read8(r, REG_RF_CTRL) & (u8)~(BIT_RF_SDM_RSTB | BIT_RF_RSTB | BIT_RF_EN));
    rtw_write32(r, REG_WLRF1, rtw_read32(r, REG_WLRF1) & ~BIT_WLRF1_BBRF_EN);
}

/* rtw_sub_pwr_seq_parser, for USB and this chip's cut. */
static u32 run_pwr_seq(RTW *r, const RTW_PWR_CMD *cmd)
{
    u32 cut_mask = 1u << (r->cut + 1), reads;
    u8 v;

    for (; cmd->cmd != RTW_PWR_CMD_END; cmd++) {
        if (!(cmd->intf_mask & RTW_PWR_INTF_USB) || !(cmd->cut_mask & cut_mask))
            continue;
        switch (cmd->cmd) {
        case RTW_PWR_CMD_WRITE:
            v = rtw_read8(r, cmd->offset);
            v = (u8)((v & ~cmd->mask) | (cmd->value & cmd->mask));
            rtw_write8(r, cmd->offset, v);
            break;
        case RTW_PWR_CMD_POLLING:
            for (reads = 0; reads < PWR_POLL_READS; reads++)
                if ((rtw_read8(r, cmd->offset) & cmd->mask) == (cmd->value & cmd->mask))
                    break;
            if (reads == PWR_POLL_READS) {
                r->st->poll_timeouts++;
                r->st->last_poll_address = cmd->offset;
                return STATUS_UNSUCCESSFUL;
            }
            break;
        case RTW_PWR_CMD_DELAY:
            /* A register access is itself longer than the microseconds asked. */
            if (cmd->value != RTW_PWR_DELAY_US)
                usb_sleep(cmd->offset);
            break;
        default:
            break;
        }
    }
    return 0;
}

static u32 run_flow(RTW *r, const RTW_PWR_CMD *const *flow)
{
    u32 status;

    for (; *flow; flow++)
        if ((status = run_pwr_seq(r, *flow)) != 0)
            return status;
    return 0;
}

/* rtw_mac_power_switch: 1 if the chip already was where it was asked to go. */
static u32 power_switch(RTW *r, u32 on, u32 *already)
{
    u32 status, powered;

    if (rtw_read8(r, REG_CR) == 0xEA)
        powered = 0;
    else if (rtw_read8(r, REG_SYS_STATUS1 + 1) & BIT(0))
        powered = 0;
    else
        powered = 1;
    *already = on == powered;
    if (*already)
        return 0;
    status = run_flow(r, on ? card_enable_flow_8822b : card_disable_flow_8822b);
    if (on)
        rtw_write8_clr(r, REG_SYS_STATUS1 + 1, BIT(0));
    return status;
}

/* __rtw_mac_init_system_cfg. */
static void init_system_cfg(RTW *r)
{
    u32 v, tmp;

    v = rtw_read32(r, REG_CPU_DMEM_CON);
    rtw_write32(r, REG_CPU_DMEM_CON, v | BIT_WL_PLATFORM_RST | BIT_DDMA_EN);
    rtw_write8_set(r, REG_SYS_FUNC_EN + 1, 0xDC);       /* rtw8822b sys_func_en */
    rtw_write8(r, REG_CR_EXT + 3, (u8)((rtw_read8(r, REG_CR_EXT + 3) & 0xF0) | 0x0C));

    /* disable boot-from-flash for driver's DL FW */
    tmp = rtw_read32(r, REG_MCUFW_CTRL);
    if (tmp & BIT_BOOT_FSPI_EN) {
        rtw_write32(r, REG_MCUFW_CTRL, tmp & ~BIT_BOOT_FSPI_EN);
        rtw_write32(r, REG_GPIO_MUXCFG, rtw_read32(r, REG_GPIO_MUXCFG) & ~BIT_FSPI_EN);
    }
}

u32 rtw_mac_power_on(RTW *r)
{
    u32 status, already;

    pre_system_cfg(r);
    status = power_switch(r, 1, &already);
    if (!status && already) {
        /* Left on by whoever had it before: off and on again. */
        power_switch(r, 0, &already);
        pre_system_cfg(r);
        status = power_switch(r, 1, &already);
    }
    if (status)
        return status;
    init_system_cfg(r);
    return 0;
}

void rtw_mac_power_off(RTW *r)
{
    u32 already;

    power_switch(r, 0, &already);
}

/* --- firmware ------------------------------------------------------------------- */

static void wlan_cpu_enable(RTW *r, u32 enable)
{
    if (enable) {
        rtw_write8_set(r, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
        rtw_write8_set(r, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
    } else {
        rtw_write8_clr(r, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
        rtw_write8_clr(r, REG_RSV_CTRL + 1, BIT_WLMCU_IOIF);
    }
}

typedef struct {
    u32 len;
    u32 reg;
    u32 val;
} BACKUP;

#define BACKUPS 6

/* download_firmware_reg_backup */
static void reg_backup(RTW *r, BACKUP *b)
{
    u8 tmp;

    b[0].len = 1;
    b[0].reg = REG_TXDMA_PQ_MAP + 1;
    b[0].val = rtw_read8(r, REG_TXDMA_PQ_MAP + 1);
    rtw_write8(r, REG_TXDMA_PQ_MAP + 1, RTW_DMA_MAPPING_HIGH << 6);

    b[1].len = 1;
    b[1].reg = REG_CR;
    b[1].val = rtw_read8(r, REG_CR);
    b[2].len = 4;
    b[2].reg = REG_H2CQ_CSR;
    b[2].val = BIT_H2CQ_FULL_;
    rtw_write8(r, REG_CR, BIT_HCI_TXDMA_EN | BIT_TXDMA_EN);
    rtw_write32(r, REG_H2CQ_CSR, BIT_H2CQ_FULL_);

    b[3].len = 2;
    b[3].reg = REG_FIFOPAGE_INFO_1;
    b[3].val = rtw_read16(r, REG_FIFOPAGE_INFO_1);
    b[4].len = 4;
    b[4].reg = REG_RQPN_CTRL_2;
    b[4].val = rtw_read32(r, REG_RQPN_CTRL_2) | BIT_LD_RQPN;
    rtw_write16(r, REG_FIFOPAGE_INFO_1, 0x200);
    rtw_write32(r, REG_RQPN_CTRL_2, b[4].val);

    tmp = rtw_read8(r, REG_BCN_CTRL);
    b[5].len = 1;
    b[5].reg = REG_BCN_CTRL;
    b[5].val = tmp;
    rtw_write8(r, REG_BCN_CTRL, (u8)((tmp & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT));
}

static void reg_restore(RTW *r, const BACKUP *b)
{
    u32 i;

    for (i = 0; i < BACKUPS; i++) {
        if (b[i].len == 1)
            rtw_write8(r, b[i].reg, (u8)b[i].val);
        else if (b[i].len == 2)
            rtw_write16(r, b[i].reg, (u16)b[i].val);
        else
            rtw_write32(r, b[i].reg, b[i].val);
    }
}

static void reset_platform(RTW *r)
{
    rtw_write8_clr(r, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
    rtw_write8_clr(r, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
    rtw_write8_set(r, REG_CPU_DMEM_CON + 2, BIT_WL_PLATFORM_RST >> 16);
    rtw_write8_set(r, REG_SYS_CLK_CTRL + 1, BIT_CPU_CLK_EN >> 8);
}

/* The TX descriptor's first 32 bytes XORed as 16-bit words, into word 7
 * (fill_txdesc_checksum_common, 16 words on the 8822B). */
static void desc_checksum(u8 *d)
{
    u16 sum = 0;
    u32 i;

    d[28] = d[29] = 0;
    for (i = 0; i < 32; i += 2)
        sum ^= (u16)(d[i] | d[i + 1] << 8);
    d[28] = (u8)sum;
    d[29] = (u8)(sum >> 8);
}

/* rtw_fw_write_data_rsvd_page with rtw_usb_write_data_rsvd_page: one block to the
 * reserved page at `page`, through the beacon queue. */
static u32 write_rsvd_page(RTW *r, u32 page, const u8 *data, u32 size)
{
    u8 *b = r->bulk_buf, bcn_ctrl, cr1;
    u32 i, status;

    bcn_ctrl = rtw_read8(r, REG_BCN_CTRL);
    rtw_write16(r, REG_FIFOPAGE_CTRL_2, (u16)((page & BIT_MASK_BCN_HEAD_1_V1) | BIT_BCN_VALID_V1));
    cr1 = rtw_read8(r, REG_CR + 1);
    rtw_write8(r, REG_CR + 1, (u8)(cr1 | BIT_ENSWBCN >> 8));
    rtw_write8(r, REG_BCN_CTRL, (u8)((bcn_ctrl & ~BIT_EN_BCN_FUNCTION) | BIT_DIS_TSF_UDT));

    for (i = 0; i < RTW_TX_DESC_SIZE; i++)
        b[i] = 0;
    b[0] = (u8)size;                                /* w0: TXPKTSIZE */
    b[1] = (u8)(size >> 8);
    b[2] = RTW_TX_DESC_SIZE;                        /* OFFSET */
    b[3] = 1u << (26 - 24);                         /* LS */
    b[5] = RTW_QSEL_BEACON;                         /* w1: QSEL, bits 12:8 */
    desc_checksum(b);
    for (i = 0; i < size; i++)
        b[RTW_TX_DESC_SIZE + i] = data[i];
    status = rtw_bulk_out(r, RTW_EP_HIGH, b, RTW_TX_DESC_SIZE + size);

    if (NT_SUCCESS(status) && !rtw_check_hw_ready(r, REG_FIFOPAGE_CTRL_2, BIT_BCN_VALID_V1, 1))
        status = STATUS_UNSUCCESSFUL;

    /* restore: the reserved page head back at the boundary, beacons as they were */
    rtw_write16(r, REG_FIFOPAGE_CTRL_2, (u16)(r->rsvd_boundary | BIT_BCN_VALID_V1));
    rtw_write8(r, REG_BCN_CTRL, bcn_ctrl);
    rtw_write8(r, REG_CR + 1, cr1);
    return status;
}

/* iddma_download_firmware */
static u32 iddma(RTW *r, u32 src, u32 dst, u32 len, u32 first)
{
    u32 ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN | (len & BIT_MASK_DDMACH0_DLEN);

    if (!rtw_check_hw_ready(r, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
        return STATUS_UNSUCCESSFUL;
    if (!first)
        ctrl |= BIT_DDMACH0_CHKSUM_CONT;
    rtw_write32(r, REG_DDMA_CH0SA, src);
    rtw_write32(r, REG_DDMA_CH0DA, dst);
    rtw_write32(r, REG_DDMA_CH0CTRL, ctrl);
    if (!rtw_check_hw_ready(r, REG_DDMA_CH0CTRL, BIT_DDMACH0_OWN, 0))
        return STATUS_UNSUCCESSFUL;
    return 0;
}

/* check_fw_checksum */
static u32 checksum_ok(RTW *r, u32 dst)
{
    u8 ctrl = rtw_read8(r, REG_MCUFW_CTRL);
    u32 bad = rtw_read32(r, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS;

    if (dst < OCPBASE_DMEM_88XX)
        ctrl = (u8)(bad ? (ctrl | BIT_IMEM_DW_OK) & ~BIT_IMEM_CHKSUM_OK
                        : ctrl | BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK);
    else
        ctrl = (u8)(bad ? (ctrl | BIT_DMEM_DW_OK) & ~BIT_DMEM_CHKSUM_OK
                        : ctrl | BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK);
    rtw_write8(r, REG_MCUFW_CTRL, ctrl);
    return !bad;
}

/* download_firmware_to_mem: through the reserved page in blocks of 4 KB, each
 * copied by the chip's DMA to where it belongs. */
static u32 to_mem(RTW *r, const u8 *data, u32 dst, u32 size)
{
    u32 offset = 0, first = 1, n, pkt, status;

    rtw_write32(r, REG_DDMA_CH0CTRL,
                rtw_read32(r, REG_DDMA_CH0CTRL) | BIT_DDMACH0_RESET_CHKSUM_STS);
    while (offset < size) {
        n = size - offset > FW_PKT_MAX ? FW_PKT_MAX : size - offset;
        /* send_firmware_pkt: a transfer of a whole number of 512-byte packets
         * would need a zero-length one after it; one byte more avoids that. */
        pkt = !((n + RTW_TX_DESC_SIZE) & 511) ? n + 1 : n;
        status = write_rsvd_page(r, 0, data + offset, pkt);
        if (status)
            return status;
        status = iddma(r, OCPBASE_TXBUF_88XX + RTW_TX_DESC_SIZE, dst + offset, n, first);
        if (status)
            return status;
        first = 0;
        offset += n;
    }
    return checksum_ok(r, dst) ? 0 : STATUS_UNSUCCESSFUL;
}

/* start_download_firmware: DMEM, IMEM, then EMEM if the header has one. */
static u32 start_download(RTW *r, const u8 *fw)
{
    u32 dmem = le32(fw + 0x24) + FW_HDR_CHKSUM_SIZE;
    u32 imem = le32(fw + 0x30) + FW_HDR_CHKSUM_SIZE;
    u32 emem = fw[0x18] & BIT(4) ? le32(fw + 0x34) + FW_HDR_CHKSUM_SIZE : 0;
    u32 status;
    u16 v;

    v = (u16)(rtw_read16(r, REG_MCUFW_CTRL) & 0x3800);
    rtw_write16(r, REG_MCUFW_CTRL, (u16)(v | BIT_MCUFWDL_EN));
    status = to_mem(r, fw + FW_HDR_SIZE, le32(fw + 0x20) & ~BIT(31), dmem);
    if (!status)
        status = to_mem(r, fw + FW_HDR_SIZE + dmem, le32(fw + 0x3C) & ~BIT(31), imem);
    if (!status && emem)
        status = to_mem(r, fw + FW_HDR_SIZE + dmem + imem, le32(fw + 0x38) & ~BIT(31), emem);
    return status;
}

static u32 size_ok(const u8 *fw, u32 size)
{
    u32 dmem = le32(fw + 0x24) + FW_HDR_CHKSUM_SIZE;
    u32 imem = le32(fw + 0x30) + FW_HDR_CHKSUM_SIZE;
    u32 emem = fw[0x18] & BIT(4) ? le32(fw + 0x34) + FW_HDR_CHKSUM_SIZE : 0;

    return size > FW_HDR_SIZE && FW_HDR_SIZE + dmem + imem + emem == size;
}

static u32 load_file(RTW *r)
{
    void *f = file_open(RTW_FW_FILE);
    u32 n, got = 0;

    if (!f)
        return 0;
    while (got < RTW_FW_MAX && (n = file_read(f, r->fw + got, RTW_FW_MAX - got, got)) != 0)
        got += n;
    file_close(f);
    return got;
}

/* __rtw_download_firmware. The image comes from firmware\\rtw88\\rtw8822b_fw.bin beside the plugin;
 * it stays in memory for a later bring-up. */
u32 rtw_download_firmware(RTW *r)
{
    BACKUP bckp[BACKUPS];
    u32 status;

    if (!r->fw_size) {
        r->fw_size = load_file(r);
        r->st->fw_size = r->fw_size;
        if (!r->fw_size || !size_ok(r->fw, r->fw_size)) {
            r->fw_size = 0;
            return RTW_STEP_FW_FILE;
        }
        r->st->fw_version = (u32)(r->fw[4] | r->fw[5] << 8) << 16 | (u32)r->fw[6] << 8 |
                            r->fw[7];
        r->st->fw_h2c_version = (u32)(r->fw[0x1C] | r->fw[0x1D] << 8);
    }

    wlan_cpu_enable(r, 0);
    reg_backup(r, bckp);
    reset_platform(r);
    status = start_download(r, r->fw);
    if (status) {
        rtw_write8_clr(r, REG_MCUFW_CTRL, BIT_MCUFWDL_EN);
        rtw_write8_set(r, REG_SYS_FUNC_EN + 1, BIT_FEN_CPUEN);
        return RTW_STEP_FW_DOWNLOAD;
    }
    reg_restore(r, bckp);

    /* download_firmware_end_flow */
    rtw_write32(r, REG_TXDMA_STATUS, BTI_PAGE_OVF);
    {
        u16 ctrl = rtw_read16(r, REG_MCUFW_CTRL);

        if ((ctrl & BIT_CHECK_SUM_OK) == BIT_CHECK_SUM_OK)
            rtw_write16(r, REG_MCUFW_CTRL, (u16)((ctrl | BIT_FW_DW_RDY) & ~BIT_MCUFWDL_EN));
    }
    wlan_cpu_enable(r, 1);

    /* download_firmware_validate */
    if (!rtw_check_hw_ready(r, REG_MCUFW_CTRL, FW_READY_MASK, FW_READY)) {
        r->st->fw_ctrl = rtw_read16(r, REG_MCUFW_CTRL);
        return RTW_STEP_FW_READY;
    }
    r->st->fw_ctrl = rtw_read16(r, REG_MCUFW_CTRL);
    return 0;
}

/* --- after the firmware: rtw_mac_init ------------------------------------------- */

/* The 8822B over three bulk OUT endpoints: rqpn_table_8822b[3] and
 * page_table_8822b[3]. */
#define MAP_LOW             1           /* enum rtw_dma_mapping: EXTRA 0, LOW 1, */
#define MAP_NORMAL          2           /* NORMAL 2, HIGH 3 */
#define MAP_HIGH            3
#define TXFF_PAGES          (262144 / 128)
#define RSVD_PAGES          (8 + RSVD_PG_H2C_EXTRAINFO_NUM + RSVD_PG_H2C_STATICINFO_NUM + \
                             RSVD_PG_H2CQ_NUM + RSVD_PG_CPU_INSTRUCTION_NUM + \
                             RSVD_PG_FW_TXBUF_NUM)
#define PAGES_HQ            64
#define PAGES_NQ            64
#define PAGES_LQ            64
#define PAGES_EXQ           0
#define PAGES_GAP           1
#define RXFF_SIZE           24576
#define C2H_PKT_BUF         256
#define USB_TX_AGG_DESC     3

/* txdma_queue_mapping */
static void txdma_queue_mapping(RTW *r)
{
    u16 map = (u16)(BIT_TXDMA_HIQ_MAP(MAP_HIGH) | BIT_TXDMA_MGQ_MAP(MAP_HIGH) |
                    BIT_TXDMA_BKQ_MAP(MAP_LOW) | BIT_TXDMA_BEQ_MAP(MAP_LOW) |
                    BIT_TXDMA_VIQ_MAP(MAP_NORMAL) | BIT_TXDMA_VOQ_MAP(MAP_NORMAL));

    rtw_write16(r, REG_TXDMA_PQ_MAP, map);
    rtw_write8(r, REG_CR, 0);
    rtw_write8(r, REG_CR, MAC_TRX_ENABLE);
    rtw_write32(r, REG_H2CQ_CSR, BIT_H2CQ_FULL_);
    rtw_write8_set(r, REG_TXDMA_PQ_MAP, BIT_RXDMA_ARBBW_EN);
}

/* rtw_set_trx_fifo_info and __priority_queue_cfg */
static u32 priority_queue_cfg(RTW *r)
{
    u32 pubq = TXFF_PAGES - RSVD_PAGES - PAGES_HQ - PAGES_LQ - PAGES_NQ - PAGES_EXQ -
               PAGES_GAP;

    r->rsvd_boundary = TXFF_PAGES - RSVD_PAGES;
    r->rsvd_h2cq_addr = TXFF_PAGES - RSVD_PG_FW_TXBUF_NUM - RSVD_PG_CPU_INSTRUCTION_NUM -
                        RSVD_PG_H2CQ_NUM;

    rtw_write16(r, REG_FIFOPAGE_INFO_1, PAGES_HQ);
    rtw_write16(r, REG_FIFOPAGE_INFO_2, PAGES_LQ);
    rtw_write16(r, REG_FIFOPAGE_INFO_3, PAGES_NQ);
    rtw_write16(r, REG_FIFOPAGE_INFO_4, PAGES_EXQ);
    rtw_write16(r, REG_FIFOPAGE_INFO_5, (u16)pubq);
    rtw_write32_set(r, REG_RQPN_CTRL_2, BIT_LD_RQPN);

    rtw_write16(r, REG_FIFOPAGE_CTRL_2, (u16)r->rsvd_boundary);
    rtw_write8_set(r, REG_FWHW_TXQ_CTRL + 2, BIT_EN_WR_FREE_TAIL >> 16);
    rtw_write16(r, REG_BCNQ_BDNY_V1, (u16)r->rsvd_boundary);
    rtw_write16(r, REG_FIFOPAGE_CTRL_2 + 2, (u16)r->rsvd_boundary);
    rtw_write16(r, REG_BCNQ1_BDNY_V1, (u16)r->rsvd_boundary);
    rtw_write32(r, REG_RXFF_BNDY, RXFF_SIZE - C2H_PKT_BUF - 1);

    rtw_write8_mask(r, REG_AUTO_LLT_V1, BIT_MASK_BLK_DESC_NUM, USB_TX_AGG_DESC);
    rtw_write8(r, REG_AUTO_LLT_V1 + 3, USB_TX_AGG_DESC);
    rtw_write8_set(r, REG_TXDMA_OFFSET_CHK + 1, BIT(1));

    rtw_write8_set(r, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1);
    if (!rtw_check_hw_ready(r, REG_AUTO_LLT_V1, BIT_AUTO_INIT_LLT_V1, 0))
        return RTW_STEP_LLT;
    rtw_write8(r, REG_CR + 3, 0);
    return 0;
}

/* init_h2c: the firmware's command queue in the reserved pages. */
static u32 init_h2c(RTW *r)
{
    u32 addr = r->rsvd_h2cq_addr << 7, size = RSVD_PG_H2CQ_NUM << 7, v, wp, rp, free;

    v = rtw_read32(r, REG_H2C_HEAD);
    rtw_write32(r, REG_H2C_HEAD, (v & 0xFFFC0000) | addr);
    v = rtw_read32(r, REG_H2C_READ_ADDR);
    rtw_write32(r, REG_H2C_READ_ADDR, (v & 0xFFFC0000) | addr);
    v = rtw_read32(r, REG_H2C_TAIL);
    rtw_write32(r, REG_H2C_TAIL, (v & 0xFFFC0000) | (addr + size));
    rtw_write8(r, REG_H2C_INFO, (u8)((rtw_read8(r, REG_H2C_INFO) & 0xFC) | 0x01));
    rtw_write8(r, REG_H2C_INFO, (u8)((rtw_read8(r, REG_H2C_INFO) & 0xFB) | 0x04));
    rtw_write8(r, REG_TXDMA_OFFSET_CHK + 1,
               (u8)((rtw_read8(r, REG_TXDMA_OFFSET_CHK + 1) & 0x7F) | 0x80));

    wp = rtw_read32(r, REG_H2C_PKT_WRITEADDR) & 0x3FFFF;
    rp = rtw_read32(r, REG_H2C_PKT_READADDR) & 0x3FFFF;
    free = wp >= rp ? size - (wp - rp) : rp - wp;
    return free == size ? 0 : RTW_STEP_H2C_QUEUE;
}

/* rtw8822b_mac_init */
static void mac_init_8822b(RTW *r)
{
    /* protocol configuration */
    rtw_write8_clr(r, REG_SW_AMPDU_BURST_MODE_CTRL, BIT_PRE_TX_CMD);
    rtw_write8(r, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
    rtw_write8_set(r, REG_TX_HANG_CTRL, BIT_EN_EOF_V1);
    rtw_write32(r, REG_PROT_MODE_CTRL, WLAN_RTS_LEN_TH | (WLAN_RTS_TX_TIME_TH << 8) |
                (WLAN_MAX_AGG_PKT_LIMIT << 16) | (WLAN_RTS_MAX_AGG_PKT_LIMIT << 24));
    rtw_write16(r, REG_BAR_MODE_CTRL + 2,
                WLAN_BAR_RETRY_LIMIT | WLAN_RA_TRY_RATE_AGG_LIMIT << 8);
    rtw_write8(r, REG_FAST_EDCA_VOVI_SETTING, FAST_EDCA_VO_TH);
    rtw_write8(r, REG_FAST_EDCA_VOVI_SETTING + 2, FAST_EDCA_VI_TH);
    rtw_write8(r, REG_FAST_EDCA_BEBK_SETTING, FAST_EDCA_BE_TH);
    rtw_write8(r, REG_FAST_EDCA_BEBK_SETTING + 2, FAST_EDCA_BK_TH);
    /* EDCA configuration */
    rtw_write8_clr(r, REG_TIMER0_SRC_SEL, BIT_TSFT_SEL_TIMER0);
    rtw_write16(r, REG_TXPAUSE, 0x0000);
    rtw_write8(r, REG_SLOT, WLAN_SLOT_TIME);
    rtw_write8(r, REG_PIFS, WLAN_PIFS_TIME);
    rtw_write32(r, REG_SIFS, WLAN_SIFS_CFG);
    rtw_write16(r, REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
    rtw_write16(r, REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
    rtw_write32(r, REG_RD_NAV_NXT, WLAN_NAV_CFG);
    rtw_write16(r, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
    /* beacon control: TSF and the functions around it */
    rtw_write8_set(r, REG_BCN_CTRL, BIT_EN_BCN_FUNCTION);
    rtw_write32(r, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
    rtw_write8(r, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
    rtw_write8(r, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
    rtw_write8_clr(r, REG_TX_PTCL_CTRL + 1, BIT_SIFS_BK_EN >> 8);
    /* WMAC configuration */
    rtw_write16(r, REG_RXFLTMAP0, WLAN_RX_FILTER0);
    rtw_write16(r, REG_RXFLTMAP1, WLAN_RX_FILTER1);
    rtw_write16(r, REG_RXFLTMAP2, WLAN_RX_FILTER2);
    rtw_write32(r, REG_RCR, WLAN_RCR_CFG);
    rtw_write8(r, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
    rtw_write8(r, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
    rtw_write8(r, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
    rtw_write32(r, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
    rtw_write8(r, REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);
    rtw_write8_set(r, REG_SND_PTCL_CTRL, BIT_DIS_CHK_VHTSIGB_CRC);
}

u32 rtw_mac_init(RTW *r)
{
    u32 step;
    u8 v;

    /* rtw_init_trx_cfg */
    txdma_queue_mapping(r);
    step = priority_queue_cfg(r);
    if (!step)
        step = init_h2c(r);
    if (step)
        return step;

    mac_init_8822b(r);

    /* rtw_drv_info_cfg */
    rtw_write8(r, REG_RX_DRVINFO_SZ, PHY_STATUS_SIZE);
    v = rtw_read8(r, REG_TRXFF_BNDY + 1);
    rtw_write8(r, REG_TRXFF_BNDY + 1, (u8)((v & 0xF0) | 0xF));    /* rxdesc len = 0 issue */
    rtw_write32_set(r, REG_RCR, BIT_APP_PHYSTS);
    rtw_write32_clr(r, REG_WMAC_OPTION_FUNCTION + 4, BIT(8) | BIT(9));

    /* rtw_usb_interface_cfg: a high-speed bus */
    rtw_write8(r, REG_RXDMA_MODE, (u8)(BIT_DMA_BURST_CNT | BIT_DMA_MODE |
                                       (BIT_DMA_BURST_SIZE_512 << 4)));
    rtw_write16_set(r, REG_TXDMA_OFFSET_CHK, BIT_DROP_DATA_EN);
    return 0;
}
