/* Native test of the four-way handshake's own logic (src/wireless/wpa.c): which
 * message it answers, what it puts in the answer, and what it refuses.
 *
 * The console's SHA-1, HMAC-SHA1 and AES are not here, so this file stands in for
 * them with deterministic nonsense. That is deliberate: whether the real ones
 * compute the right values is checked on the console itself against the worked
 * examples in IEEE 802.11i and RFC 3394 (tools/xectl.py cmd 19 and 20), and what
 * is left to test here is the protocol -- the part those checks cannot see.
 */

#include <stdio.h>
#include <string.h>

#include "wpa.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
                      failures++; } } while (0)

/* --- what the console would do, faked ------------------------------------------- */

typedef struct { u32 acc; u32 n; } FAKE;

static void fake_init(FAKE *f, const u8 *key, u32 key_len)
{
    u32 i;

    f->acc = 0x9E3779B9u;
    f->n = 0;
    for (i = 0; i < key_len; i++)
        f->acc = f->acc * 31u + key[i];
}

static void fake_update(FAKE *f, const u8 *in, u32 length)
{
    u32 i;

    for (i = 0; i < length; i++) {
        f->acc = f->acc * 131u + in[i];
        f->n++;
    }
}

static void fake_final(FAKE *f, u8 *out, u32 out_length)
{
    u32 i;

    for (i = 0; i < out_length; i++)
        out[i] = (u8)((f->acc >> (8 * (i % 4))) ^ (f->n + i * 7u));
}

void XeCryptShaInit(XE_SHA_STATE *s) { fake_init((FAKE *)s, 0, 0); }
void XeCryptShaUpdate(XE_SHA_STATE *s, const u8 *in, u32 n) { fake_update((FAKE *)s, in, n); }
void XeCryptShaFinal(XE_SHA_STATE *s, u8 *out, u32 n) { fake_final((FAKE *)s, out, n); }

void XeCryptHmacShaInit(XE_HMAC_STATE *s, const u8 *key, u32 key_len)
{ fake_init((FAKE *)s, key, key_len); }
void XeCryptHmacShaUpdate(XE_HMAC_STATE *s, const u8 *in, u32 n)
{ fake_update((FAKE *)s, in, n); }
void XeCryptHmacShaFinal(XE_HMAC_STATE *s, u8 *out, u32 n)
{ fake_final((FAKE *)s, out, n); }

void XeCryptHmacSha(const u8 *k, u32 kl, const u8 *a, u32 al, const u8 *b, u32 bl,
                    const u8 *c, u32 cl, u8 *out, u32 ol)
{ (void)k; (void)kl; (void)a; (void)al; (void)b; (void)bl; (void)c; (void)cl;
  memset(out, 0, ol); }

static u8 aes_key_bytes[16];
void XeCryptAesKey(XE_AES_STATE *s, const u8 *key)
{ (void)s; memcpy(aes_key_bytes, key, 16); }
void XeCryptAesEcb(XE_AES_STATE *s, const u8 *in, u8 *out, u32 encrypt)
{
    u32 i;

    (void)s; (void)encrypt;
    for (i = 0; i < 16; i++)
        out[i] = in[i] ^ aes_key_bytes[i];
}

/* The check value the code will expect on a frame, using those same stand-ins. */
static void fake_mic(const u8 *kck, u8 *frame, u32 length)
{
    XE_HMAC_STATE state;
    u8 out[20];

    memset(frame + 81, 0, 16);
    XeCryptHmacShaInit(&state, kck, 16);
    XeCryptHmacShaUpdate(&state, frame, length);
    XeCryptHmacShaFinal(&state, out, 20);
    memcpy(frame + 81, out, 16);
}

/* RFC 3394 key wrap with the stand-in cipher above, as an access point wraps the
 * group key in its key data. `n` is the plaintext in 8-byte blocks. */
