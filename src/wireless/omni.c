/* The card as the console's wireless adapter; see omni.h.
 *
 * The kernel calls a wireless device through a table of fourteen services, the
 * device's word at +0x0C (WirelessNicNullServices at 0x80172118 on 17559 is the
 * empty one; omni's at 0x8017C2B0 the full one). The ones that matter here:
 *
 *   +0x04 attach(device, info)    info: +0x08 context, +0x0C receive,
 *                                 +0x10 transmit done, +0x14 link changed
 *   +0x08 detach(device)          +0x34 finalize(device)
 *   +0x10 xmit(user, queue, frame, length, cookie)
 *   +0x18 get(user, option, buffer, &length, &unsupported)
 *   +0x1C set(user, option, buffer, length, &unsupported)
 *   +0x20 link state()
 *
 * The rest (the address, multicast list, counters) keep the empty table's
 * answers. `user` is the console stack's NIC user + 16; a frame sent is handed
 * back through *(user + 8): its +0x10 called with its +0x08 and the cookie, and
 * then info's transmit done, which the kernel counts frames out by. A frame
 * received goes to info's receive with the kind of address it was sent to, and
 * the kernel hands it to every user whose filter takes that kind.
 *
 * Every address here was read off the live 17559 kernel, and the empty table is
 * checked against three of its words before anything is registered.
 * xerunner: omni-services-17559@1, wireless-nic-option-path@1, omni-options@1,
 * dashboard-wireless-join@1
 */

#include "omni.h"
#include "txq.h"
#include "wpa.h"
#include "usb.h"

#define NULL_SERVICES   0x80172118u
#define NULL_GET        0x800D5528u
#define NULL_SET        0x800D5568u
#define NULL_LINK       0x800D5580u
#define SLOTS           14
#define KIND_WIRELESS   2u
#define DEVICE_TYPE     0x40000002u     /* as the kernel's own adapters' */

/* Options, NDIS 802.11 and the console's own. */
#define OID_BSSID           0x0D010101u
#define OID_SSID            0x0D010102u
#define OID_INFRASTRUCTURE  0x0D010108u
#define OID_RSSI            0x0D010206u
#define OID_BSSID_LIST      0x0D010217u
#define OID_BSSID_LIST_SCAN 0x0D01021Au
#define OPT_STATUS          11001u      /* bit 0 present, bits 8-15 the state */
#define OPT_SECURITY        11002u      /* 72 bytes set, 8 read */
#define OPT_CAPS            11005u
#define OPT_DISCONNECT      11006u
#define OPT_RATE            11008u      /* bits per second, as omni's table */

#define STATE_CONNECTED     4u          /* omni's, the one the dashboard tests for */
#define LINK_DOWN           0x00010000u /* omni's link state answers */
#define LINK_UP             0x00010021u
#define RATE_MAX            72000000u   /* the highest omni reports */
#define RSSI_NONE           (-200)

#define STATUS_BUFFER_SMALL  0xC0000023u
#define STATUS_NOT_HANDLED   0xC00000BBu

#define BSSID_ENTRY     244u            /* dash 0x922D4E78 reads up to +240 */
#define SECURITY_SET    72u
#define PASS_MAX        64u
#define REJOIN_TICKS    250             /* 5 s of 20 ms worker ticks */

typedef struct {
    u32 reserved[2];
    void *context;
    void (*rx)(void *context, const u8 *frame, u32 length, u32 unused, u32 dest);
    void (*xmit_done)(void *context);
    void (*link_changed)(void *context);
} ATTACH_INFO;

typedef void (*USER_DONE)(void *context, void *cookie);

volatile WL_OMNI_STATS wl_omni_stats;

/* xam and the kernel call the services from their own threads: through the stubs that keep
 * the caller's r14-r31 whole (usb.h, USB_STUB). */
#if defined(__powerpc__)
USB_ENTRY(omni_attach_entry, omni_attach);
extern char omni_attach_entry[];
USB_ENTRY(omni_detach_entry, omni_detach);
extern char omni_detach_entry[];
USB_ENTRY(omni_xmit_entry, omni_xmit);
extern char omni_xmit_entry[];
USB_ENTRY(omni_get_entry, omni_get);
extern char omni_get_entry[];
USB_ENTRY(omni_set_entry, omni_set);
extern char omni_set_entry[];
USB_ENTRY(omni_link_state_entry, omni_link_state);
extern char omni_link_state_entry[];
USB_ENTRY(omni_finalize_entry, omni_finalize);
extern char omni_finalize_entry[];
#define ENTRY(n) n##_entry
#else
#define ENTRY(n) n
#endif
static u32 services[SLOTS];
static u32 device[0x80];                /* the kernel's device object */
static volatile ATTACH_INFO info;
static const WL_CARD *card;
static NET_DEV *net_dev;

