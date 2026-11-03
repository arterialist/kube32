/* The keyword spotter's network (kws.py) in C. The convolutions run as one
 * k32_matvec_i8 per output pixel over its patch; the last two (a 3x3 and a
 * 1x1) run pixel by pixel together, so their maps are never stored, and
 * the mean over pixels adds up as they go. Memory: two int8 maps of at most
 * KWS_MAP_MAX bytes plus the input's. */
#include "kws_infer.h"

#include "../../include/runtime/k32_app_abi.h"
#include "../common/k32_host.h"

#define MAGIC 0x4B32334Bu   /* "K32K" */

static const uint8_t* g_p;
static uint32_t g_left;

static const void* take(uint32_t n) {
    n = (n + 3u) & ~3u;
    if (n > g_left) return 0;
    const void* p = g_p;
    g_p += n;
    g_left -= n;
    return p;
}

int kws_load(struct kws_model* m, const uint8_t* blob, uint32_t len) {
    const uint32_t* h = (const uint32_t*)blob;
    if (len < 64 || h[0] != MAGIC || h[1] != 1 || h[7] != len || h[2] < 2 || h[2] > KWS_MAX_CONV || h[3] != KWS_CLASSES ||
        h[4] != KWS_FRAMES || h[5] != KWS_MELS)
        return -1;
    m->n_conv = h[2];
    m->n_classes = h[3];
    m->offset = (int32_t)h[6];
    kws_tables_from_blob(&m->fe, blob);
    uint32_t mel_w = 0;
    for (uint32_t i = 0; i < KWS_MELS; i++) mel_w += m->fe.mel_len[i];
    g_p = blob + 64;
    g_left = len - 64;
    if (!take(2 * (KWS_WIN + KWS_NFFT + 2 * KWS_MELS + mel_w))) return -1;
    for (uint32_t i = 0; i < m->n_conv; i++) {
        struct kws_conv* c = &m->conv[i];
        const uint32_t* d = take(32);
        const float* s = take(8);
        if (!d || !s) return -1;
        c->ci = d[0]; c->co = d[1]; c->kh = d[2]; c->kw = d[3];
        c->sh = d[4]; c->sw = d[5]; c->ph = d[6]; c->pw = d[7];
        c->scale = s[0];
        c->inv = s[1];
        c->ws = take(4 * c->co);
        c->b = take(4 * c->co);
        c->w = take(c->co * c->kh * c->kw * c->ci);
        if (!c->w || c->co > 64 || c->kh * c->kw * c->ci > 512) return -1;
    }
    const uint32_t* d = take(8);
    if (!d) return -1;
    m->fc_ci = d[0];
    m->fc_co = d[1];
    m->fc_ws = take(4 * m->fc_co);
    m->fc_b = take(4 * m->fc_co);
    m->fc_w = take(m->fc_co * m->fc_ci);
    const struct kws_conv* last = &m->conv[m->n_conv - 1];
    if (!m->fc_w || g_left || m->fc_co != KWS_CLASSES || m->fc_ci != last->co || last->kh != 1 || last->kw != 1) return -1;
    return 0;
}

