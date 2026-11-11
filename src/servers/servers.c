/* The servers in the plugin: started from the worker (src/core/servers.h) once
 * xam's network is up, each with its settings from xedrivers.ini. The servers
 * themselves know nothing of this file or of the plugin (src/servers/common/srv.h).
 *
 * Settings:
 *   nfs        = 0 | 1          (1) serve NFS
 *   nfs_root   = * | <object path>  (*) every volume (Hdd1, Usb0...), or one directory
 *   nfs_allow  = a.b.c.d/n      (anyone) the clients let in
 *   nfs_handles = <count>       (4096) names clients may hold handles to, 84 B each
 *   ftp        = 0 | 1          (1) serve FTP
 *   ftp_root   = * | <object path>  (*) every volume, or one directory, served as /
 *   ftp_port   = <port>         (21) taken from another server only once it lets go
 *   ftp_user, ftp_pass          (none) the login; without a user any login is taken
 *   ftp_allow  = a.b.c.d/n      (anyone) the clients let in
 *   ftp_pasv_low, ftp_pasv_count (50000, 1000) the passive ports, taken in turn
 *   srv_delay  = <seconds>      (30) after boot before the first start
 */

#include "servers.h"
#include "srv.h"
#if XE_SERVER_XENFS
#include "nfs.h"
#endif
#if XE_SERVER_XEFTP
#include "ftp.h"
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

/* A setting, or `fallback` when the ini has none. */
static void setting(const char *key, char *out, u32 size, const char *fallback)
{
    u32 i;

    if (srv_setting(key, out, size))
        return;
    for (i = 0; fallback[i] && i + 1 < size; i++)
        out[i] = fallback[i];
    out[i] = 0;
}

/* Every volume the console has, Hdd1, Usb0... (src/servers/common/srv_file.c). */
#define EVERY_VOLUME "*"

#if XE_SERVER_XENFS
static void start_nfs(void *arg)
{
    static char root[SRV_PATH_MAX];
    char allow[32];
    NFS_CONFIG c;
    u32 tries;

    (void)arg;
    if (!srv_setting_u32("nfs", 1))
        return;
    c.port = c.portmap_port = c.allow = c.allow_mask = 0;
    c.handles = srv_setting_u32("nfs_handles", 0);
    setting("nfs_root", root, sizeof(root), EVERY_VOLUME);
    c.root = root;
    if (srv_setting("nfs_allow", allow, sizeof(allow)))
        parse_net(allow, &c.allow, &c.allow_mask);
    for (tries = 0; tries < RETRIES && nfs_start(&c) != 0; tries++)
        srv_sleep(RETRY_MS);
}
#endif

#if XE_SERVER_XEFTP
static void start_ftp(void *arg)
{
    static char root[SRV_PATH_MAX], user[64], password[64];
    char allow[32];
    FTP_CONFIG c;
    u32 tries;

    (void)arg;
    if (!srv_setting_u32("ftp", 1))
        return;
    c.port = srv_setting_u32("ftp_port", FTP_PORT_DEFAULT);
    c.allow = c.allow_mask = 0;
    c.pasv_low = srv_setting_u32("ftp_pasv_low", 0);
    c.pasv_count = srv_setting_u32("ftp_pasv_count", 0);
    setting("ftp_root", root, sizeof(root), EVERY_VOLUME);
    setting("ftp_user", user, sizeof(user), "");
    setting("ftp_pass", password, sizeof(password), "");
    c.root = root;
    c.user = user;
    c.password = password;
    if (srv_setting("ftp_allow", allow, sizeof(allow)))
        parse_net(allow, &c.allow, &c.allow_mask);
    for (tries = 0; tries < RETRIES && ftp_start(&c) != 0; tries++)
        srv_sleep(RETRY_MS);
}
#endif

/* Each server started on a thread of its own: one waiting for its port (another
 * FTP server on 21, say) keeps no other waiting. */
static void starter(void *arg)
{
    (void)arg;
#if XE_SERVER_XENFS
    srv_thread(start_nfs, 0);
#endif
#if XE_SERVER_XEFTP
    srv_thread(start_ftp, 0);
#endif
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
