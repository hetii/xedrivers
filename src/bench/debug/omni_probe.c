/* What the dashboard asks of a wireless card, recorded: the plugin registers as one
 * (NicRegisterDevice, the way the omni and nomni drivers do) with the kernel's own
 * do-nothing services, except that option queries, option settings and link-state
 * polls pass through here first and land in a ring xectl reads. It says the card is
 * there, answers a scan with the AIC card's last one, and takes every setting. Research only: bench=debug, cmd 21 to
 * register, 22 to unregister.
 *
 * The stubs leave attach (+0x04), detach (+0x08) and finalize (+0x34) empty, and the
 * kernel calls attach as soon as it makes the card the current one (0x800D2AF0:
 * slot +0x00 with the global, +0x14 with the multicast list, then +0x04 with the
 * device and a block of callbacks, then DoSetOpt 11007), so those three are ours:
 * they record what they were given and succeed.
 *
 * Retail 17559 (live dump): WirelessNicNullServices at 0x80172118, its DoGetOpt
 * 0x800D5528 at +0x18, DoSetOpt 0x800D5568 at +0x1C, GetLinkState 0x800D5580 at
 * +0x20; NicRegisterDevice ordinal 739, NicUnregisterDevice 771. A device's first
 * two words must be zero (a list entry), +0x08 its type -- 0x40000002 as nomni's --
 * and +0x0C its services. xerunner: omni-services-17559@1, wireless-nic-option-path@1
 */

#include "debug.h"
#include "xnet.h"
#include "net.h"
#include "wireless.h"

#define NULL_SERVICES   0x80172118u
#define NULL_DOGETOPT   0x800D5528u
#define NULL_DOSETOPT   0x800D5568u
#define NULL_LINKSTATE  0x800D5580u
#define SERVICE_SLOTS   14
#define OID_BSSID_LIST_SCAN 0x0D01021Au
#define OID_BSSID_LIST      0x0D010217u

typedef s32 (*GET_OPT)(void *user, u32 option, u8 *buffer, u32 *length, u32 *flag);
typedef u32 (*LINK_STATE)(void);

volatile DEBUG_OMNI_CALL debug_omni_log[DEBUG_OMNI_LOG];
volatile u32 debug_omni_calls;
volatile u32 debug_omni_state;          /* 1 registered, else the refusal */

static u32 services[SERVICE_SLOTS];
static u32 device[0x80];                /* 512 bytes, the first words zero */

/* The dashboard polls 11001 many times a second: a call like the one before it
 * only counts on that one, or the ring would hold nothing else. */
static u32 seen_gets[64];

static void record(u32 op, u32 option, const u8 *buffer, u32 length, s32 result)
{
    u32 n = debug_omni_calls, i;
    volatile DEBUG_OMNI_CALL *c;

    /* A query is polled over and over, in turns with others: only its first time
     * is kept. Settings and everything else always are. */
    if (op == 0 && option != OID_BSSID_LIST && option != OID_BSSID_LIST_SCAN) {
        for (i = 0; i < 64 && seen_gets[i] && seen_gets[i] != option; i++)
            ;
        if (i < 64 && seen_gets[i] == option)
            return;
        if (i < 64)
            seen_gets[i] = option;
    }
    if (op == 2)
        return;

    if (n) {
        c = &debug_omni_log[(n - 1) % DEBUG_OMNI_LOG];
        if (c->op == op && c->option == option && c->length == length &&
            c->result == (u32)result &&
            (!buffer || length < 4 || *(const volatile u32 *)c->data == *(const u32 *)buffer)) {
            c->repeats++;
            return;
        }
    }
    n = __sync_fetch_and_add(&debug_omni_calls, 1);
    c = &debug_omni_log[n % DEBUG_OMNI_LOG];
    c->repeats = 0;
    c->seq = n + 1;
    c->op = op;
    c->option = option;
    c->length = length;
    c->result = (u32)result;
    for (i = 0; i < sizeof(c->data); i++)
        c->data[i] = buffer && i < length ? buffer[i] : 0;
}

