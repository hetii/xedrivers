/* The card as a cable: it joins the network xedrivers.ini names, by itself, and the
 * network layer sees an Ethernet uplink like any other.
 *
 * Every five seconds while not joined: scan, then join the configured network's
 * strongest access point if it was seen (wl_pick). The same loop takes the console
 * back when the access point goes away or the join fails. Whatever the WPA2
 * handshake settled is handed to the card first, here, because installing a key
 * is a command the firmware confirms and the worker is where that may be waited
 * for.
 */

#include "bridge.h"
#include "wpa.h"
#include "net.h"
#include "file.h"
#include "driver.h"

#define RETRY_TICKS 250             /* 5 s of 20 ms worker ticks */

volatile WL_BRIDGE_STATS wl_bridge_stats;

void wl_bridge_start(WL_BRIDGE *b, const WL_CARD *card)
{
    (void)card;
    b->wait = 0;
    b->scanned = 0;
    b->off = 0;
    b->joined_as = 0;
}

void wl_bridge_tick(WL_BRIDGE *b, const WL_CARD *card)
{
    u32 secured, state;
    s32 pick;

    if (card->wpa && wpa_apply(card->wpa))
        return;
    state = card->state(card->context);
    /* Joined on an address that is no longer this card's to use, or on its own while the
     * console's has come free (net.c hands it over when its card went without a link):
     * leave, and the loop joins again on the right one. */
    if ((state & WL_CARD_JOINED) && b->nd && b->joined_as &&
        b->joined_as != net_join_mac(b->nd)) {
        wl_bridge_stats.rejoins++;
        b->joined_as = 0;
        card->leave(card->context);
        b->wait = 0;
        return;
    }
    if ((state & (WL_CARD_JOINED | WL_CARD_SCANNING)) || b->off || !wl_config.count)
        return;
    if (b->wait) {
        b->wait--;
        return;
    }
    if (!b->scanned) {
        b->scanned = 1;
        wl_bridge_stats.scans++;
        if (!NT_SUCCESS(card->scan(card->context))) {
            b->scanned = 0;
            b->wait = RETRY_TICKS;
        }
        return;                         /* the results come in while it runs */
    }
    b->scanned = 0;
    b->wait = RETRY_TICKS;
    if (!b->only_read && b->nd && b->nd->drv) {
        char key[24];
        u32 k;

        for (k = 0; k < 16 && b->nd->drv->name[k]; k++)
            key[k] = b->nd->drv->name[k];
        key[k] = '_'; key[k + 1] = 's'; key[k + 2] = 's'; key[k + 3] = 'i'; key[k + 4] = 'd';
        key[k + 5] = 0;
        b->only_len = file_setting(WL_SETTINGS, key, b->only, sizeof(b->only));
        b->only_read = 1;
    }
    pick = wl_pick_only(&secured, b->only, b->only_len);
    if (pick < 0) {
        wl_bridge_stats.missed++;
        if (secured)
            wl_bridge_stats.secured++;
        return;
    }
    wl_bridge_stats.joins++;
    if (b->nd)
        b->joined_as = net_join_mac(b->nd);    /* what the driver's join takes too */
    card->join(card->context, &wl_bss[pick], (const char *)wl_config.pass,
               wl_config.pass_len);
}

u32 wl_bridge_join(WL_BRIDGE *b, const WL_CARD *card, u32 index)
{
    b->off = 0;
    if (index >= wl_stats.bss_count)
        return STATUS_UNSUCCESSFUL;
    return card->join(card->context, &wl_bss[index], (const char *)wl_config.pass,
                      wl_config.pass_len);
}

u32 wl_bridge_leave(WL_BRIDGE *b, const WL_CARD *card)
{
    b->off = 1;
    return card->leave(card->context);
}

/* --- as a WL_MODE (wireless.h) -------------------------------------------------- */

static void mode_start(WL_LINK *l, NET_DEV *nd)
{
    wl_bridge_start(&l->bridge, l->card);
    l->bridge.nd = nd;
}

static void mode_tick(WL_LINK *l)
{
    wl_bridge_tick(&l->bridge, l->card);
}

static void mode_gone(WL_LINK *l)
{
    (void)l;
}

static u32 mode_join(WL_LINK *l, u32 index)
{
    return wl_bridge_join(&l->bridge, l->card, index);
}

static u32 mode_leave(WL_LINK *l)
{
    return wl_bridge_leave(&l->bridge, l->card);
}

const WL_MODE wl_bridge_mode = { mode_start, mode_tick, mode_gone, mode_join, mode_leave };
