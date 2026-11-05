/* FTP commands about the session and names: login, the working directory,
 * file information, removing, making and renaming. */

#include "ftpd.h"

/* The argument's path; replies 553 and gives 0 when it has none. */
static u32 path_arg(SESSION *s, const char *arg, char *out)
{
    if (!resolve(s, arg, out)) {
        reply(s, 553, "Name too long");
        return 0;
    }
    return 1;
}

static void quoted_path(SESSION *s, u32 code, const char *path, const char *text)
{
    LINE l;

    l.n = 0;
    line_u32(&l, code);
    line_str(&l, " \"/");
    line_str(&l, path);
    line_str(&l, "\" ");
    line_str(&l, text);
    send_line(s, &l);
}

static void login(SESSION *s, const char *verb, const char *arg)
{
    if (str_eq(verb, "USER")) {
        s->logged_in = 0;
        s->user_ok = !ftp_cfg.user[0] || str_eq(arg, ftp_cfg.user);
        if (!ftp_cfg.user[0]) {
            s->logged_in = 1;               /* no user configured: anyone */
            reply(s, 230, "Logged in");
        } else {
            reply(s, 331, "Password required");
        }
        return;
    }
    if (s->logged_in) {                     /* PASS after an open USER */
        reply(s, 230, "Logged in");
        return;
    }
    if (s->user_ok && str_eq(arg, ftp_cfg.password)) {
        s->logged_in = 1;
        reply(s, 230, "Logged in");
    } else {
        ftp_stats.bad_logins++;
        srv_sleep(1000);                    /* guessing made slow */
        reply(s, 530, "Login incorrect");
    }
}

static void feat(SESSION *s)
{
    static const char text[] =
        "211-Features:\r\n"
        " SIZE\r\n MDTM\r\n REST STREAM\r\n EPSV\r\n EPRT\r\n PASV\r\n UTF8\r\n"
        " MLST type*;size*;modify*;\r\n"
        "211 End\r\n";

    srv_send(s->ctrl, text, sizeof(text) - 1);
}

/* MLST's facts of one name, also used by MLSD (transfer.c has its own line). */
static void mlst(SESSION *s, const char *path)
{
    char date[16];
    SRV_STAT st;
    LINE l;
    s32 e = srv_stat(ftp_cfg.root, path, &st);

    if (e != SRV_OK) {
        reply_error(s, e);
        return;
    }
    date_mlst(st.mtime, date);
    srv_send(s->ctrl, "250-Listing\r\n", 13);
    l.n = 0;
    line_str(&l, st.is_dir ? " type=dir;size=" : " type=file;size=");
    line_u64(&l, st.size_hi, st.size_lo);
    line_str(&l, ";modify=");
    line_str(&l, date);
    line_str(&l, "; /");
    line_str(&l, path);
    send_line(s, &l);
    reply(s, 250, "End");
}

static void cwd(SESSION *s, const char *arg)
{
    char path[SRV_PATH_MAX];
    SRV_STAT st;
    s32 e;

    if (!path_arg(s, arg, path))
        return;
    e = srv_stat(ftp_cfg.root, path, &st);
    if (e == SRV_OK && !st.is_dir)
        e = SRV_ENOTDIR;
    if (e != SRV_OK) {
        reply_error(s, e);
        return;
    }
    str_copy(s->cwd, path, SRV_PATH_MAX);
    reply(s, 250, "Directory changed");
}

static void info(SESSION *s, const char *verb, const char *path)
{
    char date[16];
    SRV_STAT st;
    LINE l;
    s32 e = srv_stat(ftp_cfg.root, path, &st);

    if (e == SRV_OK && st.is_dir && str_eq(verb, "SIZE"))
        e = SRV_EISDIR;
    if (e != SRV_OK) {
        reply_error(s, e);
        return;
    }
    l.n = 0;
    line_str(&l, "213 ");
    if (str_eq(verb, "SIZE")) {
        line_u64(&l, st.size_hi, st.size_lo);
    } else {
        date_mlst(st.mtime, date);
        line_str(&l, date);
    }
    send_line(s, &l);
}

u32 command(SESSION *s, const char *verb, const char *arg)
{
    char path[SRV_PATH_MAX];
    s32 e;

    if (str_eq(verb, "USER") || str_eq(verb, "PASS")) {
        login(s, verb, arg);
    } else if (str_eq(verb, "SYST")) {
        reply(s, 215, "UNIX Type: L8");
    } else if (str_eq(verb, "FEAT")) {
        feat(s);
    } else if (str_eq(verb, "OPTS")) {
        reply(s, 200, "OK");                /* UTF8 ON: names are passed as they are */
    } else if (str_eq(verb, "NOOP")) {
        reply(s, 200, "OK");
    } else if (str_eq(verb, "AUTH")) {
        reply(s, 502, "No TLS here");
    } else if (str_eq(verb, "TYPE") || str_eq(verb, "MODE") || str_eq(verb, "STRU")) {
        reply(s, 200, "OK");                /* every type is sent as it is */
    } else if (str_eq(verb, "ALLO")) {
        reply(s, 202, "Not needed");
    } else if (str_eq(verb, "PWD") || str_eq(verb, "XPWD")) {
        quoted_path(s, 257, s->cwd, "is the current directory");
    } else if (str_eq(verb, "CWD") || str_eq(verb, "XCWD")) {
        cwd(s, arg);
    } else if (str_eq(verb, "CDUP") || str_eq(verb, "XCUP")) {
        cwd(s, "..");
    } else if (str_eq(verb, "SIZE") || str_eq(verb, "MDTM")) {
        if (path_arg(s, arg, path))
            info(s, verb, path);
    } else if (str_eq(verb, "MLST")) {
        if (path_arg(s, arg, path))
            mlst(s, path);
    } else if (str_eq(verb, "DELE") || str_eq(verb, "RMD") || str_eq(verb, "XRMD")) {
        if (!path_arg(s, arg, path))
            return 1;
        e = verb[0] == 'D' ? srv_unlink(ftp_cfg.root, path) : srv_rmdir(ftp_cfg.root, path);
        if (e != SRV_OK)
            reply_error(s, e);
        else
            reply(s, 250, verb[0] == 'D' ? "File deleted" : "Directory removed");
    } else if (str_eq(verb, "MKD") || str_eq(verb, "XMKD")) {
        if (!path_arg(s, arg, path))
            return 1;
        e = srv_mkdir(ftp_cfg.root, path);
        if (e != SRV_OK)
            reply_error(s, e);
        else
            quoted_path(s, 257, path, "created");
    } else if (str_eq(verb, "RNFR")) {
        SRV_STAT st;

        if (!path_arg(s, arg, s->rename_from))
            return 1;
        e = srv_stat(ftp_cfg.root, s->rename_from, &st);
        s->renaming = e == SRV_OK;
        if (e != SRV_OK)
            reply_error(s, e);
        else
            reply(s, 350, "Ready for RNTO");
    } else if (str_eq(verb, "RNTO")) {
        if (!s->renaming) {
            reply(s, 503, "RNFR first");
            return 1;
        }
        s->renaming = 0;
        if (!path_arg(s, arg, path))
            return 1;
        e = srv_rename(ftp_cfg.root, s->rename_from, path);
        if (e != SRV_OK)
            reply_error(s, e);
        else
            reply(s, 250, "Renamed");
    } else if (str_eq(verb, "HELP")) {
        reply(s, 214, "See FEAT");
    } else {
        return 0;
    }
    return 1;
}
