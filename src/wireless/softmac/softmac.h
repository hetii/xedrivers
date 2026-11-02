/* A station for any SoftMAC card: what mac80211 does on Linux for a card whose
 * firmware leaves scanning, joining and 802.11 framing to the host.
 *
 * The card's driver provides WL_SOFTMAC: put a frame on the air, tune, set up the
 * link in the hardware once joined, install a key. This layer does the rest --
 * scanning (mlme.c), the frames of joining and their elements (frames.c), the data
 * path between Ethernet and 802.11 (data.c), block-ack sessions (ba.c) -- and
 * offers the card upward as a WL_CARD, so the two modes (bridge.c, omni.c),
 * wpa.c's handshake and reorder.c's windows work on it as on a FullMAC card.
 *
 * One station per card; the driver keeps a WL_STA beside its own state and calls
 * wl_sta_rx for every frame received, wl_sta_xmit for every Ethernet frame the
 * network layer hands it, and wl_sta_tick from its worker tick.
 */

#ifndef XE_WL_SOFTMAC_H
#define XE_WL_SOFTMAC_H

#include "wireless.h"
#include "reorder.h"
#include "net.h"

/* How a frame goes out. */
#define WL_TX_MGMT      0x01u   /* management: the lowest rate, no aggregation */
#define WL_TX_PROTECT   0x02u   /* the hardware encrypts it with the pairwise key */
#define WL_TX_EAPOL     0x04u   /* the handshake's: a basic rate, never aggregated */
#define WL_TX_GROUP     0x08u   /* to a group address */

/* The link as joined, for the hardware: who, which channel, what rates. */
typedef struct {
    u8  bssid[6];
    u16 aid;
    u8  channel;                /* primary */
    u8  bw;                     /* 0 20 MHz, 1 40, 2 80 */
    u8  primary;                /* the primary's place in it, rtw88's RTW_SC_* */
    u8  ht;                     /* the AP does HT, VHT */
    u8  vht;
    u8  sgi;                    /* short guard interval for this width */
    u32 legacy_rates;           /* bit n: 802.11 legacy rate n of 1,2,5.5,11,6,9,...,54 */
    u16 ht_mcs;                 /* MCS 0-15 the AP takes */
    u16 vht_mcs_map;            /* the AP's VHT RX MCS map */
    u8  ampdu_factor;           /* the AP's maximum A-MPDU length exponent */
    u8  ampdu_density;
    u8  ldpc;                   /* the AP receives LDPC, STBC */
    u8  stbc;
} WL_LINK_INFO;

typedef struct {
    void *context;
    /* One 802.11 frame on the air, from its frame control to its last payload byte
     * (the hardware adds the FCS, and the MIC if WL_TX_PROTECT), in two parts the
     * card puts one after the other: the headers and the payload (which may be
     * empty). `tid` is the QoS priority of a data frame. 1 if queued. Any IRQL up
     * to DISPATCH. */
    u32  (*tx)(void *context, const u8 *head, u32 head_length, const u8 *body,
               u32 body_length, u32 flags, u32 tid);
    /* The station's own address, for the card's filters and its port. Thread. */
    void (*set_mac)(void *context, const u8 *mac);
    /* Whether tx would take a frame now. */
    u32  (*tx_space)(void *context);
    /* Tune. Thread. */
    void (*set_channel)(void *context, u32 channel, u32 bw, u32 primary);
    /* Joined (`link`), or left (0). Thread. */
    void (*set_link)(void *context, const WL_LINK_INFO *link);
    /* A CCMP key: the station's own (`pairwise`, with the AP's address) or the
     * group's with its index. 1 on success. Thread. */
    u32  (*set_key)(void *context, u32 pairwise, u32 index, const u8 *key, u32 length,
                    const u8 *peer);
    /* Forget every key. Thread. */
    void (*clear_keys)(void *context);
    /* Aggregate data of `tid` into A-MPDUs (a block-ack session with the AP is up)
     * or stop. Any IRQL up to DISPATCH. */
    void (*set_aggregation)(void *context, u32 tid, u32 on);
} WL_SOFTMAC;

/* Where the station is. */
enum {
    WL_STA_IDLE = 0,
    WL_STA_SCANNING,
    WL_STA_PROBING,             /* on the network's channel, waiting for its elements */
    WL_STA_AUTH,                /* authentication sent, waiting */
    WL_STA_ASSOC,               /* association request sent, waiting */
    WL_STA_JOINED,              /* associated; secured networks then run the handshake */
};