static u32 fake_wrap(const u8 *kek, const u8 *in, u32 n, u8 *out)
{
    XE_AES_STATE aes;
    u8 a[8], b[16];
    u32 i, j, k, t;

    memset(a, 0xA6, 8);
    memcpy(out + 8, in, 8 * n);
    XeCryptAesKey(&aes, kek);
    for (j = 0; j < 6; j++) {
        for (i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, out + 8 * i, 8);
            XeCryptAesEcb(&aes, b, b, 1);
            t = n * j + i;
            for (k = 0; k < 8; k++)
                a[k] = b[k];
            a[7] ^= (u8)t;
            a[6] ^= (u8)(t >> 8);
            memcpy(out + 8 * i, b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    return 8 * (n + 1);
}

/* --- the station's side of the conversation ------------------------------------- */

static u8 sent[8][512];
static u32 sent_len[8], sent_count;
static u32 keys_pairwise, keys_group, ready_calls, ready_ok;
static u8 installed_tk[32];

static u32 on_send(void *c, const u8 *body, u32 length)
{
    (void)c;
    if (sent_count >= 8)
        return 0;
    memcpy(sent[sent_count], body, length);
    sent_len[sent_count++] = length;
    return 1;
}

static u32 on_key(void *c, u32 pairwise, u32 index, const u8 *key, u32 length)
{
    (void)c; (void)index;
    if (pairwise) {
        keys_pairwise++;
        memcpy(installed_tk, key, length < 32 ? length : 32);
    } else {
        keys_group++;
    }
    return 1;
}

static void on_ready(void *c, u32 ok) { (void)c; ready_calls++; ready_ok = ok; }

static const u8 own_mac[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const u8 ap_mac[6]  = { 0x78, 0xa3, 0x51, 0x3a, 0xb6, 0x6c };

/* An EAPOL-Key frame as an access point sends it. */
static u32 build(u8 *f, u32 key_info, const u8 *counter, const u8 *nonce,
                 const u8 *data, u32 data_len)
{
    u32 length = 99 + data_len;

    memset(f, 0, length);
    f[0] = 1;                           /* version */
    f[1] = 3;                           /* EAPOL-Key */
    f[2] = (u8)((length - 4) >> 8);
    f[3] = (u8)(length - 4);
    f[4] = 2;                           /* RSN descriptor */
    f[5] = (u8)(key_info >> 8);
    f[6] = (u8)key_info;
    f[7] = 0;
    f[8] = 16;                          /* key length */
    memcpy(f + 9, counter, 8);
    if (nonce)
        memcpy(f + 17, nonce, 32);
    f[97] = (u8)(data_len >> 8);
    f[98] = (u8)data_len;
    if (data_len)
        memcpy(f + 99, data, data_len);
    return length;
}

int main(void)
{
    WPA_HOST host = { on_send, on_key, on_ready, 0 };
    u8 frame[512], anonce[32], counter[8];
    u8 rsn[32];
    u32 rsn_len, n, i;

    for (i = 0; i < 32; i++)
        anonce[i] = (u8)(0xA0 + i);
    memset(counter, 0, 8);

    rsn_len = wpa_rsn_ie(rsn);
    CHECK(rsn_len == 22 && rsn[0] == 0x30 && rsn[1] == 0x14);
    CHECK(rsn[8] == 0x01 && rsn[13] == 0x04);           /* one pairwise, CCMP */
    CHECK(rsn[14] == 0x01 && rsn[19] == 0x02);          /* one manager, the PSK */

    wpa_set_psk("a passphrase", (const u8 *)"net", 3);
    CHECK(wpa_stats.psk_ready);

    wpa_begin(&host, own_mac, ap_mac, rsn, rsn_len);
    CHECK(wpa_stats.state == WPA_WAIT_1);

    /* The first message: no check value on it, the access point's number in it.
     * The answer must carry ours, our security element, and a check value. */
    counter[7] = 1;
    n = build(frame, 2 | 0x08 | 0x80, counter, anonce, 0, 0);
    wpa_eapol_in(frame, n);
    CHECK(wpa_stats.state == WPA_WAIT_3);
    CHECK(sent_count == 1);
    if (sent_count) {
        const u8 *m2 = sent[0];
        u32 data_len = (u32)(m2[97] << 8 | m2[98]);

        CHECK(m2[1] == 3 && m2[4] == 2);
        CHECK((u32)(m2[5] << 8 | m2[6]) == (2u | 0x08u | 0x100u));
        CHECK(!memcmp(m2 + 9, counter, 8));             /* the same counter back */
        CHECK(data_len == rsn_len && !memcmp(m2 + 99, rsn, rsn_len));
        for (i = 0, n = 0; i < 32; i++)
            n |= m2[17 + i];
        CHECK(n);                                       /* a number was chosen */
    }

    /* A replay of an older counter is noticed and ignored. */
    {
        u8 old[8];
        u32 before = sent_count;

        memset(old, 0, 8);
        n = build(frame, 2 | 0x08 | 0x80, old, anonce, 0, 0);
        wpa_eapol_in(frame, n);
        CHECK(wpa_stats.replays == 1 && sent_count == before);
    }

    /* A third message whose check value is wrong gets no answer. */
    counter[7] = 2;
    n = build(frame, 2 | 0x08 | 0x100 | 0x40 | 0x200, counter, anonce, 0, 0);
    memset(frame + 81, 0xFF, 16);
    wpa_eapol_in(frame, n);
    CHECK(wpa_stats.mic_failures == 1 && sent_count == 1);
    CHECK(wpa_stats.state == WPA_WAIT_3);

    /* The real third message: keys installed, the fourth sent, the port opened. */
    counter[7] = 3;
    n = build(frame, 2 | 0x08 | 0x100 | 0x40 | 0x200, counter, anonce, 0, 0);
    fake_mic(wpa_ptk_for_test(), frame, n);
    wpa_eapol_in(frame, n);
    CHECK(wpa_stats.state == WPA_DONE);
    CHECK(sent_count == 2);
    /* The keys wait for the worker: installing one is a command the firmware
     * confirms, which cannot be waited for where the frames arrive. */
    CHECK(wpa_stats.pending && !keys_pairwise && !ready_calls);
    CHECK(wpa_apply() == 1);
    CHECK(keys_pairwise == 1 && keys_group == 0);
    CHECK(!memcmp(installed_tk, wpa_ptk_for_test() + 32, 16));
    CHECK(ready_calls == 1 && ready_ok == 1);
    CHECK(!wpa_apply());                        /* nothing left to do */
    if (sent_count == 2) {
        const u8 *m4 = sent[1];

        CHECK((u32)(m4[5] << 8 | m4[6]) == (2u | 0x08u | 0x100u | 0x200u));
        CHECK((u32)(m4[97] << 8 | m4[98]) == 0);        /* nothing in it */
    }

    /* The group key handshake after it: a new group key, wrapped, under a newer
     * counter. Answered with that counter and a check value, the key installed on
     * the worker, the pairwise key and the port left alone. */
    {
        u8 kde[24], wrapped[40], gtk2[16];
        u32 before = sent_count, wl;

        for (i = 0; i < 16; i++)
            gtk2[i] = (u8)(0x50 + i);
        kde[0] = 0xDD; kde[1] = 22;
        kde[2] = 0x00; kde[3] = 0x0F; kde[4] = 0xAC; kde[5] = 0x01;
        kde[6] = 2; kde[7] = 0;                         /* index 2 */
        memcpy(kde + 8, gtk2, 16);
        wl = fake_wrap(wpa_ptk_for_test() + 16, kde, 3, wrapped);
        counter[7] = 4;
        n = build(frame, 2 | 0x80 | 0x100 | 0x200 | 0x1000, counter, 0, wrapped, wl);
        fake_mic(wpa_ptk_for_test(), frame, n);
        wpa_eapol_in(frame, n);
        CHECK(wpa_stats.group_rekeys == 1 && sent_count == before + 1);
        CHECK(wpa_stats.state == WPA_DONE);
        if (sent_count == before + 1) {
            const u8 *g2 = sent[before];

            CHECK((u32)(g2[5] << 8 | g2[6]) == (2u | 0x100u | 0x200u));
            CHECK(!memcmp(g2 + 9, counter, 8));
            CHECK((u32)(g2[97] << 8 | g2[98]) == 0);
        }
        CHECK(wpa_apply() == 1);
        CHECK(keys_pairwise == 1 && keys_group == 1 && ready_calls == 1);

        /* The same message again: an old counter, no second answer. */
        before = sent_count;
        wpa_eapol_in(frame, n);
        CHECK(sent_count == before && wpa_stats.group_rekeys == 1);
    }

    if (failures)
        return 1;
    printf("all wpa tests passed\n");
    return 0;
}
