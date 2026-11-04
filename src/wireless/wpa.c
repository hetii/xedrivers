/* WPA2-Personal's four-way handshake; see wpa.h. Every hash and block operation
 * is the console's (src/core/kernel.h), checked there against known answers. */

#include "wpa.h"

/* An EAPOL-Key frame, by byte from the start of the EAPOL header. */
#define E_VERSION       0
#define E_TYPE          1
#define E_BODY_LEN      2
#define E_DESCRIPTOR    4
#define E_KEY_INFO      5
#define E_KEY_LEN       7
#define E_REPLAY        9
#define E_NONCE         17
#define E_IV            49
#define E_RSC           65
#define E_MIC           81
#define E_DATA_LEN      97
#define E_DATA          99
#define E_MIN           E_DATA

#define EAPOL_KEY       3u      /* the EAPOL packet type */
#define DESC_RSN        2u

#define KI_VERSION      0x0007u /* 2 is HMAC-SHA1 and the AES key wrap */
#define KI_PAIRWISE     0x0008u
#define KI_INSTALL      0x0040u
#define KI_ACK          0x0080u
#define KI_MIC          0x0100u
#define KI_SECURE       0x0200u
#define KI_ERROR        0x0400u
#define KI_ENCRYPTED    0x1000u

#define FRAME_MAX       512     /* an EAPOL-Key frame we will build or answer */

volatile WPA_STATS wpa_stats;

static u8 psk[WPA_PSK_LEN];
static u8 ptk[WPA_PTK_LEN];     /* confirmation, encryption and temporal key */
static u8 anonce[WPA_NONCE_LEN];
static u8 snonce[WPA_NONCE_LEN];
static u8 replay[8];
static u8 own_addr[6], ap_addr[6];
static u8 own_rsn[32];
static u32 own_rsn_len;
static u8 frame[FRAME_MAX];
static WPA_HOST host;

/* What the handshake has settled and the worker has yet to hand to the firmware:
 * installing a key is a command that must be waited for, which cannot be done
 * where the frames arrive. */
static u8 pending_gtk[WPA_GTK_MAX];
static u32 pending_gtk_len, pending_gtk_index;
static u32 pending_ptk;         /* the pairwise key too: the four-way, not a rekey */

#define KCK (ptk)
#define KEK (ptk + 16)
#define TK  (ptk + 32)

static void zero(u8 *p, u32 n)
{
    while (n--)
        *p++ = 0;
}

static void copy(u8 *to, const u8 *from, u32 n)
{
    while (n--)
        *to++ = *from++;
}

/* Which of two equal-length strings sorts first, as the key derivation's
 * least-first ordering needs. */
static u32 lower(const u8 *a, const u8 *b, u32 n)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i];
    }
    return 1;
}

static u32 be16(const u8 *p)
{
    return (u32)p[0] << 8 | p[1];
}

static void put_be16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

/* --- what the console computes for us ----------------------------------------- */

/* HMAC-SHA1 over up to three pieces, the incremental way: the one-shot form takes
 * ten arguments and this compiler cannot pass them (kernel.h). */
static void hmac(const u8 *key, u32 key_len, const u8 *a, u32 a_len,
                 const u8 *b, u32 b_len, const u8 *c, u32 c_len, u8 *out)
{
    static XE_HMAC_STATE state;

    XeCryptHmacShaInit(&state, key, key_len);
    if (a_len)
        XeCryptHmacShaUpdate(&state, a, a_len);
    if (b_len)
        XeCryptHmacShaUpdate(&state, b, b_len);
    if (c_len)
        XeCryptHmacShaUpdate(&state, c, c_len);
    XeCryptHmacShaFinal(&state, out, 20);
}

/* The key from the passphrase: the name of the network, a counter, and four
 * thousand repetitions folded together, twice over for 32 bytes (IEEE 802.11i's
 * password-based derivation). */
