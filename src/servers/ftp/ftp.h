/* An FTP server (RFC 959, with RFC 3659's SIZE, MDTM, REST STREAM, MLST/MLSD and
 * RFC 2428's EPSV/EPRT), on the platform of src/servers/common/srv.h only.
 *
 * One thread a session, a data connection at a time: passive (PASV, EPSV) or
 * active (PORT, EPRT), binary or ASCII type both sent as they are. Paths are the
 * root's, '/' its top; ".." stops there. A login is checked only when a user is
 * configured, as ftpdll's is (a LAN server).
 */

#ifndef XE_FTP_H
#define XE_FTP_H

#include "srv.h"

#define FTP_PORT_DEFAULT 21u
#define FTP_PASV_LOW     50000u
#define FTP_PASV_COUNT   1000u

typedef struct {
    const char *root;       /* the directory served as "/" */
    u32 port;               /* 0 for FTP_PORT_DEFAULT */
    const char *user;       /* 0 or "": any login is taken */
    const char *password;
    u32 allow;              /* a client address (host order) and */
    u32 allow_mask;         /* its mask; 0 / 0 lets anyone in */
    u32 pasv_low;           /* passive ports, taken in turn; 0 for FTP_PASV_LOW */
    u32 pasv_count;         /* how many; 0 for FTP_PASV_COUNT */
} FTP_CONFIG;

typedef struct {
    u32 started;
    u32 sessions;
    u32 active;
    u32 refused;            /* clients outside ftp_allow, or a session too many */
    u32 bad_logins;
    u32 commands;
    u32 retrs, stors, lists;
    u32 sent_lo, sent_hi;
    u32 received_lo, received_hi;
    u32 data_failed;        /* a data connection not made */
    u32 wait_failed;        /* of them: select on the passive listener failed */
    u32 accept_failed;      /* of them: accept failed after the wait */
} FTP_STATS;

extern volatile FTP_STATS ftp_stats;

/* Start the listener on a thread of its own; 0 on success. The config is copied. */
s32 ftp_start(const FTP_CONFIG *config);

#endif /* XE_FTP_H */