/* 11001 is the adapter's status word (omni's 0x80139F30): bit 0 present and ready,
 * bits 8-15 its connection state. The stub says 0, which the dashboard shows as
 * "no wireless adapter"; this is what the probe says instead, settable by xectl. */
volatile u32 debug_omni_status = 1;

/* OID_802_11_BSSID_LIST_SCAN (asked as a get of no bytes) starts a scan, and
 * OID_802_11_BSSID_LIST hands its result: a count, then 244-byte entries, the
 * first word of each its length -- NDIS_WLAN_BSSID_EX with 128 bytes of elements,
 * as omni builds them (0x8013A4A0). The probe answers with what the AIC card's own
 * last scan found (wl_bss), to see what the dashboard does with a network. */
#define BSSID_ENTRY         244u

static void put32(u8 *p, u32 v) { *(u32 *)p = v; }

static u32 bssid_list(u8 *buffer, u32 length)
{
    static const u8 rsn[] = { 0x30, 20, 1, 0, 0x00, 0x0F, 0xAC, 4, 1, 0, 0x00, 0x0F, 0xAC, 4,
                              1, 0, 0x00, 0x0F, 0xAC, 2, 0, 0 };
    static const u8 rates[] = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
    u32 room = length >= 4 ? (length - 4) / BSSID_ENTRY : 0, n = 0, i, k;

    for (i = 0; i < wl_stats.bss_count && i < WL_BSS_MAX && n < room; i++) {
        const WL_BSS *b = &wl_bss[i];
        u8 *e = buffer + 4 + n * BSSID_ENTRY, *ie;

        for (k = 0; k < BSSID_ENTRY; k++)
            e[k] = 0;
        put32(e, BSSID_ENTRY);
        for (k = 0; k < 6; k++)
            e[4 + k] = b->bssid[k];
        put32(e + 12, b->ssid_len);
        for (k = 0; k < b->ssid_len && k < 32; k++)
            e[16 + k] = (u8)b->ssid[k];
        put32(e + 48, b->security != WL_SEC_OPEN);
        put32(e + 52, (u32)(s32)b->rssi);
        put32(e + 56, b->freq > 4000 ? 3 : 2);          /* OFDM5, OFDM24 */
        put32(e + 60, 32);                              /* configuration */
        put32(e + 64, 100);
        put32(e + 72, (u32)b->freq * 1000);
        put32(e + 76, 16);
        put32(e + 92, 1);                               /* infrastructure */
        for (k = 0; k < sizeof(rates); k++)
            e[96 + k] = rates[k];
        ie = e + 116;                                   /* timestamp, interval, caps */
        ie[8] = 100;
        ie[10] = (u8)b->capability;
        ie[11] = (u8)(b->capability >> 8);
        k = 12;
        if (b->security & WL_SEC_WPA2) {
            for (; k - 12 < sizeof(rsn); k++)
                ie[k] = rsn[k - 12];
        }
        put32(e + 112, k);
        /* What the dashboard reads past the elements (dash 0x922D4E78): the
         * channel at +235 and the security at +236, a word it maps through a
         * table of five (0x920298A0) -- by its range the type 11002 sets. */
        e[235] = b->channel;
        put32(e + 236, b->security & WL_SEC_WPA2 ? 3 : b->security & WL_SEC_WPA ? 2 :
                       b->security & WL_SEC_WEP ? 1 : 0);
        n++;
    }
    put32(buffer, n);
    return 4 + n * BSSID_ENTRY;
}