/* What the dashboard asked for, set on its threads, done on the worker. */
static volatile u32 want_scan, want_join, want_leave;
static volatile u32 security;
static char pass[PASS_MAX + 1];
static volatile u32 pass_len;
static char ssid[WL_SSID_MAX + 1];
static volatile u32 ssid_len;

/* The network joined, and the list the dashboard is shown: the last finished
 * scan, kept while the next one runs (the card empties its table at the start). */
static WL_BSS joined;
static volatile u32 joined_valid;
static WL_BSS list[WL_BSS_MAX];
static volatile u32 list_count;
static u32 scans_seen;
static u32 joining, rejoin_wait;

/* --- transmit ------------------------------------------------------------------ */

static u32 q_send(void *owner, const u8 *frame, u32 length)
{
    (void)owner;
    return net_dev ? net_dev_tx(net_dev, frame, length) : 0;
}

static u32 q_space(void *owner)
{
    (void)owner;
    return net_dev && net_dev_tx_space(net_dev);
}

static u32 q_keep(void *owner)
{
    (void)owner;
    return wl_omni_stats.link;
}

static void q_done(void *owner, void *user, void *cookie)
{
    u8 *callbacks = *(u8 **)((u8 *)user + 8);
    USER_DONE done = *(USER_DONE *)(callbacks + 16);

    (void)owner;
    if (done)
        done(*(void **)(callbacks + 8), cookie);
    if (info.xmit_done)
        info.xmit_done(info.context);
}

static NET_TXQ txq = { .send = q_send, .space = q_space, .keep = q_keep,
                       .done = q_done, .dropped = &wl_omni_stats.tx_dropped,
                       .high = &wl_omni_stats.tx_high };

USB_ENTERED void omni_xmit(void *user, u32 queue, void *frame, u32 length, void *cookie)
{
    (void)queue;
    wl_omni_stats.tx++;
    if (!wl_omni_stats.link) {
        wl_omni_stats.tx_dropped++;
        q_done(0, user, cookie);
        return;
    }
    net_txq_put(&txq, user, frame, length, cookie);
}

/* --- from the network layer: the card is ours, not xnet's ---------------------- */

#define DEST_UNICAST   1u
#define DEST_MULTICAST 3u
#define DEST_BROADCAST 4u

static void claim_rx(void *context, const u8 *frame, u32 length)
{
    u32 dest;

    (void)context;
    if (!info.rx || !wl_omni_stats.link)
        return;
    if (frame[0] & 1)
        dest = frame[0] == 0xFF && frame[1] == 0xFF ? DEST_BROADCAST : DEST_MULTICAST;
    else
        dest = DEST_UNICAST;
    wl_omni_stats.rx++;
    info.rx(info.context, frame, length, 0, dest);
}

static void claim_link(void *context, u32 up)
{
    (void)context;
    wl_omni_stats.link = up;
    mem_sync();
    if (!up)
        net_txq_flush(&txq);
    if (info.link_changed)
        info.link_changed(0);
}

static void claim_tx_idle(void *context)
{
    (void)context;
    net_txq_drain(&txq);
}

static const NET_CLAIM claim = { 0, claim_rx, claim_link, claim_tx_idle };

/* --- options ------------------------------------------------------------------- */

static void put32(u8 *p, u32 v)
{
    *(u32 *)p = v;
}

/* Copy an answer in, or say how much room it needs. */
static s32 answer(u8 *buffer, u32 *length, const void *data, u32 n)
{
    const u8 *d = (const u8 *)data;
    u32 i;

    if (!buffer || *length < n) {
        *length = n;
        return (s32)STATUS_BUFFER_SMALL;
    }
    for (i = 0; i < n; i++)
        buffer[i] = d[i];
    *length = n;
    return 0;
}

static s32 answer32(u8 *buffer, u32 *length, u32 value)
{
    return answer(buffer, length, &value, 4);
}

