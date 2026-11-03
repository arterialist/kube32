/* The doodle denoiser (doodle.py) in C on the node's imports. Each
 * convolution is one k32_conv3x3_i8 call per chunk of output rows, over
 * the maps of both conditions of a guided step at once (the class and "no
 * class", so the weights stream from the blob once a step), then per output
 * row the step and class bias (k32_add_f32), the skip (k32_add_f32_i8),
 * SiLU (k32_swiglu_f32 with a gate of ones) and the next convolution's
 * int8 input (k32_quantize_i8_static). Maps are int8, pixel after pixel,
 * one map per condition: the input (14x14x4), A and B (14x14xc0) and two
 * 7x7xc1 maps. `up` writes its output over B, whose pixel it adds first,
 * and d1 over A the same way. The image and v are kept as 14x14 pixels of
 * 4 channels (a 2x2 block each) inside a pass. */
#include "doodle_infer.h"

#include "../../include/runtime/k32_app_abi.h"
#include "../common/k32_host.h"

#define MAGIC 0x5532334Bu   /* "K32U" */
#define F_UP 1u
#define F_SKIP_E1 2u
#define F_SKIP_IN 4u
#define F_LAST 8u
#define NB 2                /* conditions in one pass */
#define CMAX (DOODLE_C1_MAX > DOODLE_C0_MAX ? DOODLE_C1_MAX : DOODLE_C0_MAX)
/* floats of a chunk's convolution output: 7 rows of a 14x14 map, or a 7x7 map */
#define YF (NB * 7 * 14 * DOODLE_C0_MAX > NB * 49 * DOODLE_C1_MAX ? NB * 7 * 14 * DOODLE_C0_MAX : NB * 49 * DOODLE_C1_MAX)
/* floats of one output row: 14 pixels of c0, or 7 of c1 */
#define ROWF (14 * DOODLE_C0_MAX > 7 * DOODLE_C1_MAX ? 14 * DOODLE_C0_MAX : 7 * DOODLE_C1_MAX)

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

/* ---- the noise ----------------------------------------------------------- */
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
static int8_t g_in[NB * 14 * 14 * 4], g_a[NB * 14 * 14 * DOODLE_C0_MAX], g_b[NB * 14 * 14 * DOODLE_C0_MAX];
static int8_t g_m[2][NB * 7 * 7 * DOODLE_C1_MAX];
static float g_y[YF];                 /* a chunk's convolution output, [condition][row][pixel][channel] */
static float g_rep[NB][ROWF];         /* a convolution's step and class bias, repeated for a row's pixels */
static float g_ones[ROWF];
static float g_v[NB][784];            /* v of each condition, 14x14 pixels of 4 */
static float g_cond[NB][4 * DOODLE_C0_MAX + (DOODLE_MAX_CONV - 4) * DOODLE_C1_MAX];

/* conv c over the nb maps `in` (H x H, 7x7 read as 14x14 with F_UP) into
 * out (quantized with next_inv) or, for the last one, into v */
static void conv(const struct doodle_conv* c, const int8_t* in, uint32_t H, uint32_t nb, const float* cond, const int8_t* skip,
                 float skip_scale, float next_inv, int8_t* out, float* v) {
    const uint32_t up = c->flags & F_UP ? 1u : 0u, cout = c->cout;
    const uint32_t Ho = ((H << up) + c->stride - 1) / c->stride, rowf = Ho * cout;
    uint32_t R = YF / (nb * rowf);     /* output rows a chunk */
    if (R > Ho) R = Ho;
    if (cond) {
        for (uint32_t b = 0; b < nb; b++) {
            float* rep = g_rep[b];
            for (uint32_t o = 0; o < cout; o++) rep[o] = cond[b * (sizeof g_cond[0] / sizeof(float)) + o];
            for (uint32_t done = 1; done < Ho; done *= 2) {
                const uint32_t n = Ho - done < done ? Ho - done : done;
                __builtin_memcpy(rep + done * cout, rep, n * cout * sizeof(float));
            }
        }
    }
    for (uint32_t y0 = 0; y0 < Ho; y0 += R) {
        const uint32_t y1 = y0 + R < Ho ? y0 + R : Ho;
        k32_conv3x3_i8(g_y, c->w, c->ws, c->b, in, c->scale, c->cin, cout, H, H, c->stride, up, nb, y0, y1);
        for (uint32_t b = 0; b < nb; b++) {
            if (c->flags & F_LAST) {
                __builtin_memcpy(v + b * 784 + y0 * rowf, g_y + b * (y1 - y0) * rowf, (y1 - y0) * rowf * sizeof(float));
                continue;
            }
            for (uint32_t y = y0; y < y1; y++) {
                float* row = g_y + (b * (y1 - y0) + (y - y0)) * rowf;
                const uint32_t at = (b * Ho + y) * rowf;
                k32_add_f32(row, g_rep[b], rowf);
                if (skip) k32_add_f32_i8(row, skip + at, skip_scale, rowf);
                k32_swiglu_f32(row, g_ones, rowf);
                k32_quantize_i8_static(out + at, row, next_inv, rowf);
            }
        }
    }
}

