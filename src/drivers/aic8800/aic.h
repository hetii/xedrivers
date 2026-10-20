/* AIC8800D80 (aicsemi) USB Wi-Fi 6 + Bluetooth: what the driver's parts share.
 *
 * The chip comes up as a69c:8d80 with only its boot ROM running: one vendor
 * interface, bulk OUT 0x01 and bulk IN 0x82. The host writes the firmware into its
 * RAM with debug-task messages and starts it; the chip then leaves the bus and comes
 * back with the Wi-Fi firmware running. Some adapters ("Pandora" 88M80 clones) also
 * carry a 1111:1111 mass-storage device with Windows drivers behind the same hub;
 * the console's own mass-storage driver takes that one.
 *
 * Everything here is read from aicsemi's Linux driver (aic_load_fw and
 * aic8800_fdrv, as github.com/shenmintao/aic8800d80 5327c77 carries them): message
 * layout aic_load_fw/aicbluetooth_cmds.c, loading sequence aic_compat_8800d80.c.
 */

#ifndef XE_AIC_H
#define XE_AIC_H

#include "kernel.h"
#include "usb.h"
#include "net.h"

#define AIC_VENDOR          0xA69Cu
#define AIC_PRODUCT_D80     0x8D80u     /* boot ROM, waiting for firmware */
#define AIC_PRODUCT_D81     0x8D81u     /* the Wi-Fi firmware running */

/* A message to the chip, as it goes over bulk OUT (aicwf_set_cmd_tx):
 *   +0  u16 LE  length of what follows the first word, i.e. total - 4, 12 bits
 *   +2  u8      0x11, a command
 *   +4  u32     zero
 *   +8  u16 LE  message id, then destination task, source task, parameter length
 *  +16          parameters, little-endian words
 * A confirm comes back over bulk IN as a 4-byte header (length of what follows,
 * type 0x11) and the message: id, two unused task words, parameter length, a
 * pattern word, then the parameters. Several may share one transfer, each padded
 * to 4 bytes. Type bit 0x10 clear is a data frame (later, with the Wi-Fi firmware):
 * length + 60 bytes of descriptor. */
#define AIC_MSG_HEADER      16u
#define AIC_CFM_HEADER      16u
#define AIC_TYPE_CFG        0x10u
#define AIC_TYPE_CMD_RSP    0x11u
#define AIC_RX_HW_HEADER    60u

#define AIC_TASK_DBG        1u
#define AIC_DRV_TASK        100u
#define AIC_MSG(task, n)    ((task) << 10 | (n))

#define DBG_MEM_READ_REQ        AIC_MSG(AIC_TASK_DBG, 0)
#define DBG_MEM_READ_CFM        AIC_MSG(AIC_TASK_DBG, 1)
#define DBG_MEM_WRITE_REQ       AIC_MSG(AIC_TASK_DBG, 2)
#define DBG_MEM_WRITE_CFM       AIC_MSG(AIC_TASK_DBG, 3)
#define DBG_MEM_BLOCK_WRITE_REQ AIC_MSG(AIC_TASK_DBG, 11)
#define DBG_MEM_BLOCK_WRITE_CFM AIC_MSG(AIC_TASK_DBG, 12)
#define DBG_START_APP_REQ       AIC_MSG(AIC_TASK_DBG, 13)
#define DBG_MEM_MASK_WRITE_REQ  AIC_MSG(AIC_TASK_DBG, 17)
#define DBG_MEM_MASK_WRITE_CFM  AIC_MSG(AIC_TASK_DBG, 18)

#define AIC_BLOCK           1024u       /* bytes in one DBG_MEM_BLOCK_WRITE_REQ */
#define AIC_START_APP_AUTO  1u