/* The security type 11002 carries, from what a scan saw: 0 open, 1 WEP, 2 WPA,
 * 3 WPA2 (dash maps it through its table at 0x920298A0 into what it shows). */
static u32 security_of(const WL_BSS *b)
{
    return b->security & WL_SEC_WPA2 ? 3 : b->security & WL_SEC_WPA ? 2 :
           b->security & WL_SEC_WEP ? 1 : 0;
}

/* The scan, as omni hands it (0x8013A4A0): a count, then 244-byte entries, an
 * NDIS_WLAN_BSSID_EX each, with the channel at +235 and the security type at +236
 * where the dashboard reads them. It reads no elements, so none are given. */
static u32 bssid_list(u8 *buffer, u32 length)
{
    static const u8 rates[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
    u32 room = (length - 4) / BSSID_ENTRY, n, i, k;

    for (n = 0; n < list_count && n < room; n++) {
        const WL_BSS *b = &list[n];
        u8 *e = buffer + 4 + n * BSSID_ENTRY;

        for (k = 0; k < BSSID_ENTRY; k++)
            e[k] = 0;
        put32(e, BSSID_ENTRY);
        for (k = 0; k < 6; k++)
            e[4 + k] = b->bssid[k];
        put32(e + 12, b->ssid_len);
        for (k = 0; k < b->ssid_len && k < 32; k++)
            e[16 + k] = (u8)b->ssid[k];
        put32(e + 48, b->security != WL_SEC_OPEN);      /* privacy */
        put32(e + 52, (u32)(s32)b->rssi);
        put32(e + 56, b->freq > 4000 ? 3 : 2);          /* OFDM5, OFDM24 */
        put32(e + 60, 32);                              /* configuration */
        put32(e + 64, 100);                             /* beacon interval */
        put32(e + 72, (u32)b->freq * 1000);             /* kHz */
        put32(e + 76, 16);
        put32(e + 92, 1);                               /* infrastructure */
        for (i = 0; i < sizeof(rates); i++)
            e[96 + i] = rates[i];
        put32(e + 112, 12);                             /* the fixed fields only */
        e[116 + 8] = 100;
        e[116 + 10] = (u8)b->capability;
        e[116 + 11] = (u8)(b->capability >> 8);
        e[235] = b->channel;
        put32(e + 236, security_of(b));
    }
    put32(buffer, n);
    return 4 + n * BSSID_ENTRY;
}

USB_ENTERED s32 omni_get(void *user, u32 option, u8 *buffer, u32 *length, u32 *unsupported)
{
    u8 data[36];
    u32 i, n;

    (void)user;
    *unsupported = 0;
    wl_omni_stats.gets++;
    switch (option) {
    case OPT_STATUS:
        return answer32(buffer, length, wl_omni_stats.present |
                        (wl_omni_stats.link ? STATE_CONNECTED << 8 : 0));
    case OPT_SECURITY:
        put32(data, security);
        put32(data + 4, 0);
        return answer(buffer, length, data, 8);
    case OPT_CAPS:                      /* omni's constant (0x8004C724) */
        put32(data, 4);
        for (i = 0; i < 4; i++)
            put32(data + 4 + 4 * i, i);
        return answer(buffer, length, data, 20);
    case OPT_RATE:
        return answer32(buffer, length, wl_omni_stats.link ? RATE_MAX : 0);
    case OID_INFRASTRUCTURE:
        return answer32(buffer, length, 1);
    case OID_RSSI:
        return answer32(buffer, length, wl_omni_stats.link && joined_valid ?
                        (u32)(s32)joined.rssi : (u32)RSSI_NONE);
    case OID_SSID:
        n = ssid_len;
        put32(data, n);
        for (i = 0; i < 32; i++)
            data[4 + i] = i < n ? (u8)ssid[i] : 0;
        return answer(buffer, length, data, 36);
    case OID_BSSID:
        if (!wl_omni_stats.link || !joined_valid)
            break;
        return answer(buffer, length, joined.bssid, 6);
    case OID_BSSID_LIST_SCAN:
        if (!wl_omni_stats.present)
            break;
        want_scan = 1;
        return 0;
    case OID_BSSID_LIST:
        if (!buffer || *length < 4) {
            *length = 4 + list_count * BSSID_ENTRY;
            return (s32)STATUS_BUFFER_SMALL;
        }
        *length = bssid_list(buffer, *length);
        return 0;
    default:
        break;
    }
    wl_omni_stats.unknown_gets++;
    wl_omni_stats.last_unknown = option;
    *unsupported = 1;
    return (s32)STATUS_NOT_HANDLED;
}

USB_ENTERED s32 omni_set(void *user, u32 option, u8 *buffer, u32 length, u32 *unsupported)
{
    u32 i, n;

    (void)user;
    *unsupported = 0;
    wl_omni_stats.sets++;
    switch (option) {
    case OPT_SECURITY:                  /* type, passphrase length, passphrase */
        if (length != SECURITY_SET || !buffer)
            return (s32)STATUS_UNSUCCESSFUL;
        n = *(const u32 *)(buffer + 4);
        if (n > PASS_MAX)
            n = PASS_MAX;
        for (i = 0; i < n; i++)
            pass[i] = (char)buffer[8 + i];
        pass[n] = 0;
        pass_len = n;
        security = *(const u32 *)buffer;
        wl_omni_stats.security = security;
        return 0;
    case OID_SSID:                      /* the join */
        if (length < 4 || !buffer)
            return (s32)STATUS_UNSUCCESSFUL;
        n = *(const u32 *)buffer;
        if (n > WL_SSID_MAX || n + 4 > length)
            return (s32)STATUS_UNSUCCESSFUL;
        for (i = 0; i < n; i++)
            ssid[i] = (char)buffer[4 + i];
        ssid[n] = 0;
        ssid_len = n;
        mem_sync();
        want_leave = 0;
        want_join = 1;
        return 0;
    case OPT_DISCONNECT:
        want_join = 0;
        want_leave = 1;
        return 0;
    default:
        /* The mode, the network type, the antennas: settled by the card. */
        return 0;
    }
}

USB_ENTERED u32 omni_link_state(void)
{
    return wl_omni_stats.link ? LINK_UP : LINK_DOWN;
}

/* --- the device's lifetime, as the kernel sees it -------------------------------- */

USB_ENTERED u32 omni_attach(void *dev, const ATTACH_INFO *given)
{
    (void)dev;
    info.context = given->context;
    info.rx = given->rx;
    info.xmit_done = given->xmit_done;
    info.link_changed = given->link_changed;
    mem_sync();
    wl_omni_stats.attached = 1;
    return 0;
}

USB_ENTERED void omni_detach(void *dev)
{
    (void)dev;
    wl_omni_stats.attached = 0;
    info.rx = 0;
    info.link_changed = 0;
    mem_sync();
    net_txq_flush(&txq);
    info.xmit_done = 0;
}

USB_ENTERED void omni_finalize(void *dev)
{
    (void)dev;
}

/* The host tests (tests/wireless/test_omni.c) give a table of their own. */
#ifndef WL_OMNI_EMPTY_TABLE
#define WL_OMNI_EMPTY_TABLE ((const volatile u32 *)(unsigned long)NULL_SERVICES)
#endif

static u32 register_device(void)
{
    const volatile u32 *empty = WL_OMNI_EMPTY_TABLE;
    s32 status;
    u32 i;

    if (empty[6] != NULL_GET || empty[7] != NULL_SET || empty[8] != NULL_LINK)
        return 0xBAD0;                  /* not the kernel these were read off */
    for (i = 0; i < SLOTS; i++)
        services[i] = empty[i];
    services[1] = (u32)(unsigned long)ENTRY(omni_attach);
    services[2] = (u32)(unsigned long)ENTRY(omni_detach);
    services[4] = (u32)(unsigned long)ENTRY(omni_xmit);
    services[6] = (u32)(unsigned long)ENTRY(omni_get);
    services[7] = (u32)(unsigned long)ENTRY(omni_set);
    services[8] = (u32)(unsigned long)ENTRY(omni_link_state);
    services[13] = (u32)(unsigned long)ENTRY(omni_finalize);
    for (i = 0; i < sizeof(device) / sizeof(device[0]); i++)
        device[i] = 0;
    device[2] = DEVICE_TYPE;
    device[3] = (u32)(unsigned long)services;
    mem_sync();
    status = NicRegisterDevice(KIND_WIRELESS, device);
    return status ? (u32)status : 1;
}

/* --- the worker ----------------------------------------------------------------- */

void wl_omni_start(const WL_CARD *c, NET_DEV *nd)
{
    card = c;
    net_dev = nd;
    joining = 0;
    rejoin_wait = 0;
    scans_seen = wl_stats.scans_done;
    net_claim(nd, &claim);
    wl_omni_stats.link = 0;
    wl_omni_stats.present = 1;
    mem_sync();
    if (wl_omni_stats.registered != 1)
        wl_omni_stats.registered = register_device();
    if (info.link_changed)
        info.link_changed(0);
}

void wl_omni_gone(void)
{
    wl_omni_stats.present = 0;
    wl_omni_stats.link = 0;
    card = 0;
    mem_sync();
    net_txq_flush(&txq);
    if (info.link_changed)
        info.link_changed(0);
}

/* The strongest access point of the network asked for, in the last scan. */
static s32 pick(void)
{
    s32 best = -1;
    u32 i, k;

    for (i = 0; i < list_count; i++) {
        const WL_BSS *b = &list[i];

        if (b->ssid_len != ssid_len)
            continue;
        for (k = 0; k < ssid_len && b->ssid[k] == ssid[k]; k++)
            ;
        if (k == ssid_len && (best < 0 || b->rssi > list[best].rssi))
            best = (s32)i;
    }
    return best;
}

/* Field by field would be the same; a structure assigned whole is a memcpy the
 * plugin has no library for. */
static void copy_bss(WL_BSS *to, const WL_BSS *from)
{
    const u8 *f = (const u8 *)from;
    u8 *t = (u8 *)to;
    u32 i;

    for (i = 0; i < sizeof(WL_BSS); i++)
        t[i] = f[i];
}

static void keep_scan(void)
{
    u32 i;

    for (i = 0; i < wl_stats.bss_count && i < WL_BSS_MAX; i++)
        copy_bss(&list[i], &wl_bss[i]);
    mem_sync();
    list_count = i;
    scans_seen = wl_stats.scans_done;
}

void wl_omni_tick(void)
{
    u32 state;
    s32 at;

    if (!card)
        return;
    if (card->wpa && wpa_apply(card->wpa))
        return;
    state = card->state(card->context);
    if (wl_stats.scans_done != scans_seen && !(state & WL_CARD_SCANNING))
        keep_scan();

    if (want_leave) {
        want_leave = 0;
        joining = 0;
        joined_valid = 0;
        wl_omni_stats.leaves++;
        card->leave(card->context);
        return;
    }
    if (state & WL_CARD_SCANNING)
        return;

    if (want_join) {
        /* A network not in the last scan is looked for once more. */
        at = pick();
        if (at < 0 && joining != 2) {
            joining = 2;
            wl_omni_stats.scans++;
            card->scan(card->context);
            return;
        }
        want_join = 0;
        joining = 0;
        if (at < 0) {
            wl_omni_stats.missed++;
            return;
        }
        if (state & WL_CARD_JOINED)
            card->leave(card->context);
        copy_bss(&joined, &list[at]);
        joined_valid = 1;
        wl_omni_stats.joins++;
        card->join(card->context, &joined, pass, pass_len);
        rejoin_wait = REJOIN_TICKS;
        return;
    }

    /* Lost the network the dashboard joined: join it again, as its own adapters
     * do, every few seconds until it is back or something else is asked for. */
    if (joined_valid && !(state & WL_CARD_JOINED)) {
        if (rejoin_wait) {
            rejoin_wait--;
            return;
        }
        wl_omni_stats.rejoins++;
        rejoin_wait = REJOIN_TICKS;
        card->join(card->context, &joined, pass, pass_len);
        return;
    }

    if (want_scan) {
        want_scan = 0;
        wl_omni_stats.scans++;
        card->scan(card->context);
    }
}

/* --- as a WL_MODE (wireless.h) -------------------------------------------------- */

static void mode_start(WL_LINK *l, NET_DEV *nd)
{
    wl_omni_start(l->card, nd);
}

static void mode_tick(WL_LINK *l)
{
    (void)l;
    wl_omni_tick();
}

static void mode_gone(WL_LINK *l)
{
    (void)l;
    wl_omni_gone();
}

static u32 mode_refuse(WL_LINK *l, u32 index)
{
    (void)l;
    (void)index;
    return STATUS_UNSUCCESSFUL;
}

static u32 mode_leave(WL_LINK *l)
{
    (void)l;
    return STATUS_UNSUCCESSFUL;
}

const WL_MODE wl_omni_mode = { mode_start, mode_tick, mode_gone, mode_refuse, mode_leave };
