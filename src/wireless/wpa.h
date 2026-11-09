/* WPA2-Personal: the four-way handshake an access point runs with a station once
 * it has associated, and the keys it ends with.
 *
 * The station does none of the ciphering itself -- the card's firmware does that,
 * given the keys -- so all that happens here is the handshake: a key from the
 * passphrase, two values derived from the numbers both sides send, four messages
 * carrying a check value each, and the group key unwrapped out of the third. Every
 * hash, keyed hash and block decryption is the console's own (src/core/kernel.h);
 * nothing cryptographic is implemented here.
 *
 * The driver owns the frames and the keys: it hands every EAPOL frame it receives
 * to wpa_eapol_in and provides WPA_HOST, whose `send` puts one on the air and
 * whose `key` installs a derived key in the firmware. The state machine is driven
 * entirely by what arrives, so it needs no thread of its own.
 */

#ifndef XE_WPA_H
#define XE_WPA_H

#include "kernel.h"

#define WPA_ETHERTYPE   0x888Eu
#define WPA_PSK_LEN     32
#define WPA_PTK_LEN     48      /* CCMP: 16 confirmation, 16 encryption, 16 temporal */
#define WPA_KEY_LEN     16
#define WPA_NONCE_LEN   32
#define WPA_MIC_LEN     16
#define WPA_GTK_MAX     32

/* Where the handshake has got to. */
enum {
    WPA_IDLE = 0,
    WPA_WAIT_1,             /* associated, waiting for the first message */
    WPA_WAIT_3,             /* answered the first, waiting for the third */
    WPA_DONE,
    WPA_FAILED,
};

/* What the driver must provide. `send` is called as frames arrive, which may be at
 * DISPATCH_LEVEL; `key` and `ready` are only ever called from wpa_apply, so they
 * may talk to the firmware and wait for it. */
typedef struct {
    /* One EAPOL frame onto the air: `body` is what follows the Ethernet header,
     * which the driver builds from its own and the AP's address. 1 if it went. */
    u32 (*send)(void *context, const u8 *body, u32 length);

    /* Install a key in the firmware. `pairwise` is the station's own key, else the
     * group key with that index. 1 on success. */
    u32 (*key)(void *context, u32 pairwise, u32 index, const u8 *key, u32 length);

    /* The handshake is over: the controlled port may be opened. */
    void (*ready)(void *context, u32 ok);

    void *context;
} WPA_HOST;

typedef struct {
    u32 state;
    u32 failures;
    u32 replays;            /* a message whose counter was not newer */
    u32 mic_failures;
    u32 messages_in;
    u32 messages_out;
    u32 unwrap_failures;
    u32 last_key_info;
    u32 gtk_len;
    u32 no_gtk;             /* a third message whose key data held no group key */
    u32 pending;            /* keys settled, waiting for the worker to install */
    u32 applied;
    u32 psk_ready;
    u32 group_rekeys;       /* group key handshakes answered after the four-way */
} WPA_STATS;

extern volatile WPA_STATS wpa_stats;       /* the last handshake's, for xectl.py */

/* One card's handshake: each wireless card has its own (two cards, an RTL8822BU and an
 * AIC8800, shared one and wrote over each other's keys: the AIC8800 reset looped on 4-way
 * timeouts while the RTL8822BU was joined). */
#define WPA_FRAME_MAX 512
typedef struct WPA {
    u8 psk[WPA_PSK_LEN];
    u8 ptk[WPA_PTK_LEN];                    /* confirmation, encryption and temporal key */
    u8 anonce[WPA_NONCE_LEN];
    u8 snonce[WPA_NONCE_LEN];
    u8 replay[8];
    u8 own_addr[6], ap_addr[6];
    u8 own_rsn[32];
    u32 own_rsn_len;
    u8 frame[WPA_FRAME_MAX];
    WPA_HOST host;
    /* settled by the handshake, to be installed on the worker (wpa_apply) */
    u8 pending_gtk[WPA_GTK_MAX];
    u32 pending_gtk_len, pending_gtk_index;
    u32 pending_ptk;                        /* the pairwise key too: the four-way, not a rekey */
    volatile WPA_STATS st;
} WPA;

/* The key from the passphrase and the network's name. Thread-only: it is 8192
 * keyed hashes and takes a moment. */
void wpa_set_psk(WPA *w, const char *passphrase, const u8 *ssid, u32 ssid_len);

/* Start a handshake: the addresses are the station's own and the access point's.
 * The association request's security element must be the one the access point will
 * see in the second message, so it is given here. */
void wpa_begin(WPA *w, const WPA_HOST *host, const u8 *own_mac, const u8 *ap_mac,
               const u8 *rsn_ie, u32 rsn_ie_len);

/* One EAPOL frame from the air: what follows the Ethernet header. Any IRQL up to
 * DISPATCH: it hashes, answers on the air, and leaves the keys for wpa_apply. */
void wpa_eapol_in(WPA *w, const u8 *body, u32 length);

/* Hand the firmware whatever the handshake has settled, if anything: the keys and
 * then the open port. Thread-only, because installing a key is a command the
 * firmware confirms and that has to be waited for. 1 if there was something to
 * do, so the caller knows the link state may have changed. */
u32 wpa_apply(WPA *w);

/* The group key's wrapping undone (RFC 3394) with the console's AES: `in_len` a
 * multiple of eight and at least 24, the result eight bytes shorter, 0 when the
 * check value does not come back. Public so src/bench/debug can put a known
 * answer through it on the console. */
u32 wpa_unwrap(const u8 *kek, const u8 *in, u32 in_len, u8 *out);

/* The derived keys themselves, for that check and the host test, nothing else. */
const u8 *wpa_psk_for_test(WPA *w);
const u8 *wpa_ptk_for_test(WPA *w);

/* The security element a station announces for WPA2 with a pre-shared key and
 * CCMP: written into `out`, which needs 22 bytes. Returns its length. */
u32 wpa_rsn_ie(u8 *out);

#endif /* XE_WPA_H */
