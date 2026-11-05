/* FTP's data connections and what goes over them: PASV, EPSV, PORT, EPRT; RETR,
 * STOR, APPE, REST; LIST, NLST, MLSD. */

#include "ftpd.h"

/* === Data connections ========================================================= */

void data_reset(SESSION *s)
{
    if (s->pasv >= 0)
        srv_close(s->pasv);
    s->pasv = -1;
    s->data_mode = DATA_NONE;
}

/* A listener for the client to come to, on the next port of the passive range;
 * its port, 0 on failure. Not on a port xnet picks: its own are 1025-1255, 231
 * of them in turn (2026-11-05), and after a lap the port given could still be a
 * data connection's not yet closed -- one transfer in about 230 failed. */
static u32 passive(SESSION *s)
{
    static u32 next;
    u32 tries, port = 0;

    data_reset(s);
    for (tries = 0; tries < 16; tries++) {
        srv_lock();
        port = ftp_cfg.pasv_low + next++ % ftp_cfg.pasv_count;
        srv_unlock();
        s->pasv = srv_listen(port, 1);
        if (s->pasv >= 0)
            break;
    }
    if (s->pasv < 0) {
        s->pasv = -1;
        return 0;
    }
    s->data_mode = DATA_PASSIVE;
    return port;
}

/* "h1,h2,h3,h4,p1,p2" (PORT) into an address and port; 0 if it is not one. */
static u32 parse_port(const char *a, u32 *address, u32 *port)
{
    u32 v[6], i, n;

    for (i = 0; i < 6; i++) {
        if (*a < '0' || *a > '9')
            return 0;
        for (n = 0; *a >= '0' && *a <= '9'; a++)
            n = n * 10 + (u32)(*a - '0');
        if (n > 255)
            return 0;
        v[i] = n;
        if (i < 5 && *a++ != ',')
            return 0;
    }
    *address = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
    *port = v[4] << 8 | v[5];
    return 1;
}

/* "|1|a.b.c.d|port|" (EPRT, IPv4 only). */
static u32 parse_eprt(const char *a, u32 *address, u32 *port)
{
    u32 v[4], i, n;
    char d = *a++;

    if (!d || *a++ != '1' || *a++ != d)
        return 0;
    for (i = 0; i < 4; i++) {
        for (n = 0; *a >= '0' && *a <= '9'; a++)
            n = n * 10 + (u32)(*a - '0');
        if (n > 255 || *a++ != (i < 3 ? '.' : d))
            return 0;
        v[i] = n;
    }
    for (n = 0; *a >= '0' && *a <= '9'; a++)
        n = n * 10 + (u32)(*a - '0');
    if (*a != d || !n || n > 65535)
        return 0;
    *address = v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
    *port = n;
    return 1;
}

/* The data connection, after the 150 that announces it; < 0 (425 sent) if none. */
static SRV_SOCK open_data(SESSION *s, const char *what)
{
    SRV_SOCK d = -1;
    u32 peer;
    s32 w;

    reply3(s, 150, "Opening data connection for ", what, "");
    if (s->data_mode == DATA_PASSIVE) {
        /* a select that fails (not one that times out) does not stop the accept:
         * the client may well be there (1 data connection lost in 83 rounds) */
        w = srv_wait(s->pasv, ACCEPT_MS);
        if (w < 0)
            ftp_stats.wait_failed++;
        if (w)
            d = srv_accept(s->pasv, &peer);
        if (w && d < 0)
            ftp_stats.accept_failed++;
    } else if (s->data_mode == DATA_ACTIVE) {
        d = srv_connect(s->port_address, s->port_port);
    }
    data_reset(s);
    if (d < 0) {
        ftp_stats.data_failed++;
        reply(s, 425, "No data connection");
    }
    return d;
}

