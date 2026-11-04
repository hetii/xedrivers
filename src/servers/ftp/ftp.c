/* The FTP server: its start, sessions, the command loop, replies and paths; see
 * ftpd.h. RFC 959, RFC 3659, RFC 2428. */

#include "ftpd.h"

volatile FTP_STATS ftp_stats;
FTP_CONFIG ftp_cfg;

static char root[SRV_PATH_MAX], user[64], password[64];
static SESSION sessions[SESSIONS_MAX];

/* === Replies ================================================================ */

void line_str(LINE *l, const char *s)
{
    while (*s && l->n + 3 < sizeof(l->p))
        l->p[l->n++] = *s++;
    l->p[l->n] = 0;
}

void line_u64(LINE *l, u32 hi, u32 lo)
{
    char d[24];
    u32 n = 0;
    u64 v = (u64)hi << 32 | lo;

    do {                                    /* by 10 in 32-bit halves: no __udivdi3 */
        u32 r = 0, h = (u32)(v >> 32), w = (u32)v, q_h, q_l, k;

        q_h = h / 10;
        r = h % 10;
        q_l = 0;
        for (k = 0; k < 32; k++) {
            r = r << 1 | (w >> 31);
            w <<= 1;
            q_l <<= 1;
            if (r >= 10) {
                r -= 10;
                q_l |= 1;
            }
        }
        d[n++] = (char)('0' + r);
        v = (u64)q_h << 32 | q_l;
    } while (v);
    while (n && l->n + 3 < sizeof(l->p))
        l->p[l->n++] = d[--n];
    l->p[l->n] = 0;
}

void line_u32(LINE *l, u32 v)
{
    line_u64(l, 0, v);
}

s32 send_line(SESSION *s, LINE *l)
{
    l->p[l->n++] = '\r';
    l->p[l->n++] = '\n';
    return srv_send(s->ctrl, l->p, l->n);
}

s32 reply3(SESSION *s, u32 code, const char *a, const char *b, const char *c)
{
    LINE l;

    l.n = 0;
    line_u32(&l, code);
    line_str(&l, " ");
    line_str(&l, a);
    line_str(&l, b);
    line_str(&l, c);
    return send_line(s, &l);
}

s32 reply(SESSION *s, u32 code, const char *text)
{
    return reply3(s, code, text, "", "");
}

/* srv.h's error as a reply. */
void reply_error(SESSION *s, s32 e)
{
    switch (e) {
    case SRV_ENOENT: reply(s, 550, "No such file or directory"); break;
    case SRV_EEXIST: reply(s, 550, "Already exists"); break;
    case SRV_ENOTDIR: reply(s, 550, "Not a directory"); break;
    case SRV_EISDIR: reply(s, 550, "Is a directory"); break;
    case SRV_ENOTEMPTY: reply(s, 550, "Directory not empty"); break;
    case SRV_EACCES: reply(s, 550, "Permission denied"); break;
    case SRV_ENOSPC: reply(s, 452, "No space left"); break;
    case SRV_EFBIG: reply(s, 552, "File too large for the disk (4 GiB on FATX)"); break;
    case SRV_ENAMETOOLONG: reply(s, 553, "Name too long"); break;
    default: reply(s, 451, "Error on the disk"); break;
    }
}

/* === Paths and dates ========================================================== */

u32 resolve(SESSION *s, const char *arg, char *out)
{
    char tmp[SRV_PATH_MAX * 2];
    u32 n = 0, i, start;
    const char *p;

    if (arg[0] != '/') {                    /* under the cwd */
        for (p = s->cwd; *p && n + 2 < sizeof(tmp); p++)
            tmp[n++] = *p;
        if (n)
            tmp[n++] = '/';
    }
    for (p = arg; *p && n + 1 < sizeof(tmp); p++)
        tmp[n++] = *p == '\\' ? '/' : *p;
    if (*p)
        return 0;
    tmp[n] = 0;
    /* components one by one into out: "." dropped, ".." takes one back */
    n = 0;
    for (p = tmp; *p;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        for (start = 0; p[start] && p[start] != '/'; start++)
            ;
        if (start == 1 && p[0] == '.') {
        } else if (start == 2 && p[0] == '.' && p[1] == '.') {
            while (n && out[n - 1] != '/')
                n--;
            if (n)
                n--;
        } else {
            if (n + (n ? 1 : 0) + start + 1 > SRV_PATH_MAX)
                return 0;
            if (n)
                out[n++] = '/';
            for (i = 0; i < start; i++)
                out[n++] = p[i];
        }
        p += start;
    }
    out[n] = 0;
    return 1;
}

