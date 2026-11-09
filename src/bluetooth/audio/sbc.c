/* See sbc.h. The steps are the specification's, in its order: the analysis filter (B.2.2),
 * scale factors, joint stereo's choice, bit allocation (B.3.4.2) and quantisation, then the
 * frame with its CRC-8.
 */

#include "sbc.h"

/* Proto_8_80, the analysis window for 8 subbands (A2DP Table 12.24). */
static const float window[80] = {
     0.00000000e+00f,  1.56575398e-04f,  3.43256425e-04f,  5.54620202e-04f,
     8.23919506e-04f,  1.13992507e-03f,  1.47640169e-03f,  1.78371725e-03f,
     2.01182542e-03f,  2.10371989e-03f,  1.99454554e-03f,  1.61656283e-03f,
     9.02154502e-04f, -1.78805361e-04f, -1.64973098e-03f, -3.49717454e-03f,
     5.65949473e-03f,  8.02941163e-03f,  1.04584443e-02f,  1.27472335e-02f,
     1.46525263e-02f,  1.59045603e-02f,  1.62208471e-02f,  1.53184106e-02f,
     1.29371806e-02f,  8.85757540e-03f,  2.92408442e-03f, -4.91578024e-03f,
    -1.46404076e-02f, -2.61098752e-02f, -3.90751381e-02f, -5.31873032e-02f,
     6.79989431e-02f,  8.29847578e-02f,  9.75753918e-02f,  1.11196689e-01f,
     1.23264548e-01f,  1.33264415e-01f,  1.40753505e-01f,  1.45389847e-01f,
     1.46955068e-01f,  1.45389847e-01f,  1.40753505e-01f,  1.33264415e-01f,
     1.23264548e-01f,  1.11196689e-01f,  9.75753918e-02f,  8.29847578e-02f,
    -6.79989431e-02f, -5.31873032e-02f, -3.90751381e-02f, -2.61098752e-02f,
    -1.46404076e-02f, -4.91578024e-03f,  2.92408442e-03f,  8.85757540e-03f,
     1.29371806e-02f,  1.53184106e-02f,  1.62208471e-02f,  1.59045603e-02f,
     1.46525263e-02f,  1.27472335e-02f,  1.04584443e-02f,  8.02941163e-03f,
    -5.65949473e-03f, -3.49717454e-03f, -1.64973098e-03f, -1.78805361e-04f,
     9.02154502e-04f,  1.61656283e-03f,  1.99454554e-03f,  2.10371989e-03f,
     2.01182542e-03f,  1.78371725e-03f,  1.47640169e-03f,  1.13992507e-03f,
     8.23919506e-04f,  5.54620202e-04f,  3.43256425e-04f,  1.56575398e-04f,
};

/* M[i][k] = cos((i + 0.5)(k - 4) pi / 8), the analysis matrix, i subband, k 0..15. */
static float matrix[8][16];
static u32 matrix_ready;

/* Loudness offsets for 8 subbands, per sampling rate (B.3.4.2). */
static const s32 offset8[4][8] = {
    { -2, 0, 0, 0, 0, 0, 0, 1 },
    { -3, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 },
};

/* cos without libm, for the matrix only: x in [-4 pi, 4 pi], a Taylor series after
 * reduction to [-pi, pi], good to 1e-7. */
