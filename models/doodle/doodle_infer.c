/* The doodle denoiser (doodle.py) in C. Each convolution runs one
 * k32_matvec_i8 per output pixel over its 3x3 patch. Maps are int8: the
 * input (14x14x4), A and B (14x14xc0) and two 7x7xc1 maps. `up` writes its
 * output over B, whose pixel it adds first, and d1 over A the same way. */
#include "doodle_infer.h"

#include "../../include/runtime/k32_app_abi.h"
#include "../common/k32_host.h"

#define MAGIC 0x5532334Bu   /* "K32U" */
#define F_UP 1u
#define F_SKIP_E1 2u
#define F_SKIP_IN 4u
#define F_LAST 8u

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

int doodle_load(struct doodle_model* m, const uint8_t* blob, uint32_t len) {
    const uint32_t* h = (const uint32_t*)blob;
    if (len < 64 || h[0] != MAGIC || h[1] != 1 || h[9] != len || h[2] > DOODLE_C0_MAX || h[3] > DOODLE_C1_MAX ||
        h[5] > DOODLE_MAX_CONV || h[5] != 6 + h[4] || h[7] > 4)
        return -1;
    m->c0 = h[2]; m->c1 = h[3]; m->mids = h[4]; m->n_conv = h[5];
    m->n_cond = h[6]; m->n_sched = h[7]; m->width = h[8];
    g_p = blob + 64;
    g_left = len - 64;
    for (uint32_t i = 0; i < m->n_conv; i++) {
        struct doodle_conv* c = &m->conv[i];
        const uint32_t* d = take(16);
        const float* s = take(8);
        if (!d || !s) return -1;
        c->cin = d[0]; c->cout = d[1]; c->stride = d[2]; c->flags = d[3];
        c->scale = s[0];
        c->inv = s[1];
        c->ws = take(4 * c->cout);
        c->b = take(4 * c->cout);
        c->w = take(c->cout * 9 * c->cin);
        if (!c->w) return -1;
    }
    m->class_q = take(m->n_cond * m->width);
    m->class_s = take(4 * m->n_cond);
    for (uint32_t k = 0; k < m->n_sched; k++) {
        struct doodle_sched* s = &m->sched[k];
        const uint32_t* n = take(4);
        if (!n) return -1;
        s->steps = *n;
        s->alpha = take(4 * (s->steps + 1));
        s->sigma = take(4 * (s->steps + 1));
        s->tq = take(s->steps * m->width);
        s->ts = take(4 * s->steps);
        if (!s->ts) return -1;
    }
    return m->class_s && !g_left ? 0 : -1;
}