void wpa_set_psk(const char *passphrase, const u8 *ssid, u32 ssid_len)
{
    u8 counter[4], u[20], t[20];
    u32 pass_len = 0, block, round, i;

    while (passphrase[pass_len])
        pass_len++;
    wpa_stats.psk_ready = 0;
    for (block = 1; block <= 2; block++) {
        counter[0] = counter[1] = counter[2] = 0;
        counter[3] = (u8)block;
        hmac((const u8 *)passphrase, pass_len, ssid, ssid_len, counter, 4, 0, 0, u);
        copy(t, u, 20);
        for (round = 1; round < 4096; round++) {
            hmac((const u8 *)passphrase, pass_len, u, 20, 0, 0, 0, 0, u);
            for (i = 0; i < 20; i++)
                t[i] ^= u[i];
        }
        for (i = 0; i < 20 && (block - 1) * 20 + i < WPA_PSK_LEN; i++)
            psk[(block - 1) * 20 + i] = t[i];
    }
    mem_sync();
    wpa_stats.psk_ready = 1;
}

const u8 *wpa_psk_for_test(void)
{
    return psk;
}

const u8 *wpa_ptk_for_test(void)
{
    return ptk;
}

/* The pair of keys both sides end up with: the two addresses least first, then the
 * two numbers least first, hashed with the label until there are enough bytes
 * (IEEE 802.11i's pseudo-random function). */
static void derive_ptk(void)
{
    static const char label[] = "Pairwise key expansion";
    u8 data[2 * 6 + 2 * WPA_NONCE_LEN + 1], block[20];
    u32 at = 0, i, k;

    if (lower(own_addr, ap_addr, 6)) {
        copy(data, own_addr, 6);
        copy(data + 6, ap_addr, 6);
    } else {
        copy(data, ap_addr, 6);
        copy(data + 6, own_addr, 6);
    }
    if (lower(snonce, anonce, WPA_NONCE_LEN)) {
        copy(data + 12, snonce, WPA_NONCE_LEN);
        copy(data + 12 + WPA_NONCE_LEN, anonce, WPA_NONCE_LEN);
    } else {
        copy(data + 12, anonce, WPA_NONCE_LEN);
        copy(data + 12 + WPA_NONCE_LEN, snonce, WPA_NONCE_LEN);
    }
    for (i = 0; at < WPA_PTK_LEN; i++) {
        data[12 + 2 * WPA_NONCE_LEN] = (u8)i;
        hmac(psk, WPA_PSK_LEN, (const u8 *)label, sizeof(label), data,
             sizeof(data), 0, 0, block);
        for (k = 0; k < 20 && at < WPA_PTK_LEN; k++)
            ptk[at++] = block[k];
    }
}

/* The group key travels wrapped under the encryption key: RFC 3394, undone with
 * the console's AES. `in_len` is a multiple of eight and at least 24; the result
 * is eight bytes shorter and only believed if the check value comes back. */
u32 wpa_unwrap(const u8 *kek, const u8 *in, u32 in_len, u8 *out)
{
    static XE_AES_STATE aes;
    u8 a[8], block[16], decrypted[16];
    u32 n = in_len / 8 - 1, i, j, t;

    if (in_len < 24 || in_len % 8 || n > 8)
        return 0;
    XeCryptAesKey(&aes, kek);
    copy(a, in, 8);
    copy(out, in + 8, in_len - 8);
    for (j = 6; j-- > 0;) {
        for (i = n; i >= 1; i--) {
            t = n * j + i;
            copy(block, a, 8);
            block[7] ^= (u8)t;
            block[6] ^= (u8)(t >> 8);
            copy(block + 8, out + (i - 1) * 8, 8);
            XeCryptAesEcb(&aes, block, decrypted, 0);
            copy(a, decrypted, 8);
            copy(out + (i - 1) * 8, decrypted + 8, 8);
        }
    }
    for (i = 0; i < 8; i++)
        if (a[i] != 0xA6)
            return 0;
    return n * 8;
}