/* Days since 1970 into a civil date (H. Hinnant's days_from_civil, inverted). */
static void civil(u32 days, u32 *y, u32 *m, u32 *d)
{
    u32 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;

    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static void two(char *p, u32 v)
{
    p[0] = (char)('0' + v / 10 % 10);
    p[1] = (char)('0' + v % 10);
}

void date_mlst(u32 t, char *out)
{
    u32 y, m, d, s = t % 86400;

    civil(t / 86400, &y, &m, &d);
    two(out, y / 100);
    two(out + 2, y % 100);
    two(out + 4, m);
    two(out + 6, d);
    two(out + 8, s / 3600);
    two(out + 10, s / 60 % 60);
    two(out + 12, s % 60);
    out[14] = 0;
}

/* === Sessions ================================================================= */

/* The next command line (CR LF or LF ended) into `line`; < 0 when the client left. */
static s32 read_line(SESSION *s, char *line)
{
    u32 i;
    s32 k;

    for (;;) {
        for (i = 0; i < s->in_len; i++)
            if (s->in[i] == '\n') {
                u32 n = i && s->in[i - 1] == '\r' ? i - 1 : i;

                mem_copy(line, s->in, n);
                line[n] = 0;
                s->in_len -= i + 1;
                mem_copy(s->in, s->in + i + 1, s->in_len);
                return (s32)n;
            }
        if (s->in_len == LINE_MAX)          /* too long: thrown away */
            s->in_len = 0;
        if (srv_wait(s->ctrl, IDLE_MS) <= 0) {
            reply(s, 421, "Idle too long");
            return -1;
        }
        k = srv_recv(s->ctrl, s->in + s->in_len, LINE_MAX - s->in_len);
        if (k <= 0)
            return -1;
        s->in_len += (u32)k;
    }
}

/* Before a login only these: the rest are answered 530. */
static u32 before_login(const char *verb)
{
    static const char *const ok[] = { "USER", "PASS", "QUIT", "SYST", "FEAT", "NOOP",
                                      "AUTH", "OPTS", 0 };
    u32 i;

    for (i = 0; ok[i]; i++)
        if (str_eq(verb, ok[i]))
            return 1;
    return 0;
}

static void session(void *arg)
{
    SESSION *s = (SESSION *)arg;
    char line[LINE_MAX], verb[8];
    const char *a;
    u32 i;

    reply(s, 220, "xedrivers FTP ready");
    while (read_line(s, line) >= 0) {
        for (i = 0; line[i] && line[i] != ' ' && i + 1 < sizeof(verb); i++)
            verb[i] = line[i] >= 'a' && line[i] <= 'z' ? (char)(line[i] - 32) : line[i];
        verb[i] = 0;
        for (a = line + i; *a && *a != ' '; a++)
            ;
        if (*a == ' ')
            a++;
        ftp_stats.commands++;
        if (str_eq(verb, "QUIT")) {
            reply(s, 221, "Goodbye");
            break;
        }
        if (!s->logged_in && !before_login(verb)) {
            reply(s, 530, "Log in first");
            continue;
        }
        if (!transfer(s, verb, a) && !command(s, verb, a))
            reply(s, 502, "Command not implemented");
    }
    data_reset(s);
    srv_close(s->ctrl);
    srv_lock();
    ftp_stats.active--;
    s->busy = 0;
    srv_unlock();
}

static u32 allowed(u32 peer)
{
    return !ftp_cfg.allow_mask ||
           (peer & ftp_cfg.allow_mask) == (ftp_cfg.allow & ftp_cfg.allow_mask);
}

static SESSION *take_session(void)
{
    SESSION *s = 0;
    u32 i;

    srv_lock();
    for (i = 0; i < SESSIONS_MAX; i++)
        if (!sessions[i].busy) {
            s = &sessions[i];
            s->busy = 1;
            break;
        }
    srv_unlock();
    if (s && !s->buf)
        s->buf = (u8 *)srv_alloc(DATA_BUF);
    if (s && !s->buf) {
        s->busy = 0;
        return 0;
    }
    return s;
}

static void listener(void *arg)
{
    SRV_SOCK l = (SRV_SOCK)(long)arg, c;
    SESSION *s;
    u32 peer;

    for (;;) {
        c = srv_accept(l, &peer);
        if (c < 0) {
            srv_sleep(100);
            continue;
        }
        s = allowed(peer) ? take_session() : 0;
        if (!s) {
            ftp_stats.refused++;
            srv_send(c, "421 Not now\r\n", 13);
            srv_close(c);
            continue;
        }
        s->ctrl = c;
        s->peer = peer;
        s->logged_in = s->user_ok = s->renaming = 0;
        s->cwd[0] = 0;
        s->rest_lo = s->rest_hi = 0;
        s->data_mode = DATA_NONE;
        s->pasv = -1;
        s->in_len = 0;
        srv_lock();
        ftp_stats.sessions++;
        ftp_stats.active++;
        srv_unlock();
        if (srv_thread(session, s)) {
            srv_close(c);
            s->busy = 0;
        }
    }
}

s32 ftp_start(const FTP_CONFIG *config)
{
    SRV_SOCK l;

    if (ftp_stats.started)
        return 0;
    ftp_cfg = *config;
    if (!ftp_cfg.port)
        ftp_cfg.port = FTP_PORT_DEFAULT;
    str_copy(root, config->root, sizeof(root));
    str_copy(user, config->user ? config->user : "", sizeof(user));
    str_copy(password, config->password ? config->password : "", sizeof(password));
    ftp_cfg.root = root;
    ftp_cfg.user = user;
    ftp_cfg.password = password;
    l = srv_listen(ftp_cfg.port, SESSIONS_MAX);
    if (l < 0)
        return -1;                          /* the network not up yet: asked again later */
    if (srv_thread(listener, (void *)(long)l)) {
        srv_close(l);
        return -1;
    }
    ftp_stats.started = 1;
    return 0;
}
