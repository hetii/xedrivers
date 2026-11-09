/* Native test of the card-as-a-cable policy (src/wireless/bridge.c) against a
 * card that is only a WL_CARD: when it scans, which network it joins and with
 * what, how it waits between tries, and the commands that hold it off. */

#include <stdio.h>
#include <string.h>

#include "bridge.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

/* --- the settings and the handshake, stood in for ------------------------------ */

static const char *setting_ssid = "home, other";
static const char *setting_pass = "secret pass";

u32 file_setting(const char *name, const char *key, char *out, u32 size)
{
    const char *v = !strcmp(key, "wifi_ssid") ? setting_ssid :
                    !strcmp(key, "wifi_pass") ? setting_pass : "";
    u32 n = 0;

    (void)name;
    for (; v[n] && n + 1 < size; n++)
        out[n] = v[n];
    if (size)
        out[n] = 0;
    return n;
}

static u32 keys_waiting;
u32 wpa_apply(void) { u32 w = keys_waiting; keys_waiting = 0; return w; }

const WL_MODE wl_omni_mode;            /* not this test's (omni.c) */
const u8 *net_join_mac(NET_DEV *nd) { return nd->mac; }  /* the tests run without a device */

/* --- the card -------------------------------------------------------------------- */

static u32 scans, joins, leaves, joined, scanning;
static const WL_BSS *joined_bss;
static char joined_pass[64];

static u32 c_scan(void *c) { (void)c; scans++; scanning = 1; return 0; }
static u32 c_join(void *c, const WL_BSS *bss, const char *pass, u32 pass_len)
{
    (void)c;
    joins++;
    joined_bss = bss;
    memcpy(joined_pass, pass, pass_len);
    joined_pass[pass_len] = 0;
    return 0;
}
static u32 c_leave(void *c) { (void)c; leaves++; joined = 0; return 0; }
static u32 c_state(void *c)
{
    (void)c;
    return (joined ? WL_CARD_JOINED : 0) | (scanning ? WL_CARD_SCANNING : 0);
}

static const WL_CARD card = { 0, c_scan, c_join, c_leave, c_state };

/* A scan's result, as wireless.c keeps them. */
static void seen(u32 i, const char *ssid, s8 rssi, u8 security)
{
    memset(&wl_bss[i], 0, sizeof(wl_bss[i]));
    wl_bss[i].ssid_len = (u8)strlen(ssid);
    strcpy(wl_bss[i].ssid, ssid);
    wl_bss[i].rssi = rssi;
    wl_bss[i].security = security;
    wl_bss[i].freq = 5745;
}

static void scan_done(u32 n)
{
    wl_stats.bss_count = n;
    scanning = 0;
}

int main(void)
{
    WL_BRIDGE b;
    u32 i;

    wl_load_config();
    wl_bridge_start(&b, &card);
    CHECK(wl_config.count == 2 && wl_config.pass_len == 11);

    /* First tick: a scan. The next ones wait for it, then pick and join the
     * strongest of the named networks, with the passphrase. */
    wl_bridge_tick(&b, &card);
    CHECK(scans == 1);
    wl_bridge_tick(&b, &card);
    CHECK(scans == 1 && joins == 0);            /* still scanning */
    seen(0, "neighbour", -40, WL_SEC_WPA2);
    seen(1, "home", -70, WL_SEC_WPA2);
    seen(2, "other", -50, WL_SEC_WPA2);
    scan_done(3);
    wl_bridge_tick(&b, &card);
    CHECK(joins == 1 && joined_bss == &wl_bss[2]);
    CHECK(!strcmp(joined_pass, "secret pass"));
    CHECK(wl_bridge_stats.scans == 1 && wl_bridge_stats.joins == 1);

    /* Joined: nothing more is asked. */
    joined = 1;
    for (i = 0; i < 1000; i++)
        wl_bridge_tick(&b, &card);
    CHECK(scans == 1 && joins == 1);

    /* The handshake's keys go to the card before anything else. */
    joined = 0;
    keys_waiting = 1;
    wl_bridge_tick(&b, &card);
    CHECK(scans == 1);

    /* Lost: it waits five seconds of ticks, then scans again. */
    for (i = 0; i < 249; i++)
        wl_bridge_tick(&b, &card);
    CHECK(scans == 1);
    wl_bridge_tick(&b, &card);
    wl_bridge_tick(&b, &card);
    CHECK(scans == 2);

    /* Not seen this time: counted as missed, and tried again later. */
    scan_done(1);                               /* only the neighbour */
    wl_bridge_tick(&b, &card);
    CHECK(joins == 1 && wl_bridge_stats.missed == 1);

    /* Left by command: no automatic join until joined by command again. */
    joined = 1;
    CHECK(wl_bridge_leave(&b, &card) == 0 && leaves == 1 && !joined);
    for (i = 0; i < 1000; i++)
        wl_bridge_tick(&b, &card);
    CHECK(scans == 2);
    scan_done(3);
    CHECK(wl_bridge_join(&b, &card, 1) == 0 && joined_bss == &wl_bss[1]);
    CHECK(wl_bridge_join(&b, &card, 7) != 0);   /* past the table */

    if (failures)
        return 1;
    printf("all bridge tests passed\n");
    return 0;
}