/* Where it got to, in AIC_STATS.step; a failed step also records its status. */
enum {
    AIC_STEP_OK = 0,
    AIC_STEP_DMA = 1,
    AIC_STEP_ENDPOINTS,
    AIC_STEP_CHIP,              /* reading the chip id */
    AIC_STEP_PATCH_TABLE,       /* reading and parsing fw_patch_table */
    AIC_STEP_ADID,
    AIC_STEP_PATCH,
    AIC_STEP_EXT_PATCH,
    AIC_STEP_TABLE_LOAD,
    AIC_STEP_FMAC,
    AIC_STEP_PATCH_CONFIG,
    AIC_STEP_START_APP,
    AIC_STEP_FIRMWARE_STARTED,  /* not a failure: the chip leaves the bus now */
    AIC_STEP_UNKNOWN_PRODUCT,
    AIC_STEP_DESCRIPTOR,        /* reading the device descriptor */
    AIC_STEP_WIFI_STACK = 20,   /* the Wi-Fi firmware's bring-up, aic_wifi.c */
    AIC_STEP_WIFI_TXPWR,
    AIC_STEP_WIFI_CALIB,
    AIC_STEP_WIFI_MAC,
    AIC_STEP_WIFI_RESET,
    AIC_STEP_WIFI_CONFIG,
    AIC_STEP_WIFI_START,
    AIC_STEP_WIFI_IF,
    AIC_STEP_WIFI_SCAN,
    AIC_STEP_WIFI_UP,           /* not a failure: up, not joined */
};

/* One device slot's; read by tests/xectl.py. */
typedef struct {
    u32 product;
    u32 step;
    u32 step_status;
    u32 chip_word;              /* 0x40500000: chip id in bits 16-23 */
    u32 chip_id;
    u32 mcu_id;
    u32 fw_version;
    u32 bytes_loaded;
    u32 files_loaded;
    u32 file_status;            /* file_stats.last_status when a file was missing */
    u32 commands;
    u32 timeouts;
    u32 tx_errors;
    u32 tx_last_error;
    u32 rx_transfers;
    u32 rx_errors;
    u32 rx_last_error;
    u32 rx_messages;
    u32 rx_unexpected;          /* a message nobody waited for */
    u32 rx_last_id;
    u32 rx_data;                /* data frames (none expected from the boot ROM) */
    u32 last_cfm_id;
    u32 rx_first[8];            /* the first 32 bytes of the first transfer received */
    u32 scan_results;
    u32 fw_text[12];            /* MM_GET_FW_VERSION's text, bytes in order */
} AIC_STATS;

extern volatile AIC_STATS aic_stats[USB_MAX_DEVICES];

#define AIC_RX_COUNT    4
#define AIC_RX_SIZE     4096
#define AIC_TX_SIZE     2048
#define AIC_CFM_MAX     64      /* parameter bytes of a confirm we keep */
#define AIC_DESC_SIZE   64      /* control buffer: the device descriptor */

typedef struct AIC {
    u32 in_use;
    NET_DEV *nd;
    USB_DEV *dev;
    volatile AIC_STATS *st;
    void *ep_in;
    void *ep_out;
    URB rx_urb[AIC_RX_COUNT];
    u8 *rx_buf[AIC_RX_COUNT];
    u32 rx_consecutive_errors;
    URB tx_urb;
    u8 *tx_buf;
    volatile u32 tx_done;
    volatile u32 tx_status;
    volatile u32 running;
    volatile u32 wait_id;       /* the confirm awaited, 0 for none */
    volatile u32 cfm_done;
    u32 cfm_len;
    u8 cfm[AIC_CFM_MAX];
    u8 *file_buf;               /* AIC_BLOCK bytes, for reading files */
    u8 *desc_buf;               /* AIC_DESC_SIZE bytes, for control transfers */
    /* A message nobody waits for: returns 1 if it knew it. DISPATCH_LEVEL. */
    u32 (*ind)(struct AIC *a, u32 id, const u8 *params, u32 length);
    volatile u32 scanning;
    u32 vif;                    /* the station interface's index */
} AIC;

/* aic_cmd.c: the message channel, on bulk `ep_out` and `ep_in` (512 bytes). */
u32 aic_open(AIC *a, u32 ep_out, u32 ep_in);
void aic_close(AIC *a);
u32 aic_send(AIC *a, u32 id, const void *params, u32 length, u32 cfm_id);
u32 aic_mem_read(AIC *a, u32 address, u32 *value);
u32 aic_mem_write(AIC *a, u32 address, u32 value);
u32 aic_mem_mask_write(AIC *a, u32 address, u32 mask, u32 value);
u32 aic_block_write(AIC *a, u32 address, const u8 *data, u32 length);
u32 aic_start_app(AIC *a, u32 address, u32 type);

/* aic_fw.c: loading the firmware into a chip in boot ROM. Returns 0 or a step. */
u32 aic_load_firmware(AIC *a);

/* aic_wifi.c: the Wi-Fi firmware brought up, and a scan started. */
u32 aic_wifi_start(AIC *a);
u32 aic_wifi_scan(AIC *a);

static inline u32 aic_le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline void aic_put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

#endif /* XE_AIC_H */