static float cosine(double x)
{
    const double pi = 3.14159265358979323846;
    double term = 1, sum = 1, x2;
    u32 n;

    while (x > pi)
        x -= 2 * pi;
    while (x < -pi)
        x += 2 * pi;
    x2 = x * x;
    for (n = 1; n < 20; n++) {
        term *= -x2 / (double)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return (float)sum;
}

void sbc_init(SBC *s, u32 rate, u32 blocks, u32 mode, u32 allocation, u32 bitpool)
{
    u32 i, k;

    if (!matrix_ready) {
        for (i = 0; i < 8; i++)
            for (k = 0; k < 16; k++)
                matrix[i][k] = cosine((i + 0.5) * ((double)k - 4) * 3.14159265358979323846 / 8);
        matrix_ready = 1;
    }
    for (i = 0; i < sizeof(*s); i++)
        ((u8 *)s)[i] = 0;
    s->rate = rate;
    s->blocks = blocks;
    s->mode = mode;
    s->allocation = allocation;
    s->bitpool = bitpool;
}

static u32 channels(const SBC *s)
{
    return s->mode == SBC_MONO ? 1 : 2;
}

u32 sbc_samples(const SBC *s)
{
    return s->blocks * SBC_SUBBANDS;
}

u32 sbc_frame_length(const SBC *s)
{
    u32 ch = channels(s), bits;

    if (s->mode == SBC_MONO || s->mode == SBC_DUAL)
        bits = s->blocks * ch * s->bitpool;
    else
        bits = (s->mode == SBC_JOINT ? SBC_SUBBANDS : 0) + s->blocks * s->bitpool;
    return 4 + (4 * SBC_SUBBANDS * ch) / 8 + (bits + 7) / 8;
}

/* One block of 8 new samples of a channel through the analysis filter: 8 subband samples. */
static void analyse(float *x, const s16 *pcm, u32 step, float *out)
{
    float y[16];
    u32 i, j;

    for (i = 79; i >= 8; i--)
        x[i] = x[i - 8];
    for (i = 0; i < 8; i++)
        x[7 - i] = (float)pcm[i * step];
    for (i = 0; i < 16; i++) {
        float sum = 0;

        for (j = 0; j < 5; j++)
            sum += window[i + 16 * j] * x[i + 16 * j];
        y[i] = sum;
    }
    for (i = 0; i < 8; i++) {
        float sum = 0;

        for (j = 0; j < 16; j++)
            sum += matrix[i][j] * y[j];
        out[i] = sum;
    }
}

/* The smallest k with |v| < 2^(k+1) over the blocks, 0..15. */
static u32 scale_factor(float sb[16][2][8], u32 blocks, u32 ch, u32 band)
{
    float most = 0;
    u32 b, k;

    for (b = 0; b < blocks; b++) {
        float v = sb[b][ch][band];

        if (v < 0)
            v = -v;
        if (v > most)
            most = v;
    }
    for (k = 0; k < 15 && most >= (float)(2u << k); k++)
        ;
    return k;
}

static s32 need(const SBC *s, u32 sf, u32 band)
{
    s32 loudness;

    if (s->allocation == SBC_SNR)
        return (s32)sf;
    if (sf == 0)
        return -5;
    loudness = (s32)sf - offset8[s->rate][band];
    return loudness > 0 ? loudness / 2 : loudness;
}

/* Bits per subband for `n` channels sharing the bitpool (1: mono, dual per channel; 2:
 * stereo and joint), B.3.4.2. */
static void allocate(const SBC *s, u32 n, u32 sf[2][8], u32 bits[2][8], u32 first)
{
    s32 bitneed[2][8], most = 0, bitslice;
    u32 bitcount = 0, slicecount = 0, ch, sb;
    u32 bitpool = s->bitpool;

    for (ch = first; ch < first + n; ch++)
        for (sb = 0; sb < 8; sb++) {
            bitneed[ch][sb] = need(s, sf[ch][sb], sb);
            if (bitneed[ch][sb] > most)
                most = bitneed[ch][sb];
        }
    bitslice = most + 1;
    do {
        bitslice--;
        bitcount += slicecount;
        slicecount = 0;
        for (ch = first; ch < first + n; ch++)
            for (sb = 0; sb < 8; sb++) {
                if (bitneed[ch][sb] > bitslice + 1 && bitneed[ch][sb] < bitslice + 16)
                    slicecount++;
                else if (bitneed[ch][sb] == bitslice + 1)
                    slicecount += 2;
            }
    } while (bitcount + slicecount < bitpool);
    if (bitcount + slicecount == bitpool) {
        bitcount += slicecount;
        bitslice--;
    }
    for (ch = first; ch < first + n; ch++)
        for (sb = 0; sb < 8; sb++) {
            if (bitneed[ch][sb] < bitslice + 2)
                bits[ch][sb] = 0;
            else
                bits[ch][sb] = (u32)(bitneed[ch][sb] - bitslice) < 16 ?
                               (u32)(bitneed[ch][sb] - bitslice) : 16;
        }
    ch = first;
    sb = 0;
    while (bitcount < bitpool && sb < 8) {
        if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) {
            bits[ch][sb]++;
            bitcount++;
        } else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1) {
            bits[ch][sb] = 2;
            bitcount += 2;
        }
        if (n == 2 && ch == first) {
            ch = first + 1;
        } else {
            ch = first;
            sb++;
        }
    }
    ch = first;
    sb = 0;
    while (bitcount < bitpool && sb < 8) {
        if (bits[ch][sb] < 16) {
            bits[ch][sb]++;
            bitcount++;
        }
        if (n == 2 && ch == first) {
            ch = first + 1;
        } else {
            ch = first;
            sb++;
        }
    }
}

