/* The wireless layer; see wireless.h. */

#include "wireless.h"
#include "file.h"

#define FC_BEACON      0x0080u
#define FC_PROBE_RESP  0x0050u
#define HEADER         24u      /* frame control .. sequence control */
#define FIXED          12u      /* timestamp, beacon interval, capability */
#define CAP_PRIVACY    0x0010u

#define IE_SSID        0
#define IE_DS          3
#define IE_RSN         48
#define IE_VENDOR      221

volatile WL_STATS wl_stats;
WL_BSS wl_bss[WL_BSS_MAX];

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

/* An RSN element (or WPA's, after its 4-byte OUI and type): version, group cipher,
 * pairwise ciphers, then the AKM suites, whose last byte says PSK (2), 802.1X (1),
 * SAE (8) or PSK-SHA256 (6). */
static u8 akm_security(const u8 *e, u32 length, u8 wpa2)
{
    u32 at = 2 + 4, count, i;
    u8 sec = 0;

    if (length < at + 2)
        return wpa2 ? WL_SEC_WPA2 : WL_SEC_WPA;    /* defaults: CCMP, PSK */
    count = le16(e + at);
    at += 2 + 4 * count;
    if (length < at + 2)
        return wpa2 ? WL_SEC_WPA2 : WL_SEC_WPA;
    count = le16(e + at);
    at += 2;
    for (i = 0; i < count && at + 4 <= length; i++, at += 4) {
        switch (e[at + 3]) {
        case 1: case 3: case 5:
            sec |= WL_SEC_8021X;
            break;
        case 2: case 6:
            sec |= wpa2 ? WL_SEC_WPA2 : WL_SEC_WPA;
            break;
        case 8: case 9:
            sec |= WL_SEC_SAE;
            break;
        default:
            break;
        }
    }
    return sec;
}

u32 wl_parse_bss(const u8 *frame, u32 length, WL_BSS *bss)
{
    u32 fc, at, i;
    u8 sec = 0, has_rsn = 0;

    if (length < HEADER + FIXED)
        return 0;
    fc = le16(frame) & 0x00FCu;
    if (fc != FC_BEACON && fc != FC_PROBE_RESP)
        return 0;
    for (i = 0; i < 6; i++)
        bss->bssid[i] = frame[16 + i];
    bss->capability = le16(frame + HEADER + 10);
    bss->ssid_len = 0;
    bss->ssid[0] = 0;
    bss->channel = 0;
    for (at = HEADER + FIXED; at + 2 <= length; at += 2 + frame[at + 1]) {
        const u8 *e = frame + at + 2;
        u32 n = frame[at + 1];

        if (at + 2 + n > length)
            break;
        switch (frame[at]) {
        case IE_SSID:
            if (n > WL_SSID_MAX)
                n = WL_SSID_MAX;
            for (i = 0; i < n; i++)
                bss->ssid[i] = (char)e[i];
            bss->ssid[n] = 0;
            bss->ssid_len = (u8)n;
            break;
        case IE_DS:
            if (n >= 1)
                bss->channel = e[0];
            break;
        case IE_RSN:
            has_rsn = 1;
            sec |= akm_security(e, n, 1);
            break;
        case IE_VENDOR:     /* Microsoft's WPA element: 00:50:f2, type 1 */
            if (n >= 4 && e[0] == 0x00 && e[1] == 0x50 && e[2] == 0xF2 && e[3] == 1) {
                has_rsn = 1;
                sec |= akm_security(e + 4, n - 4, 0);
            }
            break;
        default:
            break;
        }
    }
    if (!has_rsn && (bss->capability & CAP_PRIVACY))
        sec = WL_SEC_WEP;
    bss->security = sec;
    return 1;
}

void wl_scan_begin(void)
{
    u32 i;

    for (i = 0; i < WL_BSS_MAX; i++)
        wl_bss[i].seen = 0;
    wl_stats.bss_count = 0;
    wl_stats.scans++;
}

void wl_scan_result(const u8 *frame, u32 length, u32 freq, s32 rssi)
{
    WL_BSS found, *slot = 0;
    u32 i, k;

    wl_stats.results++;
    if (!wl_parse_bss(frame, length, &found)) {
        wl_stats.bad_frames++;
        return;
    }
    for (i = 0; i < wl_stats.bss_count; i++) {
        for (k = 0; k < 6 && wl_bss[i].bssid[k] == found.bssid[k]; k++)
            ;
        if (k == 6) {
            slot = &wl_bss[i];
            break;
        }
    }
    if (!slot) {
        if (wl_stats.bss_count >= WL_BSS_MAX) {
            wl_stats.table_full++;
            return;
        }
        slot = &wl_bss[wl_stats.bss_count++];
        for (i = 0; i < sizeof(found); i++)     /* not `=`: gcc makes that memcpy */
            ((u8 *)slot)[i] = ((const u8 *)&found)[i];
        slot->rssi = (s8)rssi;
    } else {
        /* A probe response names a hidden network a beacon does not. */
        if (found.ssid_len && !slot->ssid_len) {
            for (i = 0; i <= found.ssid_len; i++)
                slot->ssid[i] = found.ssid[i];
            slot->ssid_len = found.ssid_len;
        }
        if (rssi > slot->rssi)
            slot->rssi = (s8)rssi;
    }
    slot->freq = (u16)freq;
    if (!slot->channel)                 /* 5 GHz beacons often carry no DS element */
        slot->channel = (u8)(freq >= 5000 ? (freq - 5000) / 5 :
                             freq == 2484 ? 14 : (freq - 2407) / 5);
    slot->seen++;
}

void wl_scan_end(u32 status)
{
    wl_stats.last_scan_status = status;
    wl_stats.scans_done++;
}

/* --- which network to join --------------------------------------------------- */

volatile WL_CONFIG wl_config;

void wl_load_config(void)
{
    char ssid[WL_SSID_MAX + 1];
    u32 n, i;

    n = file_setting(WL_SETTINGS, "wifi_ssid", ssid, sizeof(ssid));
    for (i = 0; i < n; i++)
        wl_config.ssid[i] = ssid[i];
    wl_config.ssid[n] = 0;
    wl_config.ssid_len = n;
    wl_config.loaded = 1;
}

s32 wl_pick(u32 *secured)
{
    s32 best = -1;
    u32 i, k;

    *secured = 0;
    if (!wl_config.ssid_len)
        return -1;
    for (i = 0; i < wl_stats.bss_count; i++) {
        const WL_BSS *b = &wl_bss[i];

        if (b->ssid_len != wl_config.ssid_len)
            continue;
        for (k = 0; k < b->ssid_len && b->ssid[k] == wl_config.ssid[k]; k++)
            ;
        if (k != b->ssid_len)
            continue;
        if (b->security) {
            *secured = 1;               /* open networks only, for now */
            continue;
        }
        if (best < 0 || b->rssi > wl_bss[best].rssi)
            best = (s32)i;
    }
    return best;
}