static u32 setup(SESSION *s, const char *verb, const char *arg)
{
    LINE l;
    u32 port, address;

    if (str_eq(verb, "PASV")) {
        port = passive(s);
        address = srv_local_address(s->ctrl);
        if (!port) {
            reply(s, 425, "No passive port");
            return 1;
        }
        l.n = 0;
        line_str(&l, "227 Entering Passive Mode (");
        line_u32(&l, address >> 24);
        line_str(&l, ",");
        line_u32(&l, address >> 16 & 0xFF);
        line_str(&l, ",");
        line_u32(&l, address >> 8 & 0xFF);
        line_str(&l, ",");
        line_u32(&l, address & 0xFF);
        line_str(&l, ",");
        line_u32(&l, port >> 8);
        line_str(&l, ",");
        line_u32(&l, port & 0xFF);
        line_str(&l, ")");
        send_line(s, &l);
    } else if (str_eq(verb, "EPSV")) {
        if (str_eq(arg, "ALL") || str_eq(arg, "all")) {
            reply(s, 200, "EPSV ALL");
            return 1;
        }
        port = passive(s);
        if (!port) {
            reply(s, 425, "No passive port");
            return 1;
        }
        l.n = 0;
        line_str(&l, "229 Entering Extended Passive Mode (|||");
        line_u32(&l, port);
        line_str(&l, "|)");
        send_line(s, &l);
    } else if (str_eq(verb, "PORT") || str_eq(verb, "EPRT")) {
        data_reset(s);
        if (verb[0] == 'P' ? !parse_port(arg, &s->port_address, &s->port_port)
                           : !parse_eprt(arg, &s->port_address, &s->port_port)) {
            reply(s, verb[0] == 'P' ? 501 : 522, "Bad address");
            return 1;
        }
        s->data_mode = DATA_ACTIVE;
        reply(s, 200, "OK");
    } else if (str_eq(verb, "REST")) {
        u64 v = 0;

        for (; *arg >= '0' && *arg <= '9'; arg++)
            v = v * 10 + (u32)(*arg - '0');
        s->rest_lo = (u32)v;
        s->rest_hi = (u32)(v >> 32);
        reply(s, 350, "Restarting");
    } else {
        return 0;
    }
    return 1;
}

/* === Files ===================================================================== */

static void add(volatile u32 *lo, volatile u32 *hi, u32 n)
{
    if ((*lo += n) < n)
        (*hi)++;
}

static void retr(SESSION *s, const char *path)
{
    u64 at = (u64)s->rest_hi << 32 | s->rest_lo;
    SRV_FILE f;
    SRV_SOCK d;
    s32 e, n;

    s->rest_lo = s->rest_hi = 0;
    e = srv_open(ftp_cfg.root, path, SRV_READ, &f);
    if (e != SRV_OK) {
        data_reset(s);
        reply_error(s, e);
        return;
    }
    d = open_data(s, path);
    if (d < 0) {
        srv_fclose(f);
        return;
    }
    for (;;) {
        n = srv_read(f, (u32)at, (u32)(at >> 32), s->buf, DATA_BUF);
        if (n < 0)
            ftp_stats.read_failed++;
        if (n <= 0)
            break;
        if (srv_send(d, s->buf, (u32)n) < 0) {
            ftp_stats.send_failed++;
            n = -1;
            break;
        }
        at += (u32)n;
        add(&ftp_stats.sent_lo, &ftp_stats.sent_hi, (u32)n);
    }
    srv_fclose(f);
    srv_close(d);
    ftp_stats.retrs++;
    if (n < 0)
        reply(s, 426, "Transfer aborted");
    else
        reply(s, 226, "Transfer complete");
}

/* STOR from REST's offset (the file kept) or from 0 (the file emptied); APPE at
 * the end. */
static void stor(SESSION *s, const char *path, u32 append)
{
    u64 at = (u64)s->rest_hi << 32 | s->rest_lo;
    SRV_FILE f;
    SRV_STAT st;
    SRV_SOCK d;
    s32 e, n, w = 0;

    s->rest_lo = s->rest_hi = 0;
    e = srv_open(ftp_cfg.root, path, SRV_WRITE | SRV_CREATE | (append || at ? 0 : SRV_TRUNC),
                 &f);
    if (e == SRV_OK && append && srv_stat(ftp_cfg.root, path, &st) == SRV_OK)
        at = (u64)st.size_hi << 32 | st.size_lo;
    if (e != SRV_OK) {
        data_reset(s);
        reply_error(s, e);
        return;
    }
    d = open_data(s, path);
    if (d < 0) {
        srv_fclose(f);
        return;
    }
    srv_share_add(d);
    for (;;) {
        if (srv_wait(d, STALL_MS) <= 0) {
            n = -1;
            break;
        }
        n = srv_recv(d, s->buf, DATA_BUF);
        if (n <= 0)
            break;
        w = srv_write(f, (u32)at, (u32)(at >> 32), s->buf, (u32)n);
        if (w != n)
            break;
        at += (u32)n;
        add(&ftp_stats.received_lo, &ftp_stats.received_hi, (u32)n);
    }
    e = srv_flush(f);
    srv_fclose(f);
    srv_share_remove(d);
    srv_close(d);
    ftp_stats.stors++;
    if (n < 0)
        reply(s, 426, "Transfer aborted");
    else if (n > 0 || e != SRV_OK)
        reply_error(s, w < 0 ? w : e != SRV_OK ? e : SRV_ENOSPC);
    else
        reply(s, 226, "Transfer complete");
}

/* === Listings ================================================================== */

static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