static s32 probe_get(void *user, u32 option, u8 *buffer, u32 *length, u32 *flag)
{
    u32 asked = length ? *length : 0;
    s32 r;

    if (option == 11001 && buffer && asked >= 4) {
        *(u32 *)buffer = debug_omni_status;
        *length = 4;
        *flag = 0;
        record(0, option, buffer, asked, 0);
        return 0;
    }
    if (option == OID_BSSID_LIST_SCAN) {
        *flag = 0;
        record(0, option, buffer, asked, 0);
        return 0;
    }
    if (option == OID_BSSID_LIST && buffer && asked >= 4) {
        *length = bssid_list(buffer, asked);
        *flag = 0;
        record(0, option, buffer, asked, 0);
        return 0;
    }
    r = ((GET_OPT)NULL_DOGETOPT)(user, option, buffer, length, flag);

    record(0, option, buffer, asked, r);
    return r;
}

/* Every setting is taken, so that the dashboard goes on to the next one. */
static s32 probe_set(void *user, u32 option, u8 *buffer, u32 length, u32 *flag)
{
    (void)user;
    *flag = 0;
    record(1, option, buffer, length, 0);
    return 0;
}

volatile u32 debug_omni_attach_info[8];   /* what attach was handed */

static u32 probe_attach(void *dev, const u32 *info)
{
    u32 i;

    for (i = 0; i < 8; i++)
        debug_omni_attach_info[i] = info ? info[i] : 0;
    record(3, 0, (const u8 *)info, info ? 32 : 0, 0);
    (void)dev;
    return 0;
}

static void probe_detach(void *dev)
{
    (void)dev;
    record(4, 0, 0, 0, 0);
}

static void probe_finalize(void *dev)
{
    (void)dev;
    record(5, 0, 0, 0, 0);
}

static u32 probe_link(void)
{
    u32 r = ((LINK_STATE)NULL_LINKSTATE)();

    record(2, 0, 0, 0, (s32)r);
    return r;
}

u32 debug_omni_register(void)
{
    const volatile u32 *null = (const volatile u32 *)NULL_SERVICES;
    u32 i;

    if (debug_omni_state == 1)
        return 1;
    if (null[6] != NULL_DOGETOPT || null[7] != NULL_DOSETOPT || null[8] != NULL_LINKSTATE)
        return debug_omni_state = 0xBAD0;       /* not the kernel this was read off */
    for (i = 0; i < SERVICE_SLOTS; i++)
        services[i] = null[i];
    services[1] = (u32)(unsigned long)probe_attach;
    services[2] = (u32)(unsigned long)probe_detach;
    services[13] = (u32)(unsigned long)probe_finalize;
    services[6] = (u32)(unsigned long)probe_get;
    services[7] = (u32)(unsigned long)probe_set;
    services[8] = (u32)(unsigned long)probe_link;
    for (i = 0; i < sizeof(device) / sizeof(device[0]); i++)
        device[i] = 0;
    device[2] = 0x40000002u;
    device[3] = (u32)(unsigned long)services;
    mem_sync();
    debug_omni_state = (u32)NicRegisterDevice(2, device);
    if (!debug_omni_state)
        debug_omni_state = 1;
    return debug_omni_state;
}

u32 debug_omni_unregister(void)
{
    if (debug_omni_state != 1)
        return 0;
    NicUnregisterDevice(2, device);
    debug_omni_state = 0;
    return 1;
}

/* While the plugin carries the network as a cable, or the onboard port has one,
 * the dashboard says a cable is plugged in and will not use the wireless card.
 * For a while the link polls say neither is (bench_wired_hidden), then they say
 * the truth again: XBDM is gone in between and returns afterwards by itself. */
volatile u32 debug_omni_wired_hidden;
static u32 wired_off_ticks;

u32 debug_omni_wired_off(u32 seconds)
{
    if (!seconds || seconds > 1800)
        return 0;
    wired_off_ticks = seconds * 50;
    debug_omni_wired_hidden = 1;
    xnet_link_changed();
    return 1;
}

void debug_omni_tick(void)
{
    if (wired_off_ticks && !--wired_off_ticks) {
        debug_omni_wired_hidden = 0;
        xnet_link_changed();
    }
}