/* Bits into the frame, most significant first; CRC-8 (x^8+x^4+x^3+x^2+1, from 0x0F) over
 * those `crc` says. */
typedef struct {
    u8 *out;
    u32 at;                             /* bits written */
    u32 crc;
    u32 counting;
} WRITER;

static void put(WRITER *w, u32 value, u32 n)
{
    while (n--) {
        u32 bit = (value >> n) & 1u;

        if (bit)
            w->out[w->at >> 3] |= (u8)(0x80u >> (w->at & 7));
        if (w->counting) {
            u32 top = ((w->crc >> 7) ^ bit) & 1u;

            w->crc = (w->crc << 1) & 0xFFu;
            if (top)
                w->crc ^= 0x1Du;
        }
        w->at++;
    }
}

u32 sbc_encode(SBC *s, const s16 *pcm, u8 *out)
{
    static float sb[16][2][8];
    u32 sf[2][8], bits[2][8], join = 0, n = channels(s), length = sbc_frame_length(s);
    u32 b, ch, i;
    WRITER w;

    for (b = 0; b < s->blocks; b++)
        for (ch = 0; ch < n; ch++)
            analyse(s->x[ch], pcm + b * 8 * n + ch, n, sb[b][ch]);
    for (ch = 0; ch < n; ch++)
        for (i = 0; i < 8; i++)
            sf[ch][i] = scale_factor(sb, s->blocks, ch, i);

    /* joint stereo: a subband (not the last) goes as mid and side where that costs less */
    if (s->mode == SBC_JOINT) {
        static float ms[16][2][8];
        u32 msf[2];

        for (i = 0; i < 7; i++) {
            for (b = 0; b < s->blocks; b++) {
                ms[b][0][i] = (sb[b][0][i] + sb[b][1][i]) * 0.5f;
                ms[b][1][i] = (sb[b][0][i] - sb[b][1][i]) * 0.5f;
            }
            msf[0] = scale_factor(ms, s->blocks, 0, i);
            msf[1] = scale_factor(ms, s->blocks, 1, i);
            if (msf[0] + msf[1] < sf[0][i] + sf[1][i]) {
                join |= 1u << (7 - i);
                sf[0][i] = msf[0];
                sf[1][i] = msf[1];
                for (b = 0; b < s->blocks; b++) {
                    sb[b][0][i] = ms[b][0][i];
                    sb[b][1][i] = ms[b][1][i];
                }
            }
        }
    }

    if (s->mode == SBC_STEREO || s->mode == SBC_JOINT) {
        allocate(s, 2, sf, bits, 0);
    } else {
        for (ch = 0; ch < n; ch++)
            allocate(s, 1, sf, bits, ch);
    }

    for (i = 0; i < length; i++)
        out[i] = 0;
    w.out = out;
    w.at = 0;
    w.crc = 0x0F;
    w.counting = 0;
    put(&w, 0x9C, 8);
    w.counting = 1;
    put(&w, s->rate, 2);
    put(&w, s->blocks / 4 - 1, 2);
    put(&w, s->mode, 2);
    put(&w, s->allocation, 1);
    put(&w, 1, 1);                      /* 8 subbands */
    put(&w, s->bitpool, 8);
    w.counting = 0;
    w.at += 8;                          /* the CRC, written at the end */
    w.counting = 1;
    if (s->mode == SBC_JOINT)
        put(&w, join, 8);
    for (ch = 0; ch < n; ch++)
        for (i = 0; i < 8; i++)
            put(&w, sf[ch][i], 4);
    w.counting = 0;
    out[3] = (u8)w.crc;

    for (b = 0; b < s->blocks; b++)
        for (ch = 0; ch < n; ch++)
            for (i = 0; i < 8; i++) {
                u32 levels, q;
                float scale, v;

                if (!bits[ch][i])
                    continue;
                levels = (1u << bits[ch][i]) - 1;
                scale = (float)(2u << sf[ch][i]);
                v = (sb[b][ch][i] / scale + 1.0f) * (float)levels * 0.5f;
                q = v <= 0 ? 0 : (u32)v;
                if (q > levels - 1)
                    q = levels - 1;
                put(&w, q, bits[ch][i]);
            }
    return length;
}