#define WL_STA_IES_MAX  512     /* the joined network's elements, as it beacons them */

/* Read by tools/xectl.py. */
typedef struct {
    u32 state;
    u32 scans;
    u32 probes;                 /* probe requests sent */
    u32 auths;
    u32 auth_status;            /* of the last authentication answer */
    u32 assocs;
    u32 assoc_status;           /* of the last association answer */
    u32 aid;
    u32 joins;                  /* associated */
    u32 timeouts;               /* a step that got no answer */
    u32 deauths;                /* deauthentications and disassociations from the AP */
    u32 deauth_reason;
    u32 beacon_losses;
    u32 rx_data;
    u32 rx_eapol;
    u32 rx_dropped;             /* data frames not for us, or malformed */
    u32 tx_data;
    u32 tx_dropped;
    u32 addba_requests;
    u32 addba_accepted;
    u32 bw;
    u32 channel;
    u32 keys;
    u32 port_open;
    u32 rx_duplicates;          /* retransmissions of a frame already received */
    u32 tx_ba;                  /* our session to the AP: 1 asked, 2 up, 3 refused */
    u32 rx_ba;                  /* the AP's to us, a bit per TID */
    WL_REORDER_STATS reorder;
} WL_STA_STATS;

typedef struct {
    const WL_SOFTMAC *hw;
    NET_DEV *nd;
    volatile WL_STA_STATS *st;
    WL_CARD card;               /* this station, as the layer above sees a card */
    u8  own[6];                 /* the address it joins on: the console's */

    volatile u32 state;
    u32 ticks;                  /* in the current step */
    u32 tries;

    u32 scan_index;             /* scanning: each channel in turn */

    /* the network being joined or joined */
    WL_BSS bss;
    u8  ies[WL_STA_IES_MAX];
    volatile u32 ies_len;
    char pass[64];
    u32 pass_len;
    u32 secured;
    u8  rsn[32];
    u32 rsn_len;
    volatile u32 port_open;
    u32 ready_pending;          /* the handshake finished before joined(): 1 failed, 2 ok */
    volatile u32 pairwise_set;
    volatile u32 last_beacon;   /* worker ticks since its last beacon */
    WL_LINK_INFO link;

    /* what the receive path saw, for the worker */
    volatile u32 got_auth;
    volatile u32 got_assoc;
    volatile u32 got_lost;

    volatile u32 seq;           /* the next data sequence number */

    /* block ack (ba.c): the AP's sessions to us by TID, and ours to it */
    volatile u32 rx_ba;         /* bit n: TID n's frames go through reorder.c */
    volatile u32 tx_ba;         /* 0 none, 1 asked, 2 up */
    u32 ba_token;
    KDPC reorder_dpc;
    volatile u32 reorder_dpc_queued;
    volatile u32 pn_lo;         /* CCMP's packet number, 48 bits in two words: */
    volatile u32 pn_hi;         /* no 64-bit atomics here */

    WL_REORDER reorder;
    u8 reorder_pool[WL_REORDER_POOL];
    u16 last_seq_ctrl[WL_REORDER_TIDS + 1];     /* per TID, and one for non-QoS */
} WL_STA;

/* Set up `s` for the card `hw` with the network device `nd`, its stats in `st`.
 * Thread. After this s->card is the WL_CARD to give the mode (wl_mode_pick). */
void wl_sta_init(WL_STA *s, const WL_SOFTMAC *hw, NET_DEV *nd, volatile WL_STA_STATS *st);

/* One received frame, from frame control to payload (no FCS); `decrypted` when the
 * hardware has, with the CCMP header and MIC still in it. The frame is the
 * card's receive buffer and may be written over (data.c builds Ethernet headers in
 * place). DISPATCH_LEVEL. */
void wl_sta_rx(WL_STA *s, u8 *frame, u32 length, s32 signal, u32 channel,
               u32 decrypted);

/* One Ethernet frame from the network layer. Any IRQL up to DISPATCH. 1 if sent. */
u32  wl_sta_xmit(WL_STA *s, const u8 *eth, u32 length);

/* Every worker tick. Thread. */
void wl_sta_tick(WL_STA *s);
/* The card's transmit path has stopped: drop the link as a beacon loss would. */
void wl_sta_lost(WL_STA *s);

#endif /* XE_WL_SOFTMAC_H */
