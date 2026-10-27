/* Commands to the RTL8822B's firmware (H2C), as rtw88's fw.c sends them.
 *
 * A command packet is 32 bytes behind a TX descriptor on the H2C queue: a header
 * of category 1 and command 0xff with the command's own id in the high half,
 * its total length and a sequence number, then the command's fields.
 */

#include "rtw.h"

#define H2C_PKT_SIZE        32
#define H2C_PKT_HDR_SIZE    8
#define H2C_PKT_CATEGORY    0x01
#define H2C_PKT_CMD_ID      0xFF
#define H2C_PKT_GENERAL_INFO 0x0D
#define H2C_PKT_PHYDM_INFO  0x11
#define FW_RF_2T2R          2
#define RSVD_PG_FW_TXBUF    4                   /* RSVD_PG_FW_TXBUF_NUM */
#define TXFF_PAGES          (262144 / 128)

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* rtw_fw_send_h2c_packet with rtw_usb_write_data_h2c: the packet's words 0 and 1
 * are the header, `w2` and `w3` its fields. */
static u32 send_packet(RTW *r, u32 id, u32 length, u32 w2, u32 w3)
{
    u8 *b = r->bulk_buf, *p = b + RTW_TX_DESC_SIZE;
    u32 i;
    u16 sum = 0;

    for (i = 0; i < RTW_TX_DESC_SIZE + H2C_PKT_SIZE; i++)
        b[i] = 0;
    put32(b, H2C_PKT_SIZE);                     /* w0: TXPKTSIZE, offset 0 */
    put32(b + 4, (u32)RTW_QSEL_H2C << 8);       /* w1: QSEL */
    for (i = 0; i < 32; i += 2)                 /* fill_txdesc_checksum_common */
        sum ^= (u16)(b[i] | b[i + 1] << 8);
    b[28] = (u8)sum;
    b[29] = (u8)(sum >> 8);

    put32(p, H2C_PKT_CATEGORY | H2C_PKT_CMD_ID << 8 | id << 16);
    put32(p + 4, (H2C_PKT_HDR_SIZE + length) | (r->h2c_seq & 0xFFFF) << 16);
    put32(p + 8, w2);
    put32(p + 12, w3);
    r->h2c_seq++;
    r->st->h2c_packets++;
    return rtw_bulk_out(r, RTW_EP_HIGH, b, RTW_TX_DESC_SIZE + H2C_PKT_SIZE);
}

/* rtw_fw_send_general_info: where the firmware's own transmit buffer is. */
u32 rtw_fw_send_general_info(RTW *r)
{
    u32 fw_txbuf = TXFF_PAGES - RSVD_PG_FW_TXBUF;

    return send_packet(r, H2C_PKT_GENERAL_INFO, 4,
                       ((fw_txbuf - r->rsvd_boundary) & 0xFF) << 16, 0);
}

/* rtw_fw_send_phydm_info: RFE, two paths each way, the cut. */
u32 rtw_fw_send_phydm_info(RTW *r)
{
    return send_packet(r, H2C_PKT_PHYDM_INFO, 8,
                       (r->st->rfe_option & 0xFF) | FW_RF_2T2R << 8 | (r->cut & 0xFF) << 16 |
                       3u << 24 | 3u << 28, 0);
}
