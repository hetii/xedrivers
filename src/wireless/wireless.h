/* The wireless layer: what is the same for every Wi-Fi card -- the networks a scan
 * found, which one to join, and (later) the WPA2 handshake. A card's driver
 * (src/drivers/<card>/) moves frames and runs its firmware's commands; it hands
 * every beacon or probe response a scan brings to wl_scan_result.
 */

#ifndef XE_WIRELESS_H
#define XE_WIRELESS_H

#include "kernel.h"

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

typedef struct {
    u32  loaded;
    u32  ssid_len;
    char ssid[WL_SSID_MAX + 1];
    u32  pass_len;              /* `wifi_pass =`, for a WPA2 network */
    char pass[WL_PASS_MAX + 1];
} WL_CONFIG;

extern volatile WL_CONFIG wl_config;

/* Read WL_SETTINGS again. Thread-only. */
void wl_load_config(void);

/* The entry of the last scan to join: the configured network's strongest access
 * point. Returns its index, or -1 when it was not seen; *secured is set when it
 * was, but only with a security this driver cannot join yet. */
s32 wl_pick(u32 *secured);

#endif /* XE_WIRELESS_H */
