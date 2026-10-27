/* Native test of the dashboard's mode (src/wireless/omni.c) against a card that is
 * only a WL_CARD and a kernel that is only what it hands the device: what each
 * option is answered with, how a scan becomes the list the dashboard reads, the
 * join it asks for, and frames both ways. omni.c is included whole, so that the
 * services the kernel would call through its table are called here directly. */

#include <stdio.h>
#include <string.h>

static unsigned int empty_table[14] = {
    0, 0, 0, 0, 0, 0, 0x800D5528u, 0x800D5568u, 0x800D5580u, 0, 0, 0, 0, 0
};
#define WL_OMNI_EMPTY_TABLE ((const volatile u32 *)empty_table)

#include "../../src/wireless/omni.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

/* --- the kernel, the network layer, the settings, the handshake ------------------ */

static u32 registered_kind;
static void *registered_device;
s32 NicRegisterDevice(u32 kind, void *dev)
{
    registered_kind = kind;
    registered_device = dev;
    return 0;
}
void NicUnregisterDevice(u32 kind, void *dev) { (void)kind; (void)dev; }
u32 KfAcquireSpinLock(KSPIN_LOCK *lock) { (void)lock; return 0; }
void KfReleaseSpinLock(KSPIN_LOCK *lock, u32 irql) { (void)lock; (void)irql; }

static const NET_CLAIM *claimed;
void net_claim(NET_DEV *nd, const NET_CLAIM *c) { (void)nd; claimed = c; }
static u32 sent, sent_length;
u32 net_dev_tx(NET_DEV *nd, const u8 *frame, u32 length)
{
    (void)nd;
    (void)frame;
    sent++;
    sent_length = length;
    return 1;
}
u32 net_dev_tx_space(NET_DEV *nd) { (void)nd; return 1; }

u32 file_setting(const char *name, const char *key, char *out, u32 size)
{
    (void)name;
    (void)key;
    if (size)
        out[0] = 0;
    return 0;
}
u32 wpa_apply(void) { return 0; }
const WL_MODE wl_bridge_mode;          /* not this test's (bridge.c) */

/* --- what the kernel was given at attach -------------------------------------- */

static u32 received, received_dest, link_changes, kernel_done;
static void k_rx(void *c, const u8 *f, u32 l, u32 u, u32 dest)
{
    (void)c; (void)f; (void)l; (void)u;
    received++;
    received_dest = dest;
}
static void k_done(void *c) { (void)c; kernel_done++; }
static void k_link(void *c) { (void)c; link_changes++; }

/* The console stack's user + 16, as the kernel passes it: its +8 points at the
 * callbacks, whose +8 is the context and +16 the transmit-complete. */
static u32 user_done;
static void *user_done_cookie;
static void u_done(void *c, void *cookie) { (void)c; user_done++; user_done_cookie = cookie; }
static struct { void *pad; void *callbacks; } user;
static struct { void *pad; void *context; void (*done)(void *, void *); } callbacks;

/* --- the card ---------------------------------------------------------------------- */

static u32 scans, joins, leaves, joined_state, scanning;
static const WL_BSS *joined_bss;
static char joined_pass[65];

static u32 c_scan(void *c) { (void)c; scans++; scanning = 1; return 0; }
static u32 c_join(void *c, const WL_BSS *bss, const char *p, u32 n)
{
    (void)c;
    joins++;
    joined_bss = bss;
    memcpy(joined_pass, p, n);
    joined_pass[n] = 0;
    return 0;
}
static u32 c_leave(void *c) { (void)c; leaves++; joined_state = 0; return 0; }
static u32 c_state(void *c)
{
    (void)c;
    return (joined_state ? WL_CARD_JOINED : 0) | (scanning ? WL_CARD_SCANNING : 0);
}
static const WL_CARD card_ops = { 0, c_scan, c_join, c_leave, c_state };

static void seen(u32 i, const char *name, s8 rssi, u8 sec, u8 b5)
{
    memset(&wl_bss[i], 0, sizeof(wl_bss[i]));
    wl_bss[i].ssid_len = (u8)strlen(name);
    strcpy(wl_bss[i].ssid, name);
    wl_bss[i].rssi = rssi;
    wl_bss[i].security = sec;
    wl_bss[i].freq = 5600;
    wl_bss[i].channel = 120;
    wl_bss[i].bssid[5] = b5;
}

static void scan_done(u32 n)
{
    wl_stats.bss_count = n;
    wl_stats.scans_done++;
    scanning = 0;
}

static u32 get32(u32 option, s32 *status)
{
    u32 v = 0, n = 4, unsupported;

    *status = get(&user, option, (u8 *)&v, &n, &unsupported);
    return v;
}