/* --- the frames ---------------------------------------------------------------- */

u32 wpa_rsn_ie(u8 *out)
{
    static const u8 ie[] = {
        0x30, 0x14,                         /* the security element, 20 bytes */
        0x01, 0x00,                         /* version 1 */
        0x00, 0x0F, 0xAC, 0x04,             /* group cipher: CCMP */
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, /* one pairwise cipher: CCMP */
        0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, /* one key manager: the pre-shared key */
        0x00, 0x00,                         /* no capabilities asked for */
    };
    u32 i;

    for (i = 0; i < sizeof(ie); i++)
        out[i] = ie[i];
    return sizeof(ie);
}

/* A number this station will not repeat: the time base, which moves every tick,
 * mixed with the keyed hash so it does not reveal the clock directly. */
static void fresh_nonce(u8 *out)
{
    static u32 count;
    u8 seed[16];
    u32 now = 0, i;

#if defined(__powerpc__)
    __asm__ volatile("mftb %0" : "=r"(now));
#endif
    count++;
    for (i = 0; i < 4; i++) {
        seed[i] = (u8)(now >> (24 - 8 * i));
        seed[4 + i] = (u8)(count >> (24 - 8 * i));
    }
    copy(seed + 8, own_addr, 6);
    seed[14] = seed[15] = 0;
    hmac(psk, WPA_PSK_LEN, seed, sizeof(seed), 0, 0, 0, 0, out);
    hmac(psk, WPA_PSK_LEN, out, 20, seed, sizeof(seed), 0, 0, out + 12);
}

/* Build an answer: the same replay counter the access point used, our number, the
 * key data given, and the check value over the whole of it. */
static u32 answer(u32 key_info, const u8 *data, u32 data_len, u32 with_nonce)
{
    u32 length = E_DATA + data_len;
    u8 mic[20];

    if (length > FRAME_MAX)
        return 0;
    zero(frame, length);
    frame[E_VERSION] = 1;
    frame[E_TYPE] = EAPOL_KEY;
    put_be16(frame + E_BODY_LEN, length - 4);
    frame[E_DESCRIPTOR] = DESC_RSN;
    put_be16(frame + E_KEY_INFO, key_info);
    put_be16(frame + E_KEY_LEN, WPA_KEY_LEN);
    copy(frame + E_REPLAY, replay, 8);
    if (with_nonce)
        copy(frame + E_NONCE, snonce, WPA_NONCE_LEN);
    put_be16(frame + E_DATA_LEN, data_len);
    if (data_len)
        copy(frame + E_DATA, data, data_len);
    hmac(KCK, 16, frame, length, 0, 0, 0, 0, mic);
    copy(frame + E_MIC, mic, WPA_MIC_LEN);
    wpa_stats.messages_out++;
    return host.send ? host.send(host.context, frame, length) : 0;
}

/* Is the check value on a received frame the one it should carry? */
static u32 mic_holds(const u8 *in, u32 length)
{
    u8 copy_of[FRAME_MAX], mic[20];
    u32 i;

    if (length > FRAME_MAX)
        return 0;
    for (i = 0; i < length; i++)
        copy_of[i] = in[i];
    zero(copy_of + E_MIC, WPA_MIC_LEN);
    hmac(KCK, 16, copy_of, length, 0, 0, 0, 0, mic);
    for (i = 0; i < WPA_MIC_LEN; i++)
        if (mic[i] != in[E_MIC + i])
            return 0;
    return 1;
}

/* The group key sits among the elements of the unwrapped key data: the vendor one
 * (0xDD) carrying 00:0f:ac type 1, whose first two bytes are its index and flags.
 * The others are skipped, not stopped at -- a WPA2 access point puts its own
 * security element (0x30) in front of it, and pads the end with zeroes. */
