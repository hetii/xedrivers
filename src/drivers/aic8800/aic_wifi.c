/* The AIC8800D80 with its Wi-Fi firmware running (a69c:8d81): bringing the firmware
 * up and scanning. Its Wi-Fi interface is the composite device's third (class ff):
 * commands over bulk 0x02 and 0x82, frames over 0x01 and 0x81. The Bluetooth
 * interfaces beside it (class e0) are not taken.
 *
 * The bring-up is aic8800_fdrv's for this chip, in its order, with the parameters
 * it sent (recorded with usbmon on the PC, tests/drivers/aic8800/wifi-8d81.txt):
 *   MM_SET_STACK_START, MM_GET_FW_VERSION, MM_SET_TXPWR_IDX_LVL, MM_SET_RF_CALIB
 *   (the firmware calibrates: 1.4 s), MM_GET_MAC_ADDR, MM_RESET, MM_VERSION,
 *   ME_CONFIG, ME_CHAN_CONFIG (twice, as Linux does), MM_START, MM_SET_COEX,
 *   MM_ADD_IF (a station on the card's own address).
 * A scan is SCANU_START over every channel the configuration lists, for any SSID;
 * its results arrive as SCANU_RESULT_IND, each a beacon or probe response, and
 * SCANU_START_CFM ends it.
 */

#include "aic.h"
#include "wireless.h"

#define TASK_MM     0u
#define TASK_SCANU  4u
#define TASK_ME     5u
#define TASK_SM     6u

#define MM_RESET_REQ            AIC_MSG(TASK_MM, 0)
#define MM_START_REQ            AIC_MSG(TASK_MM, 2)
#define MM_VERSION_REQ          AIC_MSG(TASK_MM, 4)
#define MM_ADD_IF_REQ           AIC_MSG(TASK_MM, 6)
#define MM_CHANNEL_SURVEY_IND   AIC_MSG(TASK_MM, 79)
#define MM_SET_COEX_REQ         AIC_MSG(TASK_MM, 101)
#define MM_SET_RF_CALIB_REQ     AIC_MSG(TASK_MM, 105)
#define MM_GET_MAC_ADDR_REQ     AIC_MSG(TASK_MM, 115)
#define MM_SET_TXPWR_IDX_LVL_REQ AIC_MSG(TASK_MM, 119)
#define MM_SET_STACK_START_REQ  AIC_MSG(TASK_MM, 123)
#define MM_GET_FW_VERSION_REQ   AIC_MSG(TASK_MM, 128)
#define ME_CONFIG_REQ           AIC_MSG(TASK_ME, 0)
#define ME_CHAN_CONFIG_REQ      AIC_MSG(TASK_ME, 2)
#define ME_SET_PS_MODE_REQ      AIC_MSG(TASK_ME, 19)
#define SCANU_START_REQ         AIC_MSG(TASK_SCANU, 0)
#define SCANU_START_CFM         AIC_MSG(TASK_SCANU, 1)
#define SCANU_RESULT_IND        AIC_MSG(TASK_SCANU, 4)
#define SCANU_START_CFM_ADDTIONAL AIC_MSG(TASK_SCANU, 9)
#define MM_REMOVE_IF_REQ        AIC_MSG(TASK_MM, 8)
#define SM_CONNECT_REQ          AIC_MSG(TASK_SM, 0)
#define SM_CONNECT_IND          AIC_MSG(TASK_SM, 2)
#define SM_DISCONNECT_REQ       AIC_MSG(TASK_SM, 3)
#define SM_DISCONNECT_IND       AIC_MSG(TASK_SM, 5)

#define CFM(req)                ((req) + 1)

/* mac_chan_def: freq (u16), band, flags, tx power, padding: 6 bytes. */
#define CHAN_DEF                6u
#define CHAN_24G_MAX            14u
#define CHAN_5G_MAX             28u
#define SSID_DEF                33u     /* mac_ssid: length, 32 bytes */
#define SCAN_SSID_MAX           3u
#define TX_POWER                20u     /* dBm, every channel, as Linux's world domain */

/* Linux's world regulatory domain (country code 00) on this chip: 2.4 GHz channels
 * 1-13 and 14, and the 5 GHz channels it lists. */
static const u16 freq_24g[] = {
    2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 2467, 2472, 2484,
};
static const u16 freq_5g[] = {
    5180, 5200, 5220, 5240, 5260, 5280, 5300, 5320, 5500, 5520, 5540, 5560, 5580,
    5600, 5620, 5640, 5660, 5680, 5700, 5720, 5745, 5765, 5785, 5805, 5825,
};

