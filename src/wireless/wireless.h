/* The wireless layer: what is the same for every Wi-Fi card -- the networks a scan
 * found, which one to join, the WPA2 handshake (wpa.c), putting block acks back in
 * order (reorder.c), and the policy that decides when to join what (bridge.c). A
 * card's driver (src/drivers/<card>/) moves frames and runs its firmware's
 * commands; it hands every beacon or probe response a scan brings to
 * wl_scan_result, and offers what it can do as a WL_CARD.
 */

#ifndef XE_WIRELESS_H
#define XE_WIRELESS_H

#include "kernel.h"
#include "net.h"

#define WL_SSID_MAX  32
#define WL_BSS_MAX   24

/* What a network says it needs (from its RSN or WPA element and capability). */
#define WL_SEC_OPEN   0x00u
#define WL_SEC_WEP    0x01u     /* privacy bit, no RSN or WPA element */
#define WL_SEC_WPA    0x02u
#define WL_SEC_WPA2   0x04u     /* RSN with PSK */
#define WL_SEC_SAE    0x08u     /* RSN with SAE: WPA3 */
#define WL_SEC_8021X  0x10u     /* RSN or WPA with 802.1X: enterprise */

typedef struct {
    u8  bssid[6];
    u8  ssid_len;
    u8  channel;
    char ssid[WL_SSID_MAX + 1];
    s8  rssi;                   /* dBm, the strongest frame seen */
    u8  security;               /* WL_SEC_* */
    u16 freq;                   /* MHz */
    u16 capability;
    u32 seen;                   /* frames from it in the last scan */
} WL_BSS;

typedef struct {
    u32 scans;                  /* started */
    u32 scans_done;
    u32 results;                /* frames handed in */
    u32 bad_frames;
    u32 table_full;
    u32 bss_count;
    u32 last_scan_status;
} WL_STATS;

extern volatile WL_STATS wl_stats;
extern WL_BSS wl_bss[WL_BSS_MAX];

/* A scan starts: the table is emptied. Thread or DISPATCH. */
void wl_scan_begin(void);

/* One beacon or probe response, the 802.11 frame from its frame control on, on
 * `freq` MHz at `rssi` dBm. Any IRQL up to DISPATCH. */
void wl_scan_result(const u8 *frame, u32 length, u32 freq, s32 rssi);

void wl_scan_end(u32 status);

/* Parse the elements of one frame into `bss` (the pure part, for tests). Returns 0
 * if the frame is not a beacon or probe response. */
u32 wl_parse_bss(const u8 *frame, u32 length, WL_BSS *bss);

/* --- which network to join --------------------------------------------------- */

#define WL_SETTINGS "xedrivers.ini"     /* beside the plugin (src/core/file.h) */

/* The network asked for: `wifi_ssid=` in WL_SETTINGS, read once a Wi-Fi card is
 * up. An empty name joins nothing by itself. */
#define WL_PASS_MAX  63
#define WL_NAMES_MAX 4          /* networks that may be named at once */

/* The networks asked for: `wifi_ssid =` may name several, separated by commas,
 * and the strongest access point of any of them is joined. Two routers under one
 * name are covered by that anyway; two bands are not, since they are two names,
 * and which of them carries more depends entirely on where the card sits. */
typedef struct {
    u32  loaded;
    u32  count;
    u32  ssid_len[WL_NAMES_MAX];
    char ssid[WL_NAMES_MAX][WL_SSID_MAX + 1];
    u32  pass_len;              /* `wifi_pass =`, for a WPA2 network */
    char pass[WL_PASS_MAX + 1];
    u32  dashboard;             /* `wifi_mode = dashboard`, else ethernet */
} WL_CONFIG;

extern volatile WL_CONFIG wl_config;

/* Read WL_SETTINGS again. Thread-only. */
void wl_load_config(void);

/* The entry of the last scan to join: the configured network's strongest access
 * point. Returns its index, or -1 when it was not seen; *secured is set when it
 * was, but only with a security this driver cannot join yet. */
s32 wl_pick(u32 *secured);

/* --- a card, as the layer above it sees one ------------------------------------ */

#define WL_CARD_JOINED   0x1u   /* associated (and, if secured, the handshake begun) */
#define WL_CARD_SCANNING 0x2u

/* What a driver can do, and nothing about when to do it: that is the mode's
 * (WL_MODE below). Every call is from the worker thread. */
typedef struct {
    void *context;
    /* Start a scan; its results go to wl_scan_result and wl_scan_end. */
    u32 (*scan)(void *context);
    /* Join `bss` (an entry of wl_bss), with the passphrase if it is secured. */
    u32 (*join)(void *context, const WL_BSS *bss, const char *pass, u32 pass_len);
    u32 (*leave)(void *context);
    u32 (*state)(void *context);            /* WL_CARD_* */
} WL_CARD;

/* --- which way a card serves the console ---------------------------------------- *
 * `wifi_mode =` in WL_SETTINGS: `ethernet` (the default) -- the card joins the
 * network the file names and is one more cable for xnet (bridge.c); `dashboard` --
 * it is the console's wireless adapter and the dashboard chooses and joins
 * (omni.c). The driver keeps a WL_LINK per card, takes the mode wl_mode_pick
 * names when the card is up, and calls only through it. */

/* The ethernet mode's state for one card (bridge.c). */
typedef struct {
    u32 wait;               /* worker ticks until the next automatic attempt */
    u32 scanned;            /* a scan of the automatic join is under way */
    u32 off;                /* left by command: no automatic join until asked */
} WL_BRIDGE;

typedef struct WL_LINK WL_LINK;

typedef struct {
    /* The card is up (the settings read). Thread. */
    void (*start)(WL_LINK *l, NET_DEV *nd);
    /* Every worker tick while it is up. */
    void (*tick)(WL_LINK *l);
    /* The card is gone. Any IRQL up to DISPATCH. */
    void (*gone)(WL_LINK *l);
    /* By command (tests/xectl.py): join the scan table's entry `index`, or leave;
     * a mode in which the dashboard decides refuses both. */
    u32  (*join)(WL_LINK *l, u32 index);
    u32  (*leave)(WL_LINK *l);
} WL_MODE;

struct WL_LINK {
    const WL_MODE *mode;
    const WL_CARD *card;
    WL_BRIDGE bridge;
};

extern const WL_MODE wl_bridge_mode;    /* bridge.c */
extern const WL_MODE wl_omni_mode;      /* omni.c */

/* The mode the settings name. After wl_load_config. */
const WL_MODE *wl_mode_pick(void);

#endif /* XE_WIRELESS_H */
