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

#define FRAME_MAX       WPA_FRAME_MAX   /* an EAPOL-Key frame we will build or answer */

volatile WPA_STATS wpa_stats;

#define KCK (w->ptk)
#define KEK (w->ptk + 16)
#define TK  (w->ptk + 32)

/* The last handshake's counters, for xectl.py: each card has its own (WPA.st), and the one
 * that ran last is copied here. */
static void publish(WPA *w)
{
    const volatile u32 *from = (const volatile u32 *)&w->st;
    volatile u32 *to = (volatile u32 *)&wpa_stats;
    u32 i;

    for (i = 0; i < sizeof(WPA_STATS) / 4; i++)
        to[i] = from[i];
}

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
/* Two states, never one: wpa_set_psk runs on the worker for one card while another card's
 * handshake hashes in a receive DPC (both on at once since each card has its own WPA). The
 * DPCs run on one processor, one after another, so they can share theirs. */
static XE_HMAC_STATE thread_state, dpc_state;

static void hmac_with(XE_HMAC_STATE *state, const u8 *key, u32 key_len, const u8 *a,
                      u32 a_len, const u8 *b, u32 b_len, const u8 *c, u32 c_len, u8 *out)
{
    XeCryptHmacShaInit(state, key, key_len);
    if (a_len)
        XeCryptHmacShaUpdate(state, a, a_len);
    if (b_len)
        XeCryptHmacShaUpdate(state, b, b_len);
    if (c_len)
        XeCryptHmacShaUpdate(state, c, c_len);
    XeCryptHmacShaFinal(state, out, 20);
}

#define hmac(...) hmac_with(&dpc_state, __VA_ARGS__)

/* The key from the passphrase: the name of the network, a counter, and four
 * thousand repetitions folded together, twice over for 32 bytes (IEEE 802.11i's
 * password-based derivation). */
void wpa_set_psk(WPA *w, const char *passphrase, const u8 *ssid, u32 ssid_len)
{
    u8 counter[4], u[20], t[20];
    u32 pass_len = 0, block, round, i;

    while (passphrase[pass_len])
        pass_len++;
    w->st.psk_ready = 0;
    for (block = 1; block <= 2; block++) {
        counter[0] = counter[1] = counter[2] = 0;
        counter[3] = (u8)block;
        hmac_with(&thread_state, (const u8 *)passphrase, pass_len, ssid, ssid_len, counter, 4, 0, 0, u);
        copy(t, u, 20);
        for (round = 1; round < 4096; round++) {
            hmac_with(&thread_state, (const u8 *)passphrase, pass_len, u, 20, 0, 0, 0, 0, u);
            for (i = 0; i < 20; i++)
                t[i] ^= u[i];
        }
        for (i = 0; i < 20 && (block - 1) * 20 + i < WPA_PSK_LEN; i++)
            w->psk[(block - 1) * 20 + i] = t[i];
    }
    mem_sync();
    w->st.psk_ready = 1;
    publish(w);
}

const u8 *wpa_psk_for_test(WPA *w)
{
    return w->psk;
}

const u8 *wpa_ptk_for_test(WPA *w)
{
    return w->ptk;
}

/* The pair of keys both sides end up with: the two addresses least first, then the
 * two numbers least first, hashed with the label until there are enough bytes
 * (IEEE 802.11i's pseudo-random function). */