/* MM_SET_TXPWR_IDX_LVL_REQ: enable, then the power index per rate group, 2.4 GHz
 * (11b, 11ag, 11n, 11ac, 11ax) and 5 GHz -- aic_userconfig_8800d80.txt's defaults
 * as Linux packs them. */
static const u8 txpwr_idx_lvl[95] = {
    0x01, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x10, 0x10, 0x0f,
    0x0f, 0x12, 0x12, 0x12, 0x12, 0x10, 0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x12,
    0x12, 0x12, 0x12, 0x10, 0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x80,
    0x80, 0x80, 0x80, 0x12, 0x12, 0x12, 0x12, 0x10, 0x10, 0x0f, 0x0f, 0x12,
    0x12, 0x12, 0x12, 0x10, 0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x12, 0x12, 0x12,
    0x12, 0x10, 0x10, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c, 0x0c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* MM_SET_RF_CALIB_REQ: calibration items 2.4 GHz 0xf8f, 5 GHz 0xf0f, alpha
 * 0x0c34c008, BT calibration off with parameter 0x264203, crystal trim 0. */
static const u8 rf_calib[24] = {
    0x8f, 0x0f, 0x00, 0x00, 0x0f, 0x0f, 0x00, 0x00, 0x08, 0xc0, 0x34, 0x0c,
    0x00, 0x00, 0x00, 0x00, 0x03, 0x42, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ME_CONFIG_REQ: the HT, VHT and HE capabilities Linux advertises for the chip,
 * then tx lifetime 1000, widest band 80 MHz, HT, VHT and HE on, HE uplink off, power
 * save on, antenna diversity on, DPSM off. */
static const u8 me_config[112] = {
    0x63, 0x09, 0x1f, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x96, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0x71, 0x98, 0x03,
    0xfe, 0xff, 0x86, 0x01, 0xfe, 0xff, 0x86, 0x01, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x06, 0xe0, 0x2b, 0x58, 0x0d, 0xc0, 0xcf, 0x00, 0x02, 0x30,
    0x00, 0x00, 0xfe, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x38, 0x1c, 0xc7, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x02, 0x01, 0x01, 0x01, 0x00, 0x01,
    0x01, 0x00, 0x00, 0x00,
};

#define N24 (sizeof(freq_24g) / sizeof(freq_24g[0]))
#define N5  (sizeof(freq_5g) / sizeof(freq_5g[0]))

static u8 scratch[512];             /* message parameters, worker thread only */

static void zero(u8 *p, u32 n)
{
    while (n--)
        *p++ = 0;
}

static void chan_def(u8 *p, u32 freq, u32 band)
{
    p[0] = (u8)freq;
    p[1] = (u8)(freq >> 8);
    p[2] = (u8)band;
    p[3] = 0;
    p[4] = TX_POWER;
    p[5] = 0;
}

/* me_chan_config_req: 14 2.4 GHz slots, 28 5 GHz slots, the two counts. */
static u32 build_chan_config(u8 *p)
{
    u32 i;

    zero(p, (CHAN_24G_MAX + CHAN_5G_MAX) * CHAN_DEF + 2);
    for (i = 0; i < N24; i++)
        chan_def(p + i * CHAN_DEF, freq_24g[i], 0);
    for (i = 0; i < N5; i++)
        chan_def(p + (CHAN_24G_MAX + i) * CHAN_DEF, freq_5g[i], 1);
    p[(CHAN_24G_MAX + CHAN_5G_MAX) * CHAN_DEF] = (u8)N24;
    p[(CHAN_24G_MAX + CHAN_5G_MAX) * CHAN_DEF + 1] = (u8)N5;
    return (CHAN_24G_MAX + CHAN_5G_MAX) * CHAN_DEF + 2;
}

/* scanu_start_req: 42 channel slots (the 2.4 then the 5 GHz ones, back to back),
 * 3 SSIDs (one of length 0: any), the BSSID (broadcast), extra elements (none), vif,
 * counts, CCK allowed, duration 0 (the firmware's). 376 bytes. */
static u32 build_scan(u8 *p)
{
    u32 i, at = (CHAN_24G_MAX + CHAN_5G_MAX) * CHAN_DEF + SCAN_SSID_MAX * SSID_DEF;

    zero(p, 376);
    for (i = 0; i < N24; i++)
        chan_def(p + i * CHAN_DEF, freq_24g[i], 0);
    for (i = 0; i < N5; i++)
        chan_def(p + (N24 + i) * CHAN_DEF, freq_5g[i], 1);
    at = (at + 1) & ~1u;                    /* mac_addr: u16 aligned, 352 */
    for (i = 0; i < 6; i++)
        p[at + i] = 0xFF;
    /* add_ies at 360, add_ie_len 364: none */
    p[366] = 0;                             /* vif_idx */
    p[367] = (u8)(N24 + N5);                /* chan_cnt */
    p[368] = 1;                             /* ssid_cnt: the empty one */
    p[369] = 0;                             /* no_cck */
    return 376;
}

/* MM_ADD_IF_REQ: a station (type 0) on `mac`, not P2P -- ten bytes, as the driver
 * sends them. The confirm gives the interface's index. */
static u32 add_if(AIC *a, const u8 *mac)
{
    u32 status, i;

    zero(scratch, 10);
    for (i = 0; i < 6; i++)
        scratch[2 + i] = mac[i];
    a->st->if_adds++;
    status = aic_send(a, MM_ADD_IF_REQ, scratch, 10, CFM(MM_ADD_IF_REQ));
    if (!NT_SUCCESS(status)) {
        a->st->if_add_status = status;
        return status;
    }
    a->st->if_add_status = a->cfm_len >= 2 ? a->cfm[0] : 0xFF;
    if (a->cfm_len < 2 || a->cfm[0])
        return STATUS_UNSUCCESSFUL;             /* the firmware refused it */
    a->vif = a->cfm[1];
    a->st->vif = a->vif;
    for (i = 0; i < 6; i++)
        ((volatile u8 *)a->st->if_mac)[i] = mac[i];
    return STATUS_SUCCESS;
}

/* The address the station associates on must be the console's: xnet sends from it
 * and takes in only what is addressed to it, and the firmware puts the interface's
 * own address in every frame it builds, so the card has to be the console as far
 * as the network is concerned. The core asks the console for it before any device
 * is brought up (net_learn_console_mac), so the interface is created on it once
 * and needs no remaking; the card's own address is all that is left if that
 * failed. */
static const u8 *join_mac(AIC *a)
{
    if (net_console_mac_known)
        return net_console_mac;
    return a->nd->mac;
}

static u32 fail(AIC *a, u32 step, u32 status)
{
    a->st->step = step;
    a->st->step_status = status;
    return step;
}

/* Indications, at DISPATCH_LEVEL. */
static u32 wifi_ind(AIC *a, u32 id, const u8 *p, u32 length)
{
    u32 frame_len, freq, i;

    switch (id) {
    case SM_CONNECT_IND:
        /* status_code 0, the BSSID, the interface, then the AP's station index --
         * which every transmitted frame names -- and at the end the association id
         * and the channel settled on. */
        if (length < 12)
            return 1;
        a->st->connect_result = (u32)(p[0] | p[1] << 8);
        if (a->st->connect_result) {
            a->joined = 0;
            net_link_report(a->nd, 0);
            return 1;
        }
        a->ap_sta = p[10];
        a->st->ap_sta = a->ap_sta;
        for (i = 0; i < 6; i++)
            ((volatile u8 *)a->st->bssid)[i] = p[2 + i];
        if (length >= 826) {
            a->st->aid = (u32)(p[820] | p[821] << 8);
            a->st->join_freq = (u32)(p[824] | p[825] << 8);
        }
        mem_sync();
        a->joined = 1;
        a->st->joined++;
        /* The speed the dashboard shows is cosmetic here; what matters is that
         * the link is up, so xnet runs DHCP on it. */
        net_speed_report(a->nd, 65);
        net_link_report(a->nd, 1);
        return 1;
    case SM_DISCONNECT_IND:
        a->joined = 0;
        a->st->disconnects++;
        a->st->disconnect_reason = length >= 2 ? (u32)(p[0] | p[1] << 8) : 0;
        mem_sync();
        net_link_report(a->nd, 0);
        return 1;
    case SCANU_RESULT_IND:
        /* length, frame control, centre frequency, band, station, instance, rssi,
         * padding, then the frame. */
        if (length < 12)
            return 1;
        frame_len = (u32)(p[0] | p[1] << 8);
        freq = (u32)(p[4] | p[5] << 8);
        if (frame_len > length - 12)
            frame_len = length - 12;
        wl_scan_result(p + 12, frame_len, freq, (s8)p[9]);
        a->st->scan_results++;
        return 1;
    case SCANU_START_CFM:
        wl_scan_end(length >= 1 ? p[0] : 0xFF);
        a->scanning = 0;
        return 1;
    case MM_CHANNEL_SURVEY_IND:
        return 1;
    default:
        return 0;
    }
}

u32 aic_wifi_scan(AIC *a)
{
    u32 n = build_scan(scratch), status;

    if (a->scanning)
        return STATUS_UNSUCCESSFUL;
    wl_scan_begin();
    a->scanning = 1;
    status = aic_send(a, SCANU_START_REQ, scratch, n, SCANU_START_CFM_ADDTIONAL);
    if (!NT_SUCCESS(status))
        a->scanning = 0;
    wl_stats.last_scan_status = status;
    return status;
}

u32 aic_wifi_start(AIC *a)
{
    u32 status, n, i;

    a->ind = wifi_ind;
    a->scanning = 0;
    n = aic_open(a, 0x02, 0x82);
    if (n)
        return fail(a, n, 0);

    if (!NT_SUCCESS(status = aic_mem_read(a, 0x40500000u, &n)))
        return fail(a, AIC_STEP_CHIP, status);
    a->st->chip_word = n;
    a->st->chip_id = (n >> 16) & 0xFF;
    /* MM_SET_STACK_START: the stack starts, efuse not taken as valid, vendor info
     * 0x20, firmware trace not redirected. */
    scratch[0] = 1;
    scratch[1] = 0;
    scratch[2] = 0x20;
    scratch[3] = 0;
    if (!NT_SUCCESS(status = aic_send(a, MM_SET_STACK_START_REQ, scratch, 4,
                                      CFM(MM_SET_STACK_START_REQ))))
        return fail(a, AIC_STEP_WIFI_STACK, status);
    scratch[0] = 0;
    if (!NT_SUCCESS(status = aic_send(a, MM_GET_FW_VERSION_REQ, scratch, 1,
                                      CFM(MM_GET_FW_VERSION_REQ))))
        return fail(a, AIC_STEP_WIFI_STACK, status);
    for (i = 0; i < sizeof(a->st->fw_text) && i < a->cfm_len; i++)
        ((volatile u8 *)a->st->fw_text)[i] = a->cfm[i];
    if (!NT_SUCCESS(status = aic_send(a, MM_SET_TXPWR_IDX_LVL_REQ, txpwr_idx_lvl,
                                      sizeof(txpwr_idx_lvl),
                                      CFM(MM_SET_TXPWR_IDX_LVL_REQ))))
        return fail(a, AIC_STEP_WIFI_TXPWR, status);
    if (!NT_SUCCESS(status = aic_send(a, MM_SET_RF_CALIB_REQ, rf_calib, sizeof(rf_calib),
                                      CFM(MM_SET_RF_CALIB_REQ))))
        return fail(a, AIC_STEP_WIFI_CALIB, status);
    aic_put32(scratch, 1);
    if (!NT_SUCCESS(status = aic_send(a, MM_GET_MAC_ADDR_REQ, scratch, 4,
                                      CFM(MM_GET_MAC_ADDR_REQ))) || a->cfm_len < 6)
        return fail(a, AIC_STEP_WIFI_MAC, status);
    for (i = 0; i < 6; i++)
        a->nd->mac[i] = a->cfm[i];
    if (!NT_SUCCESS(status = aic_send(a, MM_RESET_REQ, 0, 0, CFM(MM_RESET_REQ))))
        return fail(a, AIC_STEP_WIFI_RESET, status);
    if (!NT_SUCCESS(status = aic_send(a, MM_VERSION_REQ, 0, 0, CFM(MM_VERSION_REQ))))
        return fail(a, AIC_STEP_WIFI_RESET, status);
    if (a->cfm_len >= 4)
        a->st->fw_version = aic_le32(a->cfm);
    if (!NT_SUCCESS(status = aic_send(a, ME_CONFIG_REQ, me_config, sizeof(me_config),
                                      CFM(ME_CONFIG_REQ))))
        return fail(a, AIC_STEP_WIFI_CONFIG, status);
    n = build_chan_config(scratch);
    for (i = 0; i < 2; i++)
        if (!NT_SUCCESS(status = aic_send(a, ME_CHAN_CONFIG_REQ, scratch, n,
                                          CFM(ME_CHAN_CONFIG_REQ))))
            return fail(a, AIC_STEP_WIFI_CONFIG, status);

    /* MM_START: no PHY configuration, U-APSD timeout 300 ms, sleep clock accuracy
     * 20 ppm. */
    zero(scratch, 72);
    aic_put32(scratch + 64, 300);
    scratch[68] = 20;
    if (!NT_SUCCESS(status = aic_send(a, MM_START_REQ, scratch, 72, CFM(MM_START_REQ))))
        return fail(a, AIC_STEP_WIFI_START, status);
    /* MM_SET_COEX: Bluetooth on, coexistence nulls on, null-CTS on, no periodic
     * timer, default time slots. */
    zero(scratch, 16);
    scratch[0] = 1;
    scratch[2] = 1;
    if (!NT_SUCCESS(status = aic_send(a, MM_SET_COEX_REQ, scratch, 16,
                                      CFM(MM_SET_COEX_REQ))))
        return fail(a, AIC_STEP_WIFI_START, status);
    /* A station on the console's address, created once: see join_mac. */
    if (!NT_SUCCESS(status = add_if(a, join_mac(a))))
        return fail(a, AIC_STEP_WIFI_IF, status);
    for (i = 0; i < 6; i++)
        a->if_mac[i] = join_mac(a)[i];


    /* The composite device's Bluetooth interface has its HCI event endpoint at 0x83
     * (interrupt, 64 bytes, bInterval 4: 1 ms at high speed). Whether the node the
     * kernel gave this driver reaches it decides how Bluetooth can be driven. */
    if (usb_open(a->dev, USB_TYPE_INTERRUPT, 0x83, 64, 1)) {
        a->st->bt_opened = 1;
        a->st->bt_open_status = usb_stats.last_open_status;
    } else {
        a->st->bt_open_status = usb_stats.last_open_status;
    }

    a->st->step = AIC_STEP_WIFI_UP;
    wl_load_config();
    a->auto_wait = 0;
    a->auto_scanned = 0;
    a->auto_off = 0;
    status = aic_wifi_scan(a);
    if (!NT_SUCCESS(status))
        return fail(a, AIC_STEP_WIFI_SCAN, status);
    return 0;
}

/* --- joining ------------------------------------------------------------------- */

/* sm_connect_req, 320 bytes: the SSID, the BSSID, the channel, no connection flags
 * (an open network needs none -- no pairwise key, and the firmware opens the
 * controlled port itself), the EAPOL ethertype, no added elements, and open
 * authentication. The firmware builds the association request from the
 * capabilities ME_CONFIG gave it.
 *
 * The flags the driver's own structures use are one byte wide (bool_l is C's
 * bool), which is what puts the authentication type at 59 and the interface at 61
 * rather than four bytes further on. */
static u32 build_connect(AIC *a, const WL_BSS *bss)
{
    u32 i;

    zero(scratch, 320);
    scratch[0] = bss->ssid_len;
    for (i = 0; i < bss->ssid_len && i < WL_SSID_MAX; i++)
        scratch[1 + i] = (u8)bss->ssid[i];
    for (i = 0; i < 6; i++)
        scratch[34 + i] = bss->bssid[i];
    aic_put16(scratch + 40, bss->freq);         /* chan: frequency, band, flags */
    scratch[42] = (u8)(bss->freq >= 5000 ? 1 : 0);
    aic_put32(scratch + 48, 0);                 /* flags */
    scratch[52] = 0x88;                         /* controlled port ethertype 888E */
    scratch[53] = 0x8E;
    aic_put16(scratch + 54, 0);                 /* no added elements */
    aic_put16(scratch + 56, 0);                 /* listen interval */
    scratch[58] = 0;                            /* wait for broadcast after beacon */
    scratch[59] = 0;                            /* open authentication */
    scratch[60] = 1;                            /* U-APSD for voice, as Linux */
    scratch[61] = (u8)a->vif;
    return 320;
}

u32 aic_wifi_join(AIC *a, u32 index)
{
    const u8 *mac = join_mac(a);
    const WL_BSS *bss;
    u32 status, i, differs = 0;

    if (index >= wl_stats.bss_count || a->st->product != AIC_PRODUCT_D81)
        return STATUS_UNSUCCESSFUL;
    bss = &wl_bss[index];
    if (!bss->ssid_len)
        return STATUS_UNSUCCESSFUL;             /* a hidden network needs its name */
    if (bss->security) {
        /* Only open networks for now: a secured one needs the key exchange, which
         * this driver does not do yet. */
        a->st->join_refused_security++;
        return STATUS_UNSUCCESSFUL;
    }
    if (a->joined && !NT_SUCCESS(status = aic_wifi_disconnect(a)))
        return status;

    for (i = 0; i < 6; i++)
        differs |= a->if_mac[i] != mac[i];
    a->st->join_index = index;
    a->st->join_freq_asked = bss->freq;
    if (differs) {
        /* The interface was created on another address, which only happens when the
         * console's was not known at bring-up. Made again here, on the right one;
         * the firmware hands out the next free index, so the new one is read back
         * from its confirm rather than assumed. */
        zero(scratch, 4);
        scratch[0] = (u8)a->vif;
        a->st->if_removes++;
        status = aic_send(a, MM_REMOVE_IF_REQ, scratch, 4, CFM(MM_REMOVE_IF_REQ));
        a->st->if_remove_status = status;
        if (!NT_SUCCESS(status))
            return status;
        status = add_if(a, mac);
        if (!NT_SUCCESS(status))
            return status;
        for (i = 0; i < 6; i++)
            a->if_mac[i] = mac[i];
    }
    for (i = 0; i < 6; i++)
        a->nd->mac[i] = mac[i];

    /* The receive ring has to be waiting before the association completes, and the
     * endpoints are opened once: a device has four and the commands took two. */
    if (!a->ep_data_out) {
        status = aic_data_open(a);
        if (status)
            return fail(a, status, 0);
    }

    /* Power saving off (MM_PS_MODE_OFF), as the host does when a connection is
     * asked for. ME_CONFIG had it on, because those parameters are the ones
     * Linux's driver sends and its default is on; a station that sleeps between
     * beacons carried 6 Mbit/s of the 39 the radio was managing, so the console is
     * better off awake. */
    scratch[0] = 0;
    status = aic_send(a, ME_SET_PS_MODE_REQ, scratch, 1, CFM(ME_SET_PS_MODE_REQ));
    a->st->ps_off_status = status;

    a->st->connects++;
    status = aic_send(a, SM_CONNECT_REQ, scratch, build_connect(a, bss),
                      CFM(SM_CONNECT_REQ));
    a->st->connect_status = status;
    if (NT_SUCCESS(status) && a->cfm_len >= 1 && a->cfm[0]) {
        a->st->connect_status = a->cfm[0];
        return STATUS_UNSUCCESSFUL;
    }
    return status;
}

u32 aic_wifi_disconnect(AIC *a)
{
    u32 status;

    zero(scratch, 4);
    aic_put16(scratch, 3);                      /* reason: station leaving */
    scratch[2] = (u8)a->vif;
    status = aic_send(a, SM_DISCONNECT_REQ, scratch, 4, CFM(SM_DISCONNECT_REQ));
    a->joined = 0;
    mem_sync();
    net_link_report(a->nd, 0);
    return status;
}

/* --- joining by itself ---------------------------------------------------------- */

/* Every five seconds while not joined: scan, then join the configured network's
 * strongest access point if it was seen (wl_pick). The same loop takes the console
 * back when the access point goes away (SM_DISCONNECT_IND) or the join fails. The
 * worker calls it every tick (20 ms) while the card is up. */
#define AUTO_RETRY_TICKS 250

void aic_wifi_tick(AIC *a)
{
    u32 secured;
    s32 pick;

    if (a->joined || a->auto_off || !wl_config.ssid_len || a->scanning)
        return;
    if (a->auto_wait) {
        a->auto_wait--;
        return;
    }
    if (!a->auto_scanned) {
        a->auto_scanned = 1;
        a->st->auto_scans++;
        if (!NT_SUCCESS(aic_wifi_scan(a))) {
            a->auto_scanned = 0;
            a->auto_wait = AUTO_RETRY_TICKS;
        }
        return;                         /* the results come in while it runs */
    }
    a->auto_scanned = 0;
    a->auto_wait = AUTO_RETRY_TICKS;
    pick = wl_pick(&secured);
    if (pick < 0) {
        a->st->auto_missed++;
        if (secured)
            a->st->auto_secured++;
        return;
    }
    a->st->auto_joins++;
    aic_wifi_join(a, (u32)pick);
}