static u32 gtk_from_kde(const u8 *data, u32 length, u32 *index, const u8 **key,
                        u32 *key_len)
{
    u32 at = 0, n;

    while (at + 2 <= length) {
        n = data[at + 1];
        if (!data[at] || at + 2 + n > length)
            break;                      /* the padding, or a length that overruns */
        if (data[at] == 0xDD && n >= 6 && data[at + 2] == 0x00 &&
            data[at + 3] == 0x0F && data[at + 4] == 0xAC && data[at + 5] == 0x01) {
            *index = data[at + 6] & 0x03;
            *key = data + at + 8;
            *key_len = n - 6;
            return *key_len && *key_len <= WPA_GTK_MAX;
        }
        at += 2 + n;
    }
    return 0;
}

/* --- the handshake ------------------------------------------------------------- */

void wpa_begin(const WPA_HOST *h, const u8 *own, const u8 *ap, const u8 *rsn,
               u32 rsn_len)
{
    host = *h;
    copy(own_addr, own, 6);
    copy(ap_addr, ap, 6);
    own_rsn_len = rsn_len <= sizeof(own_rsn) ? rsn_len : 0;
    copy(own_rsn, rsn, own_rsn_len);
    zero(replay, 8);
    wpa_stats.state = WPA_WAIT_1;
    wpa_stats.gtk_len = 0;
    mem_sync();
}

static void fail(void)
{
    wpa_stats.failures++;
    wpa_stats.state = WPA_FAILED;
    mem_sync();
    if (host.ready)
        host.ready(host.context, 0);
}

/* The group key handshake (802.11i 8.5.4), the access point's new group key
 * after the four-way: its first message carries it wrapped, as the third did, and
 * the answer is the same counter with our check value and nothing else. Left
 * unanswered the access point gives the station up -- hostapd's default rekey is
 * an hour, and every long session ended there (2026-11-04). */
static void group_message(const u8 *body, u32 length, u32 key_info)
{
    u8 data[FRAME_MAX];
    const u8 *gtk;
    u32 data_len = be16(body + E_DATA_LEN), index = 0, gtk_len, i, newer = 0;

    if (wpa_stats.state != WPA_DONE || !(key_info & KI_MIC) ||
        !(key_info & KI_ENCRYPTED) || E_DATA + data_len > length || !data_len)
        return;
    for (i = 0; i < 8 && !newer; i++)
        newer = body[E_REPLAY + i] > replay[i] ? 1 : body[E_REPLAY + i] < replay[i]
                                                     ? 2 : 0;
    if (newer != 1) {
        wpa_stats.replays++;
        return;
    }
    if (!mic_holds(body, length)) {
        wpa_stats.mic_failures++;
        return;
    }
    copy(replay, body + E_REPLAY, 8);
    gtk_len = wpa_unwrap(KEK, body + E_DATA, data_len, data);
    if (!gtk_len) {
        wpa_stats.unwrap_failures++;
        return;
    }
    if (!gtk_from_kde(data, gtk_len, &index, &gtk, &gtk_len)) {
        wpa_stats.no_gtk++;
        return;
    }
    copy(pending_gtk, gtk, gtk_len);
    pending_gtk_len = gtk_len;
    pending_gtk_index = index;
    pending_ptk = 0;
    wpa_stats.gtk_len = gtk_len;
    if (!answer(2 | KI_MIC | KI_SECURE, 0, 0, 0))
        return;
    wpa_stats.group_rekeys++;
    mem_sync();
    wpa_stats.pending = 1;
}