static void derive_ptk(WPA *w)
{
    static const char label[] = "Pairwise key expansion";
    u8 data[2 * 6 + 2 * WPA_NONCE_LEN + 1], block[20];
    u32 at = 0, i, k;

    if (lower(w->own_addr, w->ap_addr, 6)) {
        copy(data, w->own_addr, 6);
        copy(data + 6, w->ap_addr, 6);
    } else {
        copy(data, w->ap_addr, 6);
        copy(data + 6, w->own_addr, 6);
    }
    if (lower(w->snonce, w->anonce, WPA_NONCE_LEN)) {
        copy(data + 12, w->snonce, WPA_NONCE_LEN);
        copy(data + 12 + WPA_NONCE_LEN, w->anonce, WPA_NONCE_LEN);
    } else {
        copy(data + 12, w->anonce, WPA_NONCE_LEN);
        copy(data + 12 + WPA_NONCE_LEN, w->snonce, WPA_NONCE_LEN);
    }
    for (i = 0; at < WPA_PTK_LEN; i++) {
        data[12 + 2 * WPA_NONCE_LEN] = (u8)i;
        hmac(w->psk, WPA_PSK_LEN, (const u8 *)label, sizeof(label), data,
             sizeof(data), 0, 0, block);
        for (k = 0; k < 20 && at < WPA_PTK_LEN; k++)
            w->ptk[at++] = block[k];
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
static void fresh_nonce(WPA *w, u8 *out)
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
    copy(seed + 8, w->own_addr, 6);
    seed[14] = seed[15] = 0;
    hmac(w->psk, WPA_PSK_LEN, seed, sizeof(seed), 0, 0, 0, 0, out);
    hmac(w->psk, WPA_PSK_LEN, out, 20, seed, sizeof(seed), 0, 0, out + 12);
}

/* Build an answer: the same replay counter the access point used, our number, the
 * key data given, and the check value over the whole of it. */
static u32 answer(WPA *w, u32 key_info, const u8 *data, u32 data_len, u32 with_nonce)
{
    u32 length = E_DATA + data_len;
    u8 mic[20];

    if (length > FRAME_MAX)
        return 0;
    zero(w->frame, length);
    w->frame[E_VERSION] = 1;
    w->frame[E_TYPE] = EAPOL_KEY;
    put_be16(w->frame + E_BODY_LEN, length - 4);
    w->frame[E_DESCRIPTOR] = DESC_RSN;
    put_be16(w->frame + E_KEY_INFO, key_info);
    put_be16(w->frame + E_KEY_LEN, WPA_KEY_LEN);
    copy(w->frame + E_REPLAY, w->replay, 8);
    if (with_nonce)
        copy(w->frame + E_NONCE, w->snonce, WPA_NONCE_LEN);
    put_be16(w->frame + E_DATA_LEN, data_len);
    if (data_len)
        copy(w->frame + E_DATA, data, data_len);
    hmac(KCK, 16, w->frame, length, 0, 0, 0, 0, mic);
    copy(w->frame + E_MIC, mic, WPA_MIC_LEN);
    w->st.messages_out++;
    return w->host.send ? w->host.send(w->host.context, w->frame, length) : 0;
}

/* Is the check value on a received frame the one it should carry? */
static u32 mic_holds(WPA *w, const u8 *in, u32 length)
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

void wpa_begin(WPA *w, const WPA_HOST *h, const u8 *own, const u8 *ap, const u8 *rsn,
               u32 rsn_len)
{
    w->host = *h;
    copy(w->own_addr, own, 6);
    copy(w->ap_addr, ap, 6);
    w->own_rsn_len = rsn_len <= sizeof(w->own_rsn) ? rsn_len : 0;
    copy(w->own_rsn, rsn, w->own_rsn_len);
    zero(w->replay, 8);
    w->st.state = WPA_WAIT_1;
    w->st.gtk_len = 0;
    mem_sync();
    publish(w);
}

static void fail(WPA *w)
{
    w->st.failures++;
    w->st.state = WPA_FAILED;
    mem_sync();
    if (w->host.ready)
        w->host.ready(w->host.context, 0);
}

/* The group key handshake (802.11i 8.5.4), the access point's new group key
 * after the four-way: its first message carries it wrapped, as the third did, and
 * the answer is the same counter with our check value and nothing else. Left
 * unanswered the access point gives the station up -- hostapd's default rekey is
 * an hour, and every long session ended there (2026-11-04). */
static void group_message(WPA *w, const u8 *body, u32 length, u32 key_info)
{
    u8 data[FRAME_MAX];
    const u8 *gtk;
    u32 data_len = be16(body + E_DATA_LEN), index = 0, gtk_len, i, newer = 0;

    if (w->st.state != WPA_DONE || !(key_info & KI_MIC) ||
        !(key_info & KI_ENCRYPTED) || E_DATA + data_len > length || !data_len)
        return;
    for (i = 0; i < 8 && !newer; i++)
        newer = body[E_REPLAY + i] > w->replay[i] ? 1 : body[E_REPLAY + i] < w->replay[i]
                                                     ? 2 : 0;
    if (newer != 1) {
        w->st.replays++;
        return;
    }
    if (!mic_holds(w, body, length)) {
        w->st.mic_failures++;
        return;
    }
    copy(w->replay, body + E_REPLAY, 8);
    gtk_len = wpa_unwrap(KEK, body + E_DATA, data_len, data);
    if (!gtk_len) {
        w->st.unwrap_failures++;
        return;
    }
    if (!gtk_from_kde(data, gtk_len, &index, &gtk, &gtk_len)) {
        w->st.no_gtk++;
        return;
    }
    copy(w->pending_gtk, gtk, gtk_len);
    w->pending_gtk_len = gtk_len;
    w->pending_gtk_index = index;
    w->pending_ptk = 0;
    w->st.gtk_len = gtk_len;
    if (!answer(w, 2 | KI_MIC | KI_SECURE, 0, 0, 0))
        return;
    w->st.group_rekeys++;
    mem_sync();
    w->st.pending = 1;
}

static void eapol_in(WPA *w, const u8 *body, u32 length)
{
    u8 data[FRAME_MAX];
    const u8 *gtk;
    u32 key_info, data_len, index = 0, gtk_len = 0, i, newer = 0;

    if (length < E_MIN || body[E_TYPE] != EAPOL_KEY ||
        body[E_DESCRIPTOR] != DESC_RSN)
        return;
    if (w->st.state != WPA_WAIT_1 && w->st.state != WPA_WAIT_3 &&
        w->st.state != WPA_DONE)
        return;
    key_info = be16(body + E_KEY_INFO);
    w->st.messages_in++;
    w->st.last_key_info = key_info;
    if ((key_info & KI_VERSION) != 2 || (key_info & KI_ERROR))
        return;
    if (!(key_info & KI_PAIRWISE)) {
        group_message(w, body, length, key_info);
        return;
    }
    if (w->st.state == WPA_DONE)
        return;                         /* a pairwise rekey: not asked for here */
    data_len = be16(body + E_DATA_LEN);
    if (E_DATA + data_len > length)
        return;

    /* The access point's counter must have moved on, or this is an old message
     * being replayed at us. */
    for (i = 0; i < 8 && !newer; i++)
        newer = body[E_REPLAY + i] > w->replay[i] ? 1 : body[E_REPLAY + i] < w->replay[i]
                                                     ? 2 : 0;
    if (newer == 2) {
        w->st.replays++;
        return;
    }
    copy(w->replay, body + E_REPLAY, 8);

    if (!(key_info & KI_MIC)) {
        /* The first message: the access point's number, and nothing to check yet.
         * Answer with ours, the keys both sides can now work out, and the security
         * element the association carried. */
        if (w->st.state != WPA_WAIT_1)
            return;
        copy(w->anonce, body + E_NONCE, WPA_NONCE_LEN);
        fresh_nonce(w, w->snonce);
        derive_ptk(w);
        if (!answer(w, 2 | KI_PAIRWISE | KI_MIC, w->own_rsn, w->own_rsn_len, 1)) {
            fail(w);
            return;
        }
        w->st.state = WPA_WAIT_3;
        mem_sync();
        return;
    }

    if (!mic_holds(w, body, length)) {
        w->st.mic_failures++;
        return;                         /* not from anyone holding our key */
    }
    if (w->st.state != WPA_WAIT_3)
        return;

    /* The third message: the group key, wrapped. Install both keys, answer, and
     * the port may open. */
    if (data_len) {
        if (!(key_info & KI_ENCRYPTED)) {
            fail(w);
            return;
        }
        gtk_len = wpa_unwrap(KEK, body + E_DATA, data_len, data);
        if (!gtk_len) {
            w->st.unwrap_failures++;
            fail(w);
            return;
        }
        if (!gtk_from_kde(data, gtk_len, &index, &gtk, &gtk_len)) {
            w->st.no_gtk++;
            fail(w);
            return;
        }
    }
    /* The keys are settled. The fourth message goes at once, because the access
     * point is waiting for it; the keys themselves go in on the worker, which is
     * allowed to wait for the firmware (wpa_apply). */
    if (gtk_len)
        copy(w->pending_gtk, gtk, gtk_len);
    w->pending_gtk_len = gtk_len;
    w->pending_gtk_index = index;
    w->pending_ptk = 1;
    w->st.gtk_len = gtk_len;
    if (!answer(w, 2 | KI_PAIRWISE | KI_MIC | KI_SECURE, 0, 0, 0)) {
        fail(w);
        return;
    }
    w->st.state = WPA_DONE;
    mem_sync();
    w->st.pending = 1;
}

static u32 apply(WPA *w)
{
    u32 ok;

    if (!w->st.pending)
        return 0;
    w->st.pending = 0;
    w->st.applied++;
    mem_sync();
    if (!w->pending_ptk) {                 /* a group rekey: the new group key only */
        if (w->host.key && w->pending_gtk_len &&
            !w->host.key(w->host.context, 0, w->pending_gtk_index, w->pending_gtk, w->pending_gtk_len))
            w->st.failures++;
        mem_sync();
        return 1;
    }
    ok = w->host.key && w->host.key(w->host.context, 1, 0, TK, WPA_KEY_LEN);
    if (ok && w->pending_gtk_len)
        ok = w->host.key(w->host.context, 0, w->pending_gtk_index, w->pending_gtk,
                      w->pending_gtk_len);
    if (!ok) {
        w->st.failures++;
        w->st.state = WPA_FAILED;
    }
    mem_sync();
    if (w->host.ready)
        w->host.ready(w->host.context, ok);
    return 1;
}

void wpa_eapol_in(WPA *w, const u8 *body, u32 length)
{
    eapol_in(w, body, length);
    publish(w);
}

u32 wpa_apply(WPA *w)
{
    u32 r = apply(w);

    publish(w);
    return r;
}