/* v of the image x at nb conditions (g_cond): v[b] as 14x14 pixels of 4 */
static void forward(const struct doodle_model* m, const float* x, uint32_t nb, float* v) {
    const struct doodle_conv* c = m->conv;
    for (uint32_t y = 0; y < 14; y++)
        for (uint32_t xx = 0; xx < 14; xx++)
            for (uint32_t k = 0; k < 4; k++)
                g_y[(y * 14 + xx) * 4 + k] = x[(2 * y + (k >> 1)) * 28 + 2 * xx + (k & 1)];
    k32_quantize_i8_static(g_in, g_y, c[0].inv, 784);
    for (uint32_t b = 1; b < nb; b++) __builtin_memcpy(g_in + b * 784, g_in, 784);
    for (uint32_t i = 0; i < ROWF; i++) g_ones[i] = 1.0f;
    const float* cd = g_cond[0];
    conv(&c[0], g_in, 14, nb, cd, 0, 0.0f, c[1].inv, g_a, 0);                 /* in -> A */
    cd += c[0].cout;
    conv(&c[1], g_a, 14, nb, cd, 0, 0.0f, c[2].inv, g_b, 0);                  /* e1 -> B */
    cd += c[1].cout;
    conv(&c[2], g_b, 14, nb, cd, 0, 0.0f, c[3].inv, g_m[0], 0);               /* down -> 7x7 */
    cd += c[2].cout;
    uint32_t i = 3, cur = 0;
    for (; i < 3 + m->mids; i++, cur ^= 1) {
        conv(&c[i], g_m[cur], 7, nb, cd, 0, 0.0f, c[i + 1].inv, g_m[cur ^ 1], 0);
        cd += c[i].cout;
    }
    conv(&c[i], g_m[cur], 7, nb, cd, g_b, c[2].scale, c[i + 1].inv, g_b, 0);   /* up + B -> B */
    cd += c[i].cout;
    conv(&c[i + 1], g_b, 14, nb, cd, g_a, c[1].scale, c[i + 2].inv, g_a, 0);   /* d1 + A -> A */
    conv(&c[i + 2], g_a, 14, nb, 0, 0, 0.0f, 0.0f, 0, v);                      /* out -> v */
}

void (*doodle_on_step)(uint32_t step, uint32_t steps, const float* x);

static const struct doodle_sched* schedule(const struct doodle_model* m, uint32_t steps) {
    for (uint32_t k = 0; k < m->n_sched; k++)
        if (m->sched[k].steps == steps) return &m->sched[k];
    return 0;
}

int doodle_begin(const struct doodle_model* m, uint32_t cls, uint32_t seed, uint32_t steps, float x[784]) {
    if (!schedule(m, steps) || cls >= m->n_cond) return -1;
    g_rng = seed ? seed : 0x6D2B79F5u;
    for (uint32_t i = 0; i < 784; i++) x[i] = gauss();
    return 0;
}

static void condition(const struct doodle_model* m, const struct doodle_sched* s, uint32_t i, uint32_t cls, float* cond) {
    const int8_t* tq = s->tq + i * m->width;
    for (uint32_t k = 0; k < m->width; k++)
        cond[k] = (float)tq[k] * s->ts[i] + (float)m->class_q[cls * m->width + k] * m->class_s[cls];
}

int doodle_step(const struct doodle_model* m, uint32_t cls, uint32_t steps, uint32_t i, float guidance, float x[784]) {
    const struct doodle_sched* s = schedule(m, steps);
    if (!s || cls >= m->n_cond || i >= steps) return -1;
    const uint32_t nb = guidance != 1.0f ? 2 : 1;
    condition(m, s, i, cls, g_cond[0]);
    if (nb == 2) condition(m, s, i, DOODLE_NONE, g_cond[1]);
    forward(m, x, nb, g_v[0]);
    const float *vc = g_v[0], *vu = g_v[1];
    const float a = s->alpha[i], sg = s->sigma[i], an = s->alpha[i + 1], sn = s->sigma[i + 1];
    for (uint32_t p = 0; p < 784; p++) {
        /* channel k of 14x14 pixel (y, x) is image pixel (2y + k / 2, 2x + k % 2) */
        const uint32_t k = p & 3, q = p >> 2, py = q / 14, px = q % 14;
        float* xp = &x[(2 * py + (k >> 1)) * 28 + 2 * px + (k & 1)];
        float v = vc[p];
        if (nb == 2) v = vu[p] + guidance * (v - vu[p]);
        float x0 = a * *xp - sg * v;
        x0 = x0 < -1.0f ? -1.0f : x0 > 1.0f ? 1.0f : x0;
        const float eps = sg * *xp + a * v;
        *xp = an * x0 + sn * eps;
    }
    return 0;
}

int doodle_sample(const struct doodle_model* m, uint32_t cls, uint32_t seed, uint32_t steps, float guidance, float x[784]) {
    if (doodle_begin(m, cls, seed, steps, x)) return -1;
    if (doodle_on_step) doodle_on_step(0, steps, x);
    for (uint32_t i = 0; i < steps; i++) {
        doodle_step(m, cls, steps, i, guidance, x);
        if (doodle_on_step) doodle_on_step(i + 1, steps, x);
    }
    return 0;
}
