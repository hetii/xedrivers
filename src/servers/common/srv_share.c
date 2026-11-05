/* The receive windows of the servers' data connections, sharing xnet's pool. What
 * a connection has not read yet lies in that pool (about 1 MB with the plugin's
 * extension), and windows that add up to more stop every connection: twelve of
 * 128 KB broke uploads (2026-11-05). So each connection gets a share of a budget,
 * as much as `srv_window_kb` while few are receiving, and less as more come.
 *
 * srv_windows_kb = 0, the default, leaves the sockets to xnet's defaults. Pure code
 * on srv.h: the console's platform and the host tests' both use it. */

#include "srv.h"

#define SHARES      32u
#define WINDOW_MIN  (32u << 10)

/* Read by tools on the host: the window given last, the budget and the most for one
 * connection (0 until srv_share_init). */
volatile u32 srv_share_last_window, srv_share_budget, srv_share_most;

static SRV_SOCK shares[SHARES];
static u32 count;

/* Everyone's window for the present count. setsockopt outside the lock: a copy of
 * the list, which a connection going at the same moment leaves at worst one stale
 * call on a socket that is closing. */
static void apply(void)
{
    SRV_SOCK list[SHARES];
    u32 i, n, window;

    srv_lock();
    n = count;
    for (i = 0; i < n; i++)
        list[i] = shares[i];
    srv_unlock();
    if (!n)
        return;
    window = srv_share_budget / n;
    window = window > srv_share_most ? srv_share_most : window < WINDOW_MIN ? WINDOW_MIN : window;
    srv_share_last_window = window;
    for (i = 0; i < n; i++)
        srv_receive_window(list[i], window);
}

void srv_share_init(void)
{
    srv_share_budget = srv_setting_u32("srv_windows_kb", 0) << 10;
    srv_share_most = srv_setting_u32("srv_window_kb", 128) << 10;
}

void srv_share_add(SRV_SOCK s)
{
    u32 added = 0;

    if (!srv_share_budget)
        return;
    srv_lock();
    if (count < SHARES) {
        shares[count++] = s;
        added = 1;
    }
    srv_unlock();
    if (added)
        apply();
}

void srv_share_remove(SRV_SOCK s)
{
    u32 i, removed = 0;

    if (!srv_share_budget)
        return;
    srv_lock();
    for (i = 0; i < count; i++)
        if (shares[i] == s) {
            shares[i] = shares[--count];
            removed = 1;
            break;
        }
    srv_unlock();
    if (removed)
        apply();
}