/* "Mon DD  YYYY" or "Mon DD HH:MM" (within half a year), as ls -l writes it. */
static void ls_date(LINE *l, u32 t)
{
    char date[16], m[4];
    u32 now = srv_now(), mon;

    date_mlst(t, date);                     /* YYYYMMDDHHMMSS */
    mon = (u32)(date[4] - '0') * 10 + (u32)(date[5] - '0');
    m[0] = months[(mon - 1) * 3];
    m[1] = months[(mon - 1) * 3 + 1];
    m[2] = months[(mon - 1) * 3 + 2];
    m[3] = 0;
    line_str(l, m);
    line_str(l, date[6] == '0' ? "  " : " ");
    if (date[6] != '0') {
        char d1[2] = { date[6], 0 };

        line_str(l, d1);
    }
    {
        char d2[2] = { date[7], 0 };

        line_str(l, d2);
    }
    if (t + 15552000u > now && t < now + 86400u) {
        char hm[7] = { ' ', date[8], date[9], ':', date[10], date[11], 0 };

        line_str(l, hm);
    } else {
        char y[7] = { ' ', ' ', date[0], date[1], date[2], date[3], 0 };

        line_str(l, y);
    }
}

#define LIST_LS     0u
#define LIST_NAMES  1u
#define LIST_MLSD   2u

static void entry(LINE *l, u32 kind, const char *name, const SRV_STAT *st)
{
    char date[16];

    l->n = 0;
    if (kind == LIST_LS) {
        line_str(l, st->is_dir ? "drwxrwxrwx 1 root root " : "-rw-rw-rw- 1 root root ");
        line_u64(l, st->size_hi, st->size_lo);
        line_str(l, " ");
        ls_date(l, st->mtime);
        line_str(l, " ");
    } else if (kind == LIST_MLSD) {
        date_mlst(st->mtime, date);
        line_str(l, st->is_dir ? "type=dir;size=" : "type=file;size=");
        line_u64(l, st->size_hi, st->size_lo);
        line_str(l, ";modify=");
        line_str(l, date);
        line_str(l, "; ");
    }
    line_str(l, name);
    l->p[l->n++] = '\r';
    l->p[l->n++] = '\n';
}

static void list(SESSION *s, const char *arg, u32 kind)
{
    char path[SRV_PATH_MAX], name[SRV_NAME_MAX + 1];
    SRV_STAT st;
    SRV_DIR dir = 0;
    SRV_SOCK d;
    LINE l;
    u32 used = 0, one = 0, k;
    s32 e;

    while (*arg == '-') {                   /* ls options ("-la"): ignored */
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    if (!resolve(s, arg, path)) {
        data_reset(s);
        reply(s, 553, "Name too long");
        return;
    }
    e = srv_stat(ftp_cfg.root, path, &st);
    if (e == SRV_OK && !st.is_dir) {
        if (kind == LIST_MLSD)
            e = SRV_ENOTDIR;
        one = 1;
    }
    if (e == SRV_OK && !one)
        e = srv_opendir(ftp_cfg.root, path, &dir);
    if (e != SRV_OK) {
        data_reset(s);
        reply_error(s, e);
        return;
    }
    d = open_data(s, "the listing");
    if (d < 0) {
        srv_closedir(dir);
        return;
    }
    if (one) {
        for (k = str_len(path); k && path[k - 1] != '/'; k--)
            ;
        entry(&l, kind, path + k, &st);
        srv_send(d, l.p, l.n);
    } else {
        while (srv_readdir(dir, name, sizeof(name), &st) > 0) {
            entry(&l, kind, name, &st);
            if (used + l.n > DATA_BUF) {
                if (srv_send(d, s->buf, used) < 0)
                    break;
                used = 0;
            }
            mem_copy(s->buf + used, l.p, l.n);
            used += l.n;
        }
        if (used)
            srv_send(d, s->buf, used);
        srv_closedir(dir);
    }
    srv_close(d);
    ftp_stats.lists++;
    reply(s, 226, "Listing complete");
}

/* === Commands ================================================================== */

u32 transfer(SESSION *s, const char *verb, const char *arg)
{
    char path[SRV_PATH_MAX];
    u32 retrieve, store, append;

    if (setup(s, verb, arg))
        return 1;
    if (str_eq(verb, "LIST") || str_eq(verb, "NLST") || str_eq(verb, "MLSD")) {
        if (s->data_mode == DATA_NONE) {
            reply(s, 425, "PASV or PORT first");
            return 1;
        }
        list(s, arg, verb[0] == 'L' ? LIST_LS : verb[0] == 'N' ? LIST_NAMES : LIST_MLSD);
        return 1;
    }
    retrieve = str_eq(verb, "RETR");
    store = str_eq(verb, "STOR");
    append = str_eq(verb, "APPE");
    if (!retrieve && !store && !append)
        return 0;
    if (s->data_mode == DATA_NONE) {
        reply(s, 425, "PASV or PORT first");
        return 1;
    }
    if (!resolve(s, arg, path) || !path[0]) {
        data_reset(s);
        reply(s, 553, "Bad name");
        return 1;
    }
    if (retrieve)
        retr(s, path);
    else
        stor(s, path, append);
    return 1;
}