/* round(y * inv) half away from 0, clamped to -127..127 */
static int8_t q_static(float y, float inv) {
    const float t = y * inv;
    if (t >= 127.0f) return 127;
    if (t <= -127.0f) return -127;
    return (int8_t)(int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
}

static int8_t g_in[KWS_FRAMES * KWS_MELS], g_a[KWS_MAP_MAX], g_b[KWS_MAP_MAX];
static int8_t g_patch[512];
static float g_y[64], g_z[64], g_acc[64];

/* the 3x3 (or any) patch of output pixel (y, x), zero outside the map */
static void patch(const struct kws_conv* c, const int8_t* in, uint32_t H, uint32_t W, uint32_t y, uint32_t x) {
    uint32_t n = 0;
    for (uint32_t ky = 0; ky < c->kh; ky++)
        for (uint32_t kx = 0; kx < c->kw; kx++) {
            const int32_t iy = (int32_t)(c->sh * y + ky) - (int32_t)c->ph;
            const int32_t ix = (int32_t)(c->sw * x + kx) - (int32_t)c->pw;
            if (iy >= 0 && ix >= 0 && (uint32_t)iy < H && (uint32_t)ix < W) {
                const int8_t* src = in + ((uint32_t)iy * W + (uint32_t)ix) * c->ci;
                for (uint32_t ci = 0; ci < c->ci; ci++) g_patch[n++] = src[ci];
            } else {
                for (uint32_t ci = 0; ci < c->ci; ci++) g_patch[n++] = 0;
            }
        }
}

/* y = W patch * scales + bias, ReLU */
static void conv_pixel(const struct kws_conv* c, const int8_t* x, float* y) {
    k32_matvec_i8(y, c->w, c->ws, x, c->scale, c->co, c->kh * c->kw * c->ci, 0);
    for (uint32_t o = 0; o < c->co; o++) {
        const float v = y[o] + c->b[o];
        y[o] = v > 0.0f ? v : 0.0f;
    }
}

int kws_classify(const struct kws_model* m, const int32_t* features, float logits[KWS_CLASSES]) {
    for (uint32_t i = 0; i < KWS_FRAMES * KWS_MELS; i++) g_in[i] = q_static((float)(features[i] - m->offset), m->conv[0].inv);
    const int8_t* in = g_in;
    uint32_t H = KWS_FRAMES, W = KWS_MELS;
    int8_t* out = g_a;
    /* every convolution but the last pair: whole maps */
    for (uint32_t i = 0; i + 2 < m->n_conv; i++) {
        const struct kws_conv* c = &m->conv[i];
        const uint32_t Ho = (H + 2 * c->ph - c->kh) / c->sh + 1, Wo = (W + 2 * c->pw - c->kw) / c->sw + 1;
        if (Ho * Wo * c->co > KWS_MAP_MAX) return -1;
        const float next_inv = m->conv[i + 1].inv;
        for (uint32_t y = 0; y < Ho; y++)
            for (uint32_t x = 0; x < Wo; x++) {
                patch(c, in, H, W, y, x);
                conv_pixel(c, g_patch, g_y);
                for (uint32_t o = 0; o < c->co; o++) out[(y * Wo + x) * c->co + o] = q_static(g_y[o], next_inv);
            }
        in = out;
        out = out == g_a ? g_b : g_a;
        H = Ho;
        W = Wo;
    }
    /* the last two, pixel by pixel, into the mean */
    const struct kws_conv* c = &m->conv[m->n_conv - 2];
    const struct kws_conv* d = &m->conv[m->n_conv - 1];
    const uint32_t Ho = (H + 2 * c->ph - c->kh) / c->sh + 1, Wo = (W + 2 * c->pw - c->kw) / c->sw + 1;
    for (uint32_t o = 0; o < d->co; o++) g_acc[o] = 0.0f;
    int8_t q[64];
    for (uint32_t y = 0; y < Ho; y++)
        for (uint32_t x = 0; x < Wo; x++) {
            patch(c, in, H, W, y, x);
            conv_pixel(c, g_patch, g_y);
            for (uint32_t o = 0; o < c->co; o++) q[o] = q_static(g_y[o], d->inv);
            conv_pixel(d, q, g_z);
            for (uint32_t o = 0; o < d->co; o++) g_acc[o] = g_acc[o] + g_z[o];
        }
    const float n = (float)(Ho * Wo);
    float amax = 0.0f;
    for (uint32_t o = 0; o < d->co; o++) {
        g_acc[o] = g_acc[o] / n;
        const float a = __builtin_fabsf(g_acc[o]);
        if (a > amax) amax = a;
    }
    /* one scale for the vector, as llm-pipeline's quantize() */
    const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
    for (uint32_t o = 0; o < d->co; o++) {
        const float t = g_acc[o] * inv;
        int32_t r = (int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
        q[o] = (int8_t)(r > 127 ? 127 : r < -127 ? -127 : r);
    }
    k32_matvec_i8(logits, m->fc_w, m->fc_ws, q, amax / 127.0f, m->fc_co, m->fc_ci, 0);
    int best = 0;
    for (uint32_t k = 0; k < m->fc_co; k++) {
        logits[k] = logits[k] + m->fc_b[k];
        if (logits[k] > logits[best]) best = (int)k;
    }
    return best;
}