int main(void)
{
    static u8 list_buffer[17536];
    u8 sec[72], ssid_set[36];
    u32 n, unsupported, i;
    s32 status;
    NET_DEV nd;
    ATTACH_INFO given;
    u8 frame[60];

    memset(&nd, 0, sizeof(nd));
    wl_omni_start(&card_ops, &nd);
    CHECK(wl_omni_stats.registered == 1 && registered_kind == 2);
    CHECK(registered_device == device && device[2] == 0x40000002u);
    CHECK(claimed == &claim);

    memset(&given, 0, sizeof(given));
    given.rx = k_rx;
    given.xmit_done = k_done;
    given.link_changed = k_link;
    CHECK(attach(device, &given) == 0 && wl_omni_stats.attached);

    /* There, not connected; a scan asked for is started on the worker. */
    CHECK(get32(OPT_STATUS, &status) == 1 && status == 0);
    CHECK(link_state() == LINK_DOWN);
    n = 0;
    CHECK(get(&user, OID_BSSID_LIST_SCAN, 0, &n, &unsupported) == 0);
    wl_omni_tick();
    CHECK(scans == 1);

    /* The scan's networks become the list, 244 bytes each, the security where
     * the dashboard reads it; the table emptied by the next scan does not. */
    seen(0, "home", -70, WL_SEC_WPA2, 1);
    seen(1, "home", -50, WL_SEC_WPA2, 2);
    seen(2, "cafe", -40, WL_SEC_OPEN, 3);
    scan_done(3);
    wl_omni_tick();
    n = sizeof(list_buffer);
    CHECK(get(&user, OID_BSSID_LIST, list_buffer, &n, &unsupported) == 0);
    CHECK(n == 4 + 3 * 244 && *(u32 *)list_buffer == 3);
    CHECK(*(u32 *)(list_buffer + 4) == 244 && list_buffer[4 + 4 + 5] == 1);
    CHECK(*(u32 *)(list_buffer + 4 + 12) == 4 && !memcmp(list_buffer + 4 + 16, "home", 4));
    CHECK(*(u32 *)(list_buffer + 4 + 236) == 3 && list_buffer[4 + 235] == 120);
    CHECK(*(u32 *)(list_buffer + 4 + 2 * 244 + 236) == 0);
    wl_stats.bss_count = 0;                     /* a new scan under way */
    scanning = 1;
    wl_omni_tick();
    n = sizeof(list_buffer);
    get(&user, OID_BSSID_LIST, list_buffer, &n, &unsupported);
    CHECK(*(u32 *)list_buffer == 3);
    scan_done(3);
    wl_omni_tick();
    n = 100;                                    /* too small for one entry */
    CHECK(get(&user, OID_BSSID_LIST, list_buffer, &n, &unsupported) == 0);
    CHECK(*(u32 *)list_buffer == 0 && n == 4);

    /* The join, as the dashboard sends it: security, then the name. The
     * strongest access point of that name is joined, with the passphrase. */
    memset(sec, 0, sizeof(sec));
    *(u32 *)sec = 3;
    *(u32 *)(sec + 4) = 11;
    memcpy(sec + 8, "secret pass", 11);
    CHECK(set(&user, OPT_SECURITY, sec, 72, &unsupported) == 0);
    CHECK(set(&user, OPT_SECURITY, sec, 40, &unsupported) != 0);
    memset(ssid_set, 0, sizeof(ssid_set));
    *(u32 *)ssid_set = 4;
    memcpy(ssid_set + 4, "home", 4);
    CHECK(set(&user, OID_SSID, ssid_set, 36, &unsupported) == 0);
    wl_omni_tick();
    CHECK(joins == 1 && joined_bss && joined_bss->bssid[5] == 2);
    CHECK(!strcmp(joined_pass, "secret pass"));
    n = 8;
    CHECK(get(&user, OPT_SECURITY, sec, &n, &unsupported) == 0 && *(u32 *)sec == 3);

    /* The port opens: the kernel is told, and the dashboard sees it connected. */
    joined_state = 1;
    claimed->link(claimed->context, 1);
    CHECK(link_changes >= 1 && link_state() == LINK_UP);
    CHECK(get32(OPT_STATUS, &status) == (1 | 4 << 8));
    CHECK((s32)get32(OID_RSSI, &status) == -50);
    CHECK(get32(OPT_RATE, &status) == 72000000u);

    /* Frames: in to the kernel by the kind of address, out to the card and then
     * handed back to their user and counted out by the kernel. */
    memset(frame, 0, sizeof(frame));
    frame[0] = 0x00;
    claimed->rx(claimed->context, frame, sizeof(frame));
    CHECK(received == 1 && received_dest == 1);
    memset(frame, 0xFF, 6);
    claimed->rx(claimed->context, frame, sizeof(frame));
    CHECK(received == 2 && received_dest == 4);
    callbacks.done = u_done;
    user.callbacks = &callbacks;
    xmit(&user, 0, frame, sizeof(frame), (void *)0x1234);
    CHECK(sent == 1 && sent_length == sizeof(frame));
    CHECK(user_done == 1 && user_done_cookie == (void *)0x1234 && kernel_done == 1);

    /* Lost: frames are handed back unsent, and the network is joined again. */
    joined_state = 0;
    claimed->link(claimed->context, 0);
    xmit(&user, 0, frame, sizeof(frame), (void *)0x5678);
    CHECK(sent == 1 && user_done == 2 && kernel_done == 2);
    for (i = 0; i < 250; i++)
        wl_omni_tick();
    CHECK(joins == 1);
    wl_omni_tick();
    CHECK(joins == 2 && wl_omni_stats.rejoins == 1);

    /* A network not in the list is looked for once more, then given up. */
    memcpy(ssid_set + 4, "away", 4);
    set(&user, OID_SSID, ssid_set, 36, &unsupported);
    wl_omni_tick();
    CHECK(scans == 2 && joins == 2);
    scan_done(3);
    wl_omni_tick();
    CHECK(joins == 2 && wl_omni_stats.missed == 1);

    /* Disconnect: left, and not joined again. */
    CHECK(set(&user, OPT_DISCONNECT, 0, 0, &unsupported) == 0);
    wl_omni_tick();
    CHECK(leaves == 1);
    for (i = 0; i < 600; i++)
        wl_omni_tick();
    CHECK(joins == 2);

    /* An option not known is said so. The card gone: no adapter. */
    n = 4;
    CHECK(get(&user, 0x0D010203u, frame, &n, &unsupported) != 0 && unsupported == 1);
    wl_omni_gone();
    CHECK(get32(OPT_STATUS, &status) == 0);

    if (failures)
        return 1;
    printf("all omni tests passed\n");
    return 0;
}
