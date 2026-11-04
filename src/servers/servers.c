/* The servers in the plugin: started from the worker (src/core/servers.h) once
 * xam's network is up, each with its settings from xedrivers.ini. The servers
 * themselves know nothing of this file or of the plugin (src/servers/common/srv.h).
 *
 * Settings:
 *   nfs        = 0 | 1          (1) serve NFS
 *   nfs_root   = <object path>  (\Device\Harddisk0\Partition1) the exported directory
 *   nfs_allow  = a.b.c.d/n      (anyone) the clients let in
 *   nfs_handles = <count>       (16384) names clients may hold handles to, 80 B each
 *   srv_delay  = <seconds>      (30) after boot before the first start
 */

#include "servers.h"
#include "srv.h"
#if XE_SERVER_NFS
#include "nfs.h"
#endif

#define TICKS_PER_SECOND 50u
#define RETRY_MS         5000u
#define RETRIES          24u

static u32 launched;

/* "a.b.c.d/n" into an address and mask (host order); 0 if it is not one. */
static u32 parse_net(const char *s, u32 *address, u32 *mask)
{
    u32 a = 0, part = 0, parts = 0, bits = 32, digits = 0;

    for (;; s++) {
        if (*s >= '0' && *s <= '9') {
            part = part * 10 + (u32)(*s - '0');
            digits++;
            if (part > 255)
                return 0;
            continue;
        }
        if (!digits)
            return 0;
        a = a << 8 | part;
        parts++;
        part = digits = 0;
        if (*s == '.' && parts < 4)
            continue;
        break;
    }
    if (parts != 4)
        return 0;
    if (*s == '/') {
        for (bits = 0, s++; *s >= '0' && *s <= '9'; s++)
            bits = bits * 10 + (u32)(*s - '0');
        if (bits > 32)
            return 0;
    }
    *mask = bits ? 0xFFFFFFFFu << (32 - bits) : 0;
    *address = a;
    return 1;
}

static void starter(void *arg)
{
    u32 tries = 0;

    (void)arg;
#if XE_SERVER_NFS
    if (srv_setting_u32("nfs", 1)) {
        static char root[SRV_PATH_MAX];
        char allow[32];
        NFS_CONFIG c;

        c.port = c.portmap_port = c.allow = c.allow_mask = 0;
        c.handles = srv_setting_u32("nfs_handles", 0);

        if (!srv_setting("nfs_root", root, sizeof(root))) {
            const char *d = "\\Device\\Harddisk0\\Partition1";
            u32 i;

            for (i = 0; d[i]; i++)
                root[i] = d[i];
            root[i] = 0;
        }
        c.root = root;
        if (srv_setting("nfs_allow", allow, sizeof(allow)))
            parse_net(allow, &c.allow, &c.allow_mask);
        for (tries = 0; tries < RETRIES && nfs_start(&c) != 0; tries++)
            srv_sleep(RETRY_MS);
    }
#endif
    (void)tries;
}

void servers_tick(u32 tick)
{
    static u32 delay = 0xFFFFFFFFu;

    if (launched)
        return;
    if (delay == 0xFFFFFFFFu)           /* the ini read once, at the first tick */
        delay = srv_setting_u32("srv_delay", 30) * TICKS_PER_SECOND;
    if (tick < delay)
        return;
    launched = 1;
    srv_thread(starter, 0);
}
