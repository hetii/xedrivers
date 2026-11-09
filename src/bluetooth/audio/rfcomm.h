/* RFCOMM (TS 07.10 as Bluetooth uses it, RFCOMM 1.2): serial channels over L2CAP PSM 3, for
 * the hands-free profile's AT commands. One multiplexer per link, the console answering as the
 * responder or opening it itself; credit-based flow control whenever the other side offers it.
 */

#ifndef XE_RFCOMM_H
#define XE_RFCOMM_H

#include "host.h"

#define RFCOMM_DLCS 4u

struct RFCOMM_DLC;

typedef struct {
    void (*open)(struct RFCOMM_DLC *d);
    void (*data)(struct RFCOMM_DLC *d, const u8 *data, u32 length);
    void (*closed)(struct RFCOMM_DLC *d);
} RFCOMM_USER;

typedef struct RFCOMM_DLC {
    u32 state;                  /* 0 free, 1 opening, 2 open */
    u32 dlci;
    u32 mtu;
    u32 credits_out;            /* frames we may send */
    u32 credits_given;          /* frames the other side may still send */
    volatile BT_CONN *conn;
    const RFCOMM_USER *user;
} RFCOMM_DLC;

typedef struct {
    u32 frames_in;
    u32 frames_out;
    u32 bad_fcs;
    u32 sessions;
    u32 dlcs_opened;
    u32 refused;
    u32 no_credit;              /* data held: the other side gave no credit */
    u32 last_control;
} RFCOMM_STATS;

extern volatile RFCOMM_STATS rfcomm_stats;

/* Pure (tests/bluetooth): the FCS of `n` bytes. */
u8   rfcomm_fcs(const u8 *p, u32 n);

void rfcomm_init(void);
u32  rfcomm_listen(u32 channel, const RFCOMM_USER *u);
u32  rfcomm_send(RFCOMM_DLC *d, const u8 *data, u32 length);   /* 0 if sent */
void rfcomm_gone(volatile BT_CONN *c);

#endif /* XE_RFCOMM_H */
