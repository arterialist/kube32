/* The masked diffusion model (models/lm/mdlm.py) in C. A step runs the
 * window through the layers: per layer, q, k and v of every position first
 * (K and V int8 with a scale per head, as llm-shard's cache), then per
 * position the attention over the whole window, the output projection and
 * the feed-forward block. Then each masked position's logits give its
 * candidate token and confidence, and the most confident are revealed. */
#include "mdlm_infer.h"

#include "../../include/runtime/k32_app_abi.h"
#include "../common/k32_host.h"

#define MAGIC 0x4D32334Bu        /* "K32M" */
#define F_EMBED 1u
#define F_HEAD 2u
#define F_TIED 4u
#define F_BIDIR 8u

int mdlm_load(struct mdlm_model* m, const uint8_t* blob, uint32_t len) {
    const uint32_t* h = (const uint32_t*)blob;
    const uint32_t want = F_EMBED | F_HEAD | F_TIED | F_BIDIR;
    if (len < 64 || h[0] != MAGIC || h[1] != 1 || h[12] != len || (h[11] & want) != want || h[13] != 8 || h[8] != 0 ||
        h[9] != h[10])
        return -1;
    m->dim = h[2]; m->hidden = h[3]; m->n_heads = h[4]; m->n_kv_heads = h[5];
    m->vocab = h[6]; m->window = h[7]; m->n_layers = h[10];
    if (m->dim > MDLM_MAX_DIM || m->hidden > MDLM_MAX_HIDDEN || m->n_layers > MDLM_MAX_LAYERS || m->vocab > MDLM_MAX_VOCAB ||
        m->window != MDLM_WINDOW || !m->n_heads || !m->n_kv_heads || m->n_heads % m->n_kv_heads || m->dim % m->n_heads)
        return -1;
    const uint32_t dim = m->dim, hid = m->hidden, hs = dim / m->n_heads, kvd = m->n_kv_heads * hs;
    const uint8_t* c = blob + 64;
    m->tok_q = (const int8_t*)c; c += m->vocab * dim;
    m->tok_s = (const float*)c; c += 4 * m->vocab;
    m->rope = (const float*)c; c += 4 * m->window * hs;
    for (uint32_t l = 0; l < m->n_layers; l++) {
        struct mdlm_layer* L = &m->layer[l];
        const float** fv[] = {&L->rms_att, &L->wq_s, &L->wk_s, &L->wv_s, &L->wo_s, &L->rms_ffn, &L->w1_s, &L->w2_s, &L->w3_s};
        const uint32_t fn[] = {dim, dim, kvd, kvd, dim, dim, hid, dim, hid};
        for (int i = 0; i < 9; i++) { *fv[i] = (const float*)c; c += 4 * fn[i]; }
        const int8_t** mv[] = {&L->wq, &L->wk, &L->wv, &L->wo, &L->w1, &L->w2, &L->w3};
        const uint32_t mn[] = {dim * dim, kvd * dim, kvd * dim, dim * dim, hid * dim, dim * hid, hid * dim};
        for (int i = 0; i < 7; i++) { *mv[i] = (const int8_t*)c; c += mn[i]; }
    }
    m->rms_final = (const float*)c; c += 4 * dim;
    return c == blob + len ? 0 : -1;
}

/* ---- arithmetic, as llm-shard's ----------------------------------------- */
static float g_x[MDLM_WINDOW][MDLM_MAX_DIM], g_q[MDLM_WINDOW][MDLM_MAX_DIM];
static float g_xb[MDLM_MAX_DIM], g_xb2[MDLM_MAX_DIM], g_k[MDLM_MAX_DIM], g_v[MDLM_MAX_DIM];
static float g_hb[MDLM_MAX_HIDDEN], g_hb2[MDLM_MAX_HIDDEN], g_att[MDLM_WINDOW], g_logits[MDLM_MAX_VOCAB];
static int8_t g_xq[MDLM_MAX_HIDDEN], g_kq[MDLM_WINDOW][MDLM_MAX_DIM], g_vq[MDLM_WINDOW][MDLM_MAX_DIM];
static float g_ks[MDLM_WINDOW][MDLM_MAX_DIM / 8], g_vs[MDLM_WINDOW][MDLM_MAX_DIM / 8];

