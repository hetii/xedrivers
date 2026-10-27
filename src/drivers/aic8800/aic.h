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
#include "wireless.h"
#include "wpa.h"
#include "reorder.h"
#include "bridge.h"
#include "aic_bt.h"

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
    AIC_STEP_BT_RESET = 40,     /* the Bluetooth controller, aic_bt.c */
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

    u32 connects;               /* SM_CONNECT_REQ sent */
    u32 connect_status;         /* its confirm */
    u32 connect_result;         /* SM_CONNECT_IND's status_code */
    u32 joined;
    u32 disconnects;            /* SM_DISCONNECT_IND */
    u32 disconnect_reason;
    u32 ap_sta;
    u32 aid;
    u32 join_freq;
    u32 join_refused_security;  /* a secured network with no passphrase set */
    u32 secured_joins;          /* joins that run the WPA2 handshake */
    u32 eapol_in;               /* EAPOL frames taken off the air */
    u32 eapol_out;              /* ... and put on it */
    u32 eapol_dropped;          /* ... that could not be sent */
    u32 keys_added;
    u32 key_status;             /* of the last MM_KEY_ADD */
    u32 port_status;            /* of ME_SET_CONTROL_PORT */
    u32 vif;                    /* the interface index the firmware gave us */
    u32 if_adds;
    u32 if_add_status;
    u32 if_removes;
    u32 if_remove_status;
    u32 if_mac[2];              /* the address it was added on, bytes in order */
    u32 join_index;             /* the scan table entry asked for */
    u32 join_freq_asked;
    u32 ps_off_status;          /* of ME_SET_PS_MODE at join */
    u32 bt_open_status;         /* opening the Bluetooth event endpoint 0x83 from
                                 * the Wi-Fi interface's node: can one function
                                 * reach the other's endpoints? */
    u32 bt_opened;
    u32 bssid[2];               /* the BSSID joined, bytes in order */
    u32 data_tx_frames;
    u32 data_tx_full;           /* no free transmit slot */
    u32 data_tx_errors;
    u32 data_tx_last_error;
    u32 data_rx_transfers;
    u32 data_rx_frames;
    u32 data_rx_errors;
    u32 data_rx_last_error;
    u32 data_rx_not_data;       /* a management or control frame came up here */
    u32 data_rx_unsupported;    /* aggregated MSDU or four-address */
    u32 data_rx_not_from_ds;
    u32 data_rx_short;
    u32 data_rx_truncated;      /* a length that overran the transfer */
    u32 data_rx_out_of_order;   /* a sequence number behind the last one of its TID */
    WL_REORDER_STATS reorder;   /* src/wireless/reorder.c's, for this card */
    u32 rx_unexpected_ids[4];   /* the last unexpected messages, by rx_unexpected % 4 */
    u32 join_width;             /* SM_CONNECT_IND: the width settled on (PHY_CHNL_BW_*) */
    u32 join_center1;
    u32 assoc_ht_cap;           /* what our association request offered */
    u32 assoc_vht_cap;
    u32 ap_vht_oper;            /* the AP's VHT operation: width, centre 1, centre 2 */
} AIC_STATS;

extern volatile AIC_STATS aic_stats[USB_MAX_DEVICES];

#define AIC_RX_COUNT    4
#define AIC_RX_SIZE     4096
#define AIC_TX_SIZE     2048
#define AIC_CFM_MAX     64      /* parameter bytes of a confirm we keep */
#define AIC_RX_ERROR_LIMIT 64

/* The data rings (aic_data.c), modest on purpose: too many outstanding requests
 * hang this console (the RTL8156 learned that at RX64+TX128), but the firmware
 * needs somewhere to put what it receives; Linux keeps 20 (AICWF_USB_RX_URBS)
 * and sixteen here carried six back-to-back runs where eight did not. The D80's firmware aggregates what it receives into one transfer
 * of up to AICWF_USB_AGGR_MAX_PKT_SIZE (20480, aicwf_usb_rx_aggr is on for this
 * chip), so a receive buffer must take that much: a smaller one ends the transfer
 * early and the rest of the aggregate starts the next buffer mid-packet, lost. */