/* ---- arithmetic ---------------------------------------------------------- */
static int8_t q_static(float y, float inv) {
    const float t = y * inv;
    if (t >= 127.0f) return 127;
    if (t <= -127.0f) return -127;
    return (int8_t)(int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
}

static uint32_t g_rng;
static float uniform(void) {
    uint32_t s = g_rng;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    g_rng = s;
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}
static float gauss(void) {
    float acc = 0.0f;
    for (int i = 0; i < 12; i++) acc = acc + uniform();
    return acc - 6.0f;
}

/* ---- the network --------------------------------------------------------- */
static int8_t g_in[14 * 14 * 4], g_a[14 * 14 * DOODLE_C0_MAX], g_b[14 * 14 * DOODLE_C0_MAX];
static int8_t g_m[2][7 * 7 * DOODLE_C1_MAX];
static int8_t g_patch[9 * (DOODLE_C1_MAX > DOODLE_C0_MAX ? DOODLE_C1_MAX : DOODLE_C0_MAX)];
static float g_y[DOODLE_C1_MAX > DOODLE_C0_MAX ? DOODLE_C1_MAX : DOODLE_C0_MAX];
static float g_cond[4 * DOODLE_C0_MAX + (DOODLE_MAX_CONV - 4) * DOODLE_C1_MAX];

/* conv c over in (H x W, or 7x7 read as 14x14 with F_UP) into out
 * (quantized with next_inv) or, for the last one, into v (28x28 floats) */
static void conv(const struct doodle_conv* c, const int8_t* in, uint32_t H, const float* cond, const int8_t* skip,
                 float skip_scale, float next_inv, int8_t* out, float* v) {
    const uint32_t Hin = c->flags & F_UP ? 2 * H : H;
    const uint32_t Ho = (Hin + c->stride - 1) / c->stride, cin = c->cin;
    for (uint32_t y = 0; y < Ho; y++)
        for (uint32_t x = 0; x < Ho; x++) {
            uint32_t n = 0;
            for (uint32_t ky = 0; ky < 3; ky++)
                for (uint32_t kx = 0; kx < 3; kx++) {
                    const int32_t iy = (int32_t)(c->stride * y + ky) - 1, ix = (int32_t)(c->stride * x + kx) - 1;
                    if (iy < 0 || ix < 0 || (uint32_t)iy >= Hin || (uint32_t)ix >= Hin) {
                        for (uint32_t k = 0; k < cin; k++) g_patch[n++] = 0;
                        continue;
                    }
                    const uint32_t sy = c->flags & F_UP ? (uint32_t)iy >> 1 : (uint32_t)iy;
                    const uint32_t sx = c->flags & F_UP ? (uint32_t)ix >> 1 : (uint32_t)ix;
                    const int8_t* src = in + (sy * H + sx) * cin;
                    for (uint32_t k = 0; k < cin; k++) g_patch[n++] = src[k];
                }
            k32_matvec_i8(g_y, c->w, c->ws, g_patch, c->scale, c->cout, 9 * cin, 0);
            const uint32_t p = (y * Ho + x) * c->cout;
            for (uint32_t o = 0; o < c->cout; o++) {
                float t = g_y[o] + c->b[o];
                if (c->flags & F_LAST) {
                    /* channel 2 dy + dx of the 14x14 map is pixel (2y + dy, 2x + dx) */
                    v[(2 * y + (o >> 1)) * 28 + 2 * x + (o & 1)] = t;
                    continue;
                }
                t = t + cond[o];
                if (skip) t = t + (float)skip[p + o] * skip_scale;
                t = t * (1.0f / (1.0f + k32_expf(-t)));
                out[p + o] = q_static(t, next_inv);
            }
        }
}

/* v of the image x at a step's condition */
static void forward(const struct doodle_model* m, const float* x, const float* cond, float* v) {
    const struct doodle_conv* c = m->conv;
    for (uint32_t y = 0; y < 14; y++)
        for (uint32_t xx = 0; xx < 14; xx++)
            for (uint32_t k = 0; k < 4; k++)
                g_in[(y * 14 + xx) * 4 + k] = q_static(x[(2 * y + (k >> 1)) * 28 + 2 * xx + (k & 1)], c[0].inv);
    const float* cd = cond;
    conv(&c[0], g_in, 14, cd, 0, 0.0f, c[1].inv, g_a, 0);                 /* in -> A */
    cd += c[0].cout;
    conv(&c[1], g_a, 14, cd, 0, 0.0f, c[2].inv, g_b, 0);                  /* e1 -> B */
    cd += c[1].cout;
    conv(&c[2], g_b, 14, cd, 0, 0.0f, c[3].inv, g_m[0], 0);               /* down -> 7x7 */
    cd += c[2].cout;
    uint32_t i = 3, cur = 0;
    for (; i < 3 + m->mids; i++, cur ^= 1) {
        conv(&c[i], g_m[cur], 7, cd, 0, 0.0f, c[i + 1].inv, g_m[cur ^ 1], 0);
        cd += c[i].cout;
    }
    conv(&c[i], g_m[cur], 7, cd, g_b, c[2].scale, c[i + 1].inv, g_b, 0);   /* up + B -> B */
    cd += c[i].cout;
    conv(&c[i + 1], g_b, 14, cd, g_a, c[1].scale, c[i + 2].inv, g_a, 0);   /* d1 + A -> A */
    conv(&c[i + 2], g_a, 14, 0, 0, 0.0f, 0.0f, 0, v);                      /* out -> v */
}

static float g_vc[784], g_vu[784];

int doodle_sample(const struct doodle_model* m, uint32_t cls, uint32_t seed, uint32_t steps, float guidance, float x[784]) {
    const struct doodle_sched* s = 0;
    for (uint32_t k = 0; k < m->n_sched; k++)
        if (m->sched[k].steps == steps) s = &m->sched[k];
    if (!s || cls >= m->n_cond) return -1;
    g_rng = seed ? seed : 0x6D2B79F5u;
    for (uint32_t i = 0; i < 784; i++) x[i] = gauss();
    for (uint32_t i = 0; i < steps; i++) {
        const int8_t* tq = s->tq + i * m->width;
        for (uint32_t k = 0; k < m->width; k++)
            g_cond[k] = (float)tq[k] * s->ts[i] + (float)m->class_q[cls * m->width + k] * m->class_s[cls];
        forward(m, x, g_cond, g_vc);
        if (guidance != 1.0f) {
            for (uint32_t k = 0; k < m->width; k++)
                g_cond[k] = (float)tq[k] * s->ts[i] + (float)m->class_q[DOODLE_NONE * m->width + k] * m->class_s[DOODLE_NONE];
            forward(m, x, g_cond, g_vu);
            for (uint32_t p = 0; p < 784; p++) g_vc[p] = g_vu[p] + guidance * (g_vc[p] - g_vu[p]);
        }
        const float a = s->alpha[i], sg = s->sigma[i], an = s->alpha[i + 1], sn = s->sigma[i + 1];
        for (uint32_t p = 0; p < 784; p++) {
            float x0 = a * x[p] - sg * g_vc[p];
            x0 = x0 < -1.0f ? -1.0f : x0 > 1.0f ? 1.0f : x0;
            const float eps = sg * x[p] + a * g_vc[p];
            x[p] = an * x0 + sn * eps;
        }
    }
    return 0;
}