void wpa_eapol_in(const u8 *body, u32 length)
{
    u8 data[FRAME_MAX];
    const u8 *gtk;
    u32 key_info, data_len, index = 0, gtk_len = 0, i, newer = 0;

    if (length < E_MIN || body[E_TYPE] != EAPOL_KEY ||
        body[E_DESCRIPTOR] != DESC_RSN)
        return;
    if (wpa_stats.state != WPA_WAIT_1 && wpa_stats.state != WPA_WAIT_3 &&
        wpa_stats.state != WPA_DONE)
        return;
    key_info = be16(body + E_KEY_INFO);
    wpa_stats.messages_in++;
    wpa_stats.last_key_info = key_info;
    if ((key_info & KI_VERSION) != 2 || (key_info & KI_ERROR))
        return;
    if (!(key_info & KI_PAIRWISE)) {
        group_message(body, length, key_info);
        return;
    }
    if (wpa_stats.state == WPA_DONE)
        return;                         /* a pairwise rekey: not asked for here */
    data_len = be16(body + E_DATA_LEN);
    if (E_DATA + data_len > length)
        return;

    /* The access point's counter must have moved on, or this is an old message
     * being replayed at us. */
    for (i = 0; i < 8 && !newer; i++)
        newer = body[E_REPLAY + i] > replay[i] ? 1 : body[E_REPLAY + i] < replay[i]
                                                     ? 2 : 0;
    if (newer == 2) {
        wpa_stats.replays++;
        return;
    }
    copy(replay, body + E_REPLAY, 8);

    if (!(key_info & KI_MIC)) {
        /* The first message: the access point's number, and nothing to check yet.
         * Answer with ours, the keys both sides can now work out, and the security
         * element the association carried. */
        if (wpa_stats.state != WPA_WAIT_1)
            return;
        copy(anonce, body + E_NONCE, WPA_NONCE_LEN);
        fresh_nonce(snonce);
        derive_ptk();
        if (!answer(2 | KI_PAIRWISE | KI_MIC, own_rsn, own_rsn_len, 1)) {
            fail();
            return;
        }
        wpa_stats.state = WPA_WAIT_3;
        mem_sync();
        return;
    }

    if (!mic_holds(body, length)) {
        wpa_stats.mic_failures++;
        return;                         /* not from anyone holding our key */
    }
    if (wpa_stats.state != WPA_WAIT_3)
        return;

    /* The third message: the group key, wrapped. Install both keys, answer, and
     * the port may open. */
    if (data_len) {
        if (!(key_info & KI_ENCRYPTED)) {
            fail();
            return;
        }
        gtk_len = wpa_unwrap(KEK, body + E_DATA, data_len, data);
        if (!gtk_len) {
            wpa_stats.unwrap_failures++;
            fail();
            return;
        }
        if (!gtk_from_kde(data, gtk_len, &index, &gtk, &gtk_len)) {
            wpa_stats.no_gtk++;
            fail();
            return;
        }
    }
    /* The keys are settled. The fourth message goes at once, because the access
     * point is waiting for it; the keys themselves go in on the worker, which is
     * allowed to wait for the firmware (wpa_apply). */
    if (gtk_len)
        copy(pending_gtk, gtk, gtk_len);
    pending_gtk_len = gtk_len;
    pending_gtk_index = index;
    pending_ptk = 1;
    wpa_stats.gtk_len = gtk_len;
    if (!answer(2 | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, 0, 0)) {
        fail();
        return;
    }
    wpa_stats.state = WPA_DONE;
    mem_sync();
    wpa_stats.pending = 1;
}

u32 wpa_apply(void)
{
    u32 ok;

    if (!wpa_stats.pending)
        return 0;
    wpa_stats.pending = 0;
    wpa_stats.applied++;
    mem_sync();
    if (!pending_ptk) {                 /* a group rekey: the new group key only */
        if (host.key && pending_gtk_len &&
            !host.key(host.context, 0, pending_gtk_index, pending_gtk, pending_gtk_len))
            wpa_stats.failures++;
        mem_sync();
        return 1;
    }
    ok = host.key && host.key(host.context, 1, 0, TK, WPA_KEY_LEN);
    if (ok && pending_gtk_len)
        ok = host.key(host.context, 0, pending_gtk_index, pending_gtk,
                      pending_gtk_len);
    if (!ok) {
        wpa_stats.failures++;
        wpa_stats.state = WPA_FAILED;
    }
    mem_sync();
    if (host.ready)
        host.ready(host.context, ok);
    return 1;
}