#define AIC_DATA_RX_COUNT 16
#define AIC_DATA_RX_SIZE  20480
#define AIC_DATA_TX_COUNT 16
#define AIC_DATA_TX_SIZE  2048


/* One DMA block for the slot, carved once: both rings and nothing that grows. */
#define AIC_DMA_BYTES   (AIC_RX_COUNT * AIC_RX_SIZE + \
                         AIC_DATA_RX_COUNT * AIC_DATA_RX_SIZE + \
                         AIC_DATA_TX_COUNT * AIC_DATA_TX_SIZE + 4096)

#define AIC_AC_BK       0u      /* access categories, as the firmware numbers them */
#define AIC_AC_BE       1u
#define AIC_AC_VI       2u
#define AIC_AC_VO       3u
#define AIC_DESC_SIZE   64      /* control buffer: the device descriptor */
#define AIC_EAPOL_MAX   320     /* an EAPOL frame with its Ethernet header */

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

    /* Joined, and carrying frames (aic_data.c). */
    volatile u32 joined;
    volatile u32 data_running;
    u32 ap_sta;                 /* the AP's station index, from SM_CONNECT_IND */
    u8 if_mac[6];               /* the address the station interface was added on */
    void *ep_data_out;
    void *ep_data_in;
    URB data_rx_urb[AIC_DATA_RX_COUNT];
    u8 *data_rx_buf[AIC_DATA_RX_COUNT];
    u32 data_rx_consecutive_errors;
    URB data_tx_urb[AIC_DATA_TX_COUNT];
    u8 *data_tx_buf[AIC_DATA_TX_COUNT];
    u32 last_rx_stamp;          /* time base of the last data frame in (prof.h) */
    u16 rx_last_seq[8];         /* per TID, 0x8000 + sequence number once seen */
    WL_REORDER reorder;         /* block-ack windows (src/wireless/reorder.c) */
    u8 *reorder_pool;           /* WL_REORDER_POOL bytes, allocated once */
    KDPC reorder_dpc;           /* a timeout's flush, run where receive runs */
    volatile u32 reorder_dpc_queued;
    WL_CARD card;               /* what the layer above may ask of it */
    WL_LINK link;               /* which way it serves, and that mode's state */
    u32 secured;                /* this join runs the handshake before the link */
    u8 bssid[6];                /* the access point joined, for the EAPOL header */
    void *ep_bt_event;          /* the Bluetooth controller's event endpoint */
    volatile u32 bt_running;
    u32 bt_errors;
    u8 *eapol_buf;              /* DMA, one frame to send */
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

/* aic_wifi.c: the Wi-Fi firmware brought up, a scan started, a network joined
 * (by its index in the scan table, wl_bss). */
u32 aic_wifi_start(AIC *a);
u32 aic_wifi_scan(AIC *a);
u32 aic_wifi_join(AIC *a, const WL_BSS *bss, const char *pass, u32 pass_len);
u32 aic_wifi_disconnect(AIC *a);
void aic_wifi_card(AIC *a);         /* fills a->card */

/* aic_data.c: the data endpoints and the frames on them. */
u32 aic_data_open(AIC *a);
u32 aic_data_xmit(AIC *a, const u8 *frame, u32 length);
u32 aic_data_tx_space(AIC *a);
void aic_data_tick(AIC *a);
void aic_data_rejoin(AIC *a);

/* aic_cmd.c: one message of a transfer, for whichever endpoint it arrived on.
 * `packet` points at its four-byte header. */
void aic_message_in(AIC *a, const u8 *packet, u32 length);

static inline u32 aic_le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static inline void aic_put16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static inline void aic_put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

#endif /* XE_AIC_H */
