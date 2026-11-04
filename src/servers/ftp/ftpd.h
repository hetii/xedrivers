/* The FTP server's internals, shared by its files; users of the server have ftp.h.
 *   ftp.c        start, sessions, the command loop, replies, paths
 *   commands.c   login, directories, names, file information
 *   transfer.c   data connections, RETR/STOR/APPE, LIST/NLST/MLSD */

#ifndef XE_FTPD_H
#define XE_FTPD_H

#include "ftp.h"
#include "str.h"

#define SESSIONS_MAX    8
#define LINE_MAX        1024u
#define DATA_BUF        65536u          /* one srv_read / srv_write a time */
#define IDLE_MS         600000u         /* a control connection silent this long goes */
#define ACCEPT_MS       30000u          /* for the client to open a passive connection */
#define STALL_MS        60000u          /* a transfer with nothing coming */

extern FTP_CONFIG ftp_cfg;

#define DATA_NONE       0u
#define DATA_PASSIVE    1u
#define DATA_ACTIVE     2u

typedef struct {
    SRV_SOCK ctrl;
    u32 peer;                           /* host order */
    u32 logged_in;
    u32 user_ok;
    char cwd[SRV_PATH_MAX];             /* "" the root, no leading '/' */
    char rename_from[SRV_PATH_MAX];
    u32 renaming;
    u32 rest_lo, rest_hi;               /* REST: where the next transfer starts */
    u32 data_mode;
    SRV_SOCK pasv;                      /* the passive listener */
    u32 port_address, port_port;        /* PORT / EPRT */
    char in[LINE_MAX];                  /* the control connection's input */
    u32 in_len;
    u8 *buf;                            /* a transfer's data, DATA_BUF bytes */
    volatile u32 busy;
} SESSION;

/* --- ftp.c: replies and paths ------------------------------------------- */

/* A reply line built in pieces, sent with reply_end. */
typedef struct {
    char p[SRV_PATH_MAX + 128];
    u32 n;
} LINE;

void line_str(LINE *l, const char *s);
void line_u32(LINE *l, u32 v);
void line_u64(LINE *l, u32 hi, u32 lo);
/* "<code> <text>\r\n" in one call; < 0 when the client is gone. */
s32  reply(SESSION *s, u32 code, const char *text);
/* "<code> <a><b><c>\r\n" */
s32  reply3(SESSION *s, u32 code, const char *a, const char *b, const char *c);
s32  send_line(SESSION *s, LINE *l);
/* srv.h's error as a 4xx/5xx reply. */
void reply_error(SESSION *s, s32 e);

/* The server path of a client's argument (absolute or under the cwd), "." and
 * ".." resolved, never above the root; 0 if too long. */
u32  resolve(SESSION *s, const char *arg, char *out);

/* The date of `t` (seconds since 1970) as YYYYMMDDHHMMSS into `out` (15 bytes). */
void date_mlst(u32 t, char *out);

/* --- commands.c ------------------------------------------------------------ */

/* A command not about transfers; 0 if it is not one of them. */
u32  command(SESSION *s, const char *verb, const char *arg);

/* --- transfer.c ------------------------------------------------------------ */

/* A transfer command, or PASV/EPSV/PORT/EPRT; 0 if it is not one of them. */
u32  transfer(SESSION *s, const char *verb, const char *arg);
/* The session's passive listener closed, the data setting forgotten. */
void data_reset(SESSION *s);

#endif /* XE_FTPD_H */