static void norm(float* out, const float* x, const float* w, uint32_t n) {
    float ss = 0.0f;
    for (uint32_t i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (float)n;
    ss = ss + 1e-5f;
    ss = 1.0f / __builtin_sqrtf(ss);
    for (uint32_t i = 0; i < n; i++) out[i] = w[i] * (ss * x[i]);
}

static float quantize(int8_t* q, const float* src, uint32_t n) {
    float amax = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const float a = __builtin_fabsf(src[i]);
        if (a > amax) amax = a;
    }
    const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
    for (uint32_t i = 0; i < n; i++) {
        const float t = src[i] * inv;
        int32_t r = (int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
        q[i] = (int8_t)(r > 127 ? 127 : r < -127 ? -127 : r);
    }
    return amax / 127.0f;
}

static void rope(float* a, uint32_t n, const float* cs, uint32_t hs) {
    for (uint32_t i = 0; i < n; i += 2) {
        const float c = cs[i % hs], s = cs[i % hs + 1];
        const float a0 = a[i], a1 = a[i + 1];
        a[i] = a0 * c - a1 * s;
        a[i + 1] = a0 * s + a1 * c;
    }
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

/* the window through every layer, into g_x */
static void forward(const struct mdlm_model* m, const uint32_t* tok) {
    const uint32_t dim = m->dim, hid = m->hidden, H = m->n_heads, KVH = m->n_kv_heads, hs = dim / H, kvd = KVH * hs, P = m->window;
    const float scale = 1.0f / __builtin_sqrtf((float)hs);
    for (uint32_t p = 0; p < P; p++)
        for (uint32_t i = 0; i < dim; i++) g_x[p][i] = (float)m->tok_q[tok[p] * dim + i] * m->tok_s[tok[p]];
    for (uint32_t l = 0; l < m->n_layers; l++) {
        const struct mdlm_layer* L = &m->layer[l];
        for (uint32_t p = 0; p < P; p++) {
            const float* cs = m->rope + p * hs;
            norm(g_xb, g_x[p], L->rms_att, dim);
            const float s = quantize(g_xq, g_xb, dim);
            k32_matvec_i8(g_q[p], L->wq, L->wq_s, g_xq, s, dim, dim, 0);
            k32_matvec_i8(g_k, L->wk, L->wk_s, g_xq, s, kvd, dim, 0);
            k32_matvec_i8(g_v, L->wv, L->wv_s, g_xq, s, kvd, dim, 0);
            rope(g_q[p], dim, cs, hs);
            rope(g_k, kvd, cs, hs);
            for (uint32_t h = 0; h < KVH; h++) {
                g_ks[p][h] = quantize(g_kq[p] + h * hs, g_k + h * hs, hs);
                g_vs[p][h] = quantize(g_vq[p] + h * hs, g_v + h * hs, hs);
            }
        }
        for (uint32_t p = 0; p < P; p++) {
            for (uint32_t h = 0; h < H; h++) {
                const uint32_t kh = h / (H / KVH);
                k32_matvec_f32_i8(g_att, &g_kq[0][kh * hs], &g_ks[0][kh], g_q[p] + h * hs, scale, P, hs, sizeof g_kq[0],
                                  sizeof g_ks[0]);
                k32_softmax_f32(g_att, P);
                k32_vecmat_f32_i8(g_xb2 + h * hs, &g_vq[0][kh * hs], &g_vs[0][kh], g_att, P, hs, sizeof g_vq[0], sizeof g_vs[0]);
            }
            float s = quantize(g_xq, g_xb2, dim);
            k32_matvec_i8(g_xb, L->wo, L->wo_s, g_xq, s, dim, dim, 0);
            for (uint32_t i = 0; i < dim; i++) g_x[p][i] += g_xb[i];
            norm(g_xb, g_x[p], L->rms_ffn, dim);
            s = quantize(g_xq, g_xb, dim);
            k32_matvec_i8(g_hb, L->w1, L->w1_s, g_xq, s, hid, dim, 0);
            k32_matvec_i8(g_hb2, L->w3, L->w3_s, g_xq, s, hid, dim, 0);
            for (uint32_t i = 0; i < hid; i++) {
                float a = g_hb[i];
                a = a * (1.0f / (1.0f + k32_expf(-a)));
                g_hb[i] = a * g_hb2[i];
            }
            s = quantize(g_xq, g_hb, hid);
            k32_matvec_i8(g_xb, L->w2, L->w2_s, g_xq, s, dim, hid, 0);
            for (uint32_t i = 0; i < dim; i++) g_x[p][i] += g_xb[i];
        }
    }
}

static uint32_t g_cand[MDLM_WINDOW];
static float g_conf[MDLM_WINDOW];

int mdlm_sample(const struct mdlm_model* m, const uint32_t* prompt, uint32_t prompt_len, uint32_t steps, uint32_t seed,
                float temperature, uint32_t* x) {
    const uint32_t P = m->window, V = m->vocab - 1, MASK = m->vocab - 1;   /* the last entry is MASK */
    if (!steps || prompt_len > P) return -1;
    for (uint32_t p = 0; p < P; p++) x[p] = p < prompt_len ? prompt[p] : MASK;
    uint32_t M = 0;
    for (uint32_t p = 0; p < P; p++) M += x[p] == MASK;
    g_rng = seed ? seed : 0x6D2B79F5u;
    const float inv_t = temperature > 0.0f ? 1.0f / temperature : 0.0f;
    for (uint32_t st = 0; st < steps; st++) {
        uint32_t n = M * (st + 1) / steps - M * st / steps, masked = 0;
        for (uint32_t p = 0; p < P; p++) masked += x[p] == MASK;
        if (!masked || !n) continue;
        forward(m, x);
        for (uint32_t p = 0; p < P; p++) {
            if (x[p] != MASK) continue;
            norm(g_xb, g_x[p], m->rms_final, m->dim);
            const float s = quantize(g_xq, g_xb, m->dim);
            k32_matvec_i8(g_logits, m->tok_q, m->tok_s, g_xq, s, V, m->dim, 0);
            uint32_t c = 0;
            if (temperature > 0.0f) {
                for (uint32_t i = 0; i < V; i++) g_logits[i] = g_logits[i] * inv_t;
                k32_softmax_f32(g_logits, V);
                const float u = uniform();
                float run = 0.0f;
                uint32_t i = 0, last = 0;
                for (; i < V; i++) {
                    if (g_logits[i] > 0.0f) last = i;
                    run = run + g_logits[i];
                    if (run > u) break;
                }
                c = i < V ? i : last;   /* else the last token with a nonzero probability */
            } else {
                for (uint32_t i = 1; i < V; i++)
                    if (g_logits[i] > g_logits[c]) c = i;
                k32_softmax_f32(g_logits, V);
            }
            g_cand[p] = c;
            g_conf[p] = g_logits[c];
        }
        /* reveal the n most confident, the lower position first among equal ones */
        for (uint32_t k = 0; k < n; k++) {
            int32_t best = -1;
            for (uint32_t p = 0; p < P; p++)
                if (x[p] == MASK && (best < 0 || g_conf[p] > g_conf[best])) best = (int32_t)p;
            if (best < 0) break;
            x[best] = g_cand[best];
        }
    }
    return 0;
}
