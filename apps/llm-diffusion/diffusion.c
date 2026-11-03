/* llm-diffusion: a masked diffusion language model (models/mdlm) as a pod.
 *
 * The weights stay in the node's flash. The pod opens the blob named by env
 * BLOB (default "mdlm"): models/mdlm/mdlm.bin without its rotary table, which
 * tools/pack.py writes with the tokenizer into the module's data (tables.h).
 * The linear algebra imports read every matrix from the blob in place; the
 * pod's memory holds the 64-token window's activations and the norm vectors.
 *
 * A generation starts with all of the window masked except BOS and the
 * prompt. Each step runs the window through the 4 layers (k32_matmul_i8
 * applies a matrix to all positions at once, so a matrix is read from flash
 * once a step for 64 positions), takes the most likely token of every masked
 * position with its confidence and reveals the most confident ones. k32_tick
 * computes one layer a call (10 ms apart), so the node's other pods and the
 * pod's own requests are not held for a whole step.
 *
 *   GET|POST /generate?steps=<n>&seed=<n>&temp=<t x 100>&prompt=<text>
 *       Starts a generation: 1 to 64 steps (default 16), temperature
 *       t / 100 (default 0: greedy), the prompt URL-encoded (default: none).
 *       409 while one runs.
 *   GET /state?from=<step>
 *       {"state":"idle|running|done", ... } with the steps from <step> on:
 *       each step's text (masked positions as "_"), tokens revealed, and the
 *       time it took; once done, the tokens and the totals.
 *   GET /   the model and the memory the pod holds
 *
 * Every float step is the one of models/mdlm/mdlm_infer.c, so the window is
 * the reference's, token for token. */
#include "../../include/runtime/k32_app_abi.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);

#define NOINLINE __attribute__((noinline))

#define MAGIC 0x4D32334Bu            /* "K32M" */
#define F_EMBED 1u
#define F_HEAD 2u
#define F_TIED 4u
#define F_BIDIR 8u

#define P 64                          /* the window */
#define MAX_DIM 128
#define MAX_HID 320
#define MAX_LAYERS 4
#define MAX_VOCAB 513
#define MAX_STEPS 64
#define FFN_CHUNK 32                  /* positions a feed-forward product takes: the matrix is read P / FFN_CHUNK times */
#define CLS_CHUNK 40                  /* masked positions a classifier product takes */
#define WORK_FLOATS (CLS_CHUNK * (MAX_VOCAB - 1))
#include "tables.h"   /* k_rope, k_tok: tools/pack.py writes them with the blob */
#define HASH_SIZE 1024
#define BOS 1
#define MASK_CH '_'

/* ---- the model ---------------------------------------------------------- */
static int32_t g_blob = -1;
static uint32_t g_dim, g_hid, g_heads, g_kvh, g_vocab, g_layers, g_model_len, g_blob_len;
static uint32_t g_off_tok, g_off_tok_s;
struct layer_off { uint32_t f[9], w[7]; };
static struct layer_off g_lo[MAX_LAYERS];
#define g_rope k_rope
static float g_tok_s[MAX_VOCAB];
static float g_rms_att[MAX_LAYERS][MAX_DIM], g_rms_ffn[MAX_LAYERS][MAX_DIM], g_rms_final[MAX_DIM];
static uint32_t g_weight_bytes_step;   /* matrix bytes one step reads */
static const char* g_load_error = "";

/* ---- the tokenizer ------------------------------------------------------ */
#define g_tok_raw k_tok
static uint32_t g_tok_n, g_tok_loaded, g_off[MAX_VOCAB];
static uint8_t g_len[MAX_VOCAB];
static float g_score[MAX_VOCAB];
static uint16_t g_hash[HASH_SIZE];

/* ---- activations --------------------------------------------------------- */
static float g_x[P * MAX_DIM], g_q[P * MAX_DIM];
static float g_work[WORK_FLOATS];
static float g_xb[MAX_DIM], g_xb2[MAX_DIM], g_att[P], g_xs[P], g_xsh[FFN_CHUNK], g_ks[P * 8], g_vs[P * 8];
static int8_t g_xq[P * MAX_DIM], g_xqh[FFN_CHUNK * MAX_HID], g_kq[P * MAX_DIM], g_vq[P * MAX_DIM];
static uint32_t g_cand[P];
static float g_conf[P];
static uint32_t g_rng;

/* ---- the generation ------------------------------------------------------ */
enum { IDLE, RUNNING, DONE };
static uint32_t g_state, g_run, g_steps, g_step, g_phase, g_n_reveal, g_seed, g_masked0, g_prompt_len;
static float g_temp;
static uint32_t g_win[P];
static int64_t g_t_start;
static uint64_t g_wall_us, g_compute_us, g_rev_total;
static uint32_t g_log_n;
static uint16_t g_log_tok[MAX_STEPS][P];
static uint32_t g_log_us[MAX_STEPS], g_log_mm_us[MAX_STEPS];
static uint8_t g_log_rev[MAX_STEPS];
static uint32_t g_step_us_acc;
static uint64_t g_mm_base;

static unsigned char g_req[1024];
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_req ? (uint32_t)(uintptr_t)g_req : 0;
}

/* ---- output -------------------------------------------------------------- */
static char g_out[512];
static uint32_t g_out_n;
static NOINLINE void flush(void) {
    if (g_out_n) k32_resp_write(g_out, g_out_n);
    g_out_n = 0;
}
static NOINLINE void put_c(char c) {
    if (g_out_n == sizeof g_out) flush();
    g_out[g_out_n++] = c;
}
static NOINLINE void put(const char* s) { while (*s) put_c(*s++); }
static NOINLINE void put_u(uint64_t v) {
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k) put_c(d[--k]);
}
/* a JSON string body */
static NOINLINE void put_json(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { put_c('\\'); put_c((char)c); }
        else if (c == '\n') put("\\n");
        else if (c >= 0x20) put_c((char)c);
    }
}

/* ---- blob layout ---------------------------------------------------------- */
static NOINLINE int blob_get(uint32_t off, void* dst, uint32_t n) { return k32_blob_read(g_blob, off, dst, n) == (int32_t)n; }

static NOINLINE int load_model(void) {
    uint32_t h[16];
    if (!blob_get(0, h, 64)) { g_load_error = "short blob"; return -1; }
    const uint32_t want = F_EMBED | F_HEAD | F_TIED | F_BIDIR;
    if (h[0] != MAGIC || h[1] != 1 || (h[11] & want) != want || h[13] != 8 || h[8] != 0 || h[9] != h[10]) {
        g_load_error = "not a masked diffusion model";
        return -1;
    }
    g_dim = h[2]; g_hid = h[3]; g_heads = h[4]; g_kvh = h[5]; g_vocab = h[6]; g_layers = h[10]; g_model_len = h[12];
    if (g_dim > MAX_DIM || g_hid > MAX_HID || g_layers > MAX_LAYERS || g_vocab > MAX_VOCAB || h[7] != P || !g_heads || !g_kvh ||
        g_heads % g_kvh || g_dim % g_heads || g_dim / g_heads > MAX_DIM / 4 || g_kvh > 8) {
        g_load_error = "the model does not fit this pod's buffers";
        return -1;
    }
    const uint32_t dim = g_dim, hid = g_hid, hs = dim / g_heads, kvd = g_kvh * hs;
    uint32_t c = 64;
    g_off_tok = c; c += g_vocab * dim;
    g_off_tok_s = c; c += 4 * g_vocab;
    if (4 * P * hs != sizeof k_rope) { g_load_error = "rotary table"; return -1; }
    for (uint32_t l = 0; l < g_layers; l++) {
        const uint32_t fn[] = {dim, dim, kvd, kvd, dim, dim, hid, dim, hid};
        for (int i = 0; i < 9; i++) { g_lo[l].f[i] = c; c += 4 * fn[i]; }
        const uint32_t mn[] = {dim * dim, kvd * dim, kvd * dim, dim * dim, hid * dim, dim * hid, hid * dim};
        for (int i = 0; i < 7; i++) { g_lo[l].w[i] = c; c += mn[i]; }
    }
    const uint32_t off_final = c; c += 4 * dim;
    if (c != g_blob_len) { g_load_error = "blob size"; return -1; }
    if (!blob_get(g_off_tok_s, g_tok_s, 4 * g_vocab) || !blob_get(off_final, g_rms_final, 4 * dim)) {
        g_load_error = "blob read";
        return -1;
    }
    for (uint32_t l = 0; l < g_layers; l++)
        if (!blob_get(g_lo[l].f[0], g_rms_att[l], 4 * dim) || !blob_get(g_lo[l].f[5], g_rms_ffn[l], 4 * dim)) {
            g_load_error = "blob read";
            return -1;
        }
    g_weight_bytes_step = g_layers * (2 * dim * dim + 2 * kvd * dim + 3 * hid * dim) + (g_vocab - 1) * dim;
    return 0;
}

/* ---- tokenizer (as apps/llm-pipeline's coordinator) ------------------------ */
static NOINLINE uint32_t hash(const uint8_t* s, uint32_t n) {
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++) { h ^= s[i]; h *= 0x01000193u; }
    return h;
}

static NOINLINE int32_t lookup(const uint8_t* s, uint32_t n) {
    for (uint32_t h = hash(s, n) & (HASH_SIZE - 1);; h = (h + 1) & (HASH_SIZE - 1)) {
        const uint16_t id = g_hash[h];
        if (id == 0xffff) return -1;
        if (g_len[id] == n) {
            const uint8_t* p = g_tok_raw + g_off[id];
            uint32_t i = 0;
            while (i < n && p[i] == s[i]) i++;
            if (i == n) return id;
        }
    }
}

static NOINLINE int load_tokenizer(void) {
    const uint32_t n = sizeof k_tok;
    if (n < 12) return -1;
    for (uint32_t i = 0; i < HASH_SIZE; i++) g_hash[i] = 0xffff;
    uint32_t p = 4, v = 0;
    while (p + 8 <= n && v < MAX_VOCAB) {
        uint32_t len;
        __builtin_memcpy(&g_score[v], g_tok_raw + p, 4);
        __builtin_memcpy(&len, g_tok_raw + p + 4, 4);
        p += 8;
        if (len > 255 || p + len > n) break;
        g_off[v] = p;
        g_len[v] = (uint8_t)len;
        uint32_t h = hash(g_tok_raw + p, len) & (HASH_SIZE - 1);
        while (g_hash[h] != 0xffff) h = (h + 1) & (HASH_SIZE - 1);
        g_hash[h] = (uint16_t)v;
        p += len;
        v++;
    }
    if (p != n || v + 1 != g_vocab) return -1;   /* the model's vocabulary is the tokenizer's and MASK */
    g_tok_n = v;
    g_tok_loaded = 1;
    return 0;
}

static NOINLINE uint32_t encode(const char* text, uint32_t n, uint32_t* toks, uint32_t cap) {
    uint32_t nt = 0;
    toks[nt++] = BOS;
    if (n) {
        const int32_t sp = lookup((const uint8_t*)" ", 1);
        if (sp >= 0) toks[nt++] = (uint32_t)sp;
    }
    for (uint32_t i = 0; i < n && nt < cap;) {
        uint32_t cl = 1;
        while (i + cl < n && cl < 4 && ((uint8_t)text[i + cl] & 0xc0) == 0x80) cl++;
        const int32_t id = lookup((const uint8_t*)text + i, cl);
        if (id >= 0) toks[nt++] = (uint32_t)id;
        else for (uint32_t b = 0; b < cl && nt < cap; b++) toks[nt++] = (uint32_t)((uint8_t)text[i + b] + 3);
        i += cl;
    }
    uint8_t pair[512];
    for (;;) {
        float best = -1e10f;
        int32_t best_id = -1, best_at = -1;
        for (uint32_t i = 0; i + 1 < nt; i++) {
            const uint32_t a = g_len[toks[i]], b = g_len[toks[i + 1]];
            if (a + b > sizeof pair) continue;
            __builtin_memcpy(pair, g_tok_raw + g_off[toks[i]], a);
            __builtin_memcpy(pair + a, g_tok_raw + g_off[toks[i + 1]], b);
            const int32_t id = lookup(pair, a + b);
            if (id >= 0 && g_score[id] > best) { best = g_score[id]; best_id = id; best_at = (int32_t)i; }
        }
        if (best_at < 0) break;
        toks[best_at] = (uint32_t)best_id;
        for (uint32_t i = (uint32_t)best_at + 1; i + 1 < nt; i++) toks[i] = toks[i + 1];
        nt--;
    }
    return nt;
}

/* the text of `tok` after `prev`: no space right after BOS, <0xHH> as its byte,
 * unprintable bytes left out */
static NOINLINE uint32_t piece(uint32_t prev, uint32_t tok, const uint8_t** out, uint8_t* one) {
    const uint8_t* p = g_tok_raw + g_off[tok];
    uint32_t n = g_len[tok];
    if (prev == BOS && n && p[0] == ' ') { p++; n--; }
    if (n == 6 && p[0] == '<' && p[1] == '0' && p[2] == 'x' && p[5] == '>') {
        uint32_t b = 0;
        for (int i = 3; i < 5; i++) b = b * 16 + (uint32_t)(p[i] <= '9' ? p[i] - '0' : (p[i] | 0x20) - 'a' + 10);
        if (b < 0x20 && b != '\n') return 0;
        *one = (uint8_t)b;
        *out = one;
        return 1;
    }
    *out = p;
    return n;
}

/* the window's text up to the first BOS after position 0, a masked position as MASK_CH */
static NOINLINE void put_text(const uint16_t* w) {
    uint8_t one;
    for (uint32_t i = 1; i < P; i++) {
        if (w[i] == BOS) break;
        if (w[i] >= g_tok_n) { put_c(MASK_CH); continue; }
        const uint8_t* s;
        const uint32_t n = piece(w[i - 1], w[i], &s, &one);
        put_json((const char*)s, n);
    }
}

/* ---- arithmetic, as mdlm_infer.c's ------------------------------------------ */
static NOINLINE void norm(float* out, const float* x, const float* w, uint32_t n) {
    float ss = 0.0f;
#pragma clang loop unroll_count(4)
    for (uint32_t i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (float)n;
    ss = ss + 1e-5f;
    ss = 1.0f / __builtin_sqrtf(ss);
#pragma clang loop unroll_count(4)
    for (uint32_t i = 0; i < n; i++) out[i] = w[i] * (ss * x[i]);
}

/* The wasm interpreter runs a float operation several times slower than an integer one, so
 * the largest magnitude is found on the bit patterns (for non-negative floats the order of
 * the bits is the order of the values) and the rounding offset is a copysign. */
static NOINLINE float quantize(int8_t* q, const float* src, uint32_t n) {
    uint32_t m = 0;
#pragma clang loop unroll_count(4)
    for (uint32_t i = 0; i < n; i++) {
        uint32_t u;
        __builtin_memcpy(&u, &src[i], 4);
        u &= 0x7fffffffu;
        if (u > m) m = u;
    }
    float amax;
    __builtin_memcpy(&amax, &m, 4);
    const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
#pragma clang loop unroll_count(4)
    for (uint32_t i = 0; i < n; i++) {
        const float t = src[i] * inv;
        int32_t r = (int32_t)(t + __builtin_copysignf(0.5f, t));
        q[i] = (int8_t)(r > 127 ? 127 : r < -127 ? -127 : r);
    }
    return amax / 127.0f;
}

static NOINLINE void rope(float* a, uint32_t n, const float* cs, uint32_t hs) {
    for (uint32_t h = 0; h < n; h += hs) {
        float* v = a + h;
        for (uint32_t i = 0; i < hs; i += 2) {
            const float c = cs[i], s = cs[i + 1];
            const float a0 = v[i], a1 = v[i + 1];
            v[i] = a0 * c - a1 * s;
            v[i + 1] = a0 * s + a1 * c;
        }
    }
}

/* k32_expf's bits, with the range test on the bit pattern: |x| <= 87 takes the polynomial, other values k32_expf */
static inline float expf_fast(float x) {
    uint32_t xb;
    __builtin_memcpy(&xb, &x, 4);
    if ((xb & 0x7fffffffu) > 0x42ae0000u) return k32_expf(x);
    const float t = x * 1.44269504f;
    const int32_t n = (int32_t)(t + __builtin_copysignf(0.5f, t));
    float r = x - (float)n * 0.693145752f;
    r = r - (float)n * 1.42860677e-6f;
    float p = 1.38888889e-3f;
    p = p * r + 8.33333333e-3f;
    p = p * r + 4.16666667e-2f;
    p = p * r + 1.66666667e-1f;
    p = p * r + 0.5f;
    p = p * r + 1.0f;
    p = p * r + 1.0f;
    const uint32_t bits = (uint32_t)(n + 127) << 23;
    float scale;
    __builtin_memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

static NOINLINE float uniform(void) {
    uint32_t s = g_rng;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    g_rng = s;
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}

/* where the time goes: the section the pod is in, and the microseconds each took */
enum { S_OTHER, S_MM, S_PRE, S_ATT, S_ADD, S_FFN, S_CLS, S_EMB, S_COUNT };
static uint32_t g_cur;
static int64_t g_lap;
static uint64_t g_part[S_COUNT];
static NOINLINE void sec(uint32_t i) {
    const int64_t t = esp32_get_time_us();
    g_part[g_cur] += (uint64_t)(t - g_lap);
    g_lap = t;
    g_cur = i;
}

/* out[p * rows + r] for the n vectors at x, the matrix and its scales read from the blob in place */
static NOINLINE int32_t mm(float* out, uint32_t w_off, uint32_t s_off, const int8_t* x, const float* xs, uint32_t rows, uint32_t cols,
                           uint32_t n) {
    const uint32_t prev = g_cur;
    sec(S_MM);
    const int32_t rc = k32_matmul_i8(out, (const int8_t*)K32_BLOB_ADDR(g_blob, w_off), (const float*)K32_BLOB_ADDR(g_blob, s_off), x, xs,
                                     rows, cols, 0, n);
    sec(prev);
    return rc;
}

static NOINLINE void fail_run(const char* why) {
    uint32_t n = 0;
    while (why[n]) n++;
    k32_log(why, n);
    g_load_error = why;
    g_state = IDLE;
}

/* ---- one step, a layer at a time -------------------------------------------- */
static NOINLINE void embed(void) {
    const uint32_t dim = g_dim;
    int8_t row[MAX_DIM];
    sec(S_EMB);
    for (uint32_t p = 0; p < P; p++) {
        if (!blob_get(g_off_tok + g_win[p] * dim, row, dim)) { fail_run("llm-diffusion: embedding read"); return; }
        const float s = g_tok_s[g_win[p]];
        for (uint32_t i = 0; i < dim; i++) g_x[p * dim + i] = (float)row[i] * s;
    }
    sec(S_OTHER);
}

/* A layer in LAYER_PARTS parts, one a k32_tick call: 0 the norm, q, k and v and the key and value
 * caches, 1 the attention, 2 the output projection and the feed-forward norm, then the feed-forward
 * block FFN_CHUNK positions a part. The parts keep their state in the buffers. */
#define LAYER_PARTS (3 + P / FFN_CHUNK)

static NOINLINE void layer(uint32_t l, uint32_t part) {
    const uint32_t dim = g_dim, hid = g_hid, H = g_heads, KVH = g_kvh, hs = dim / H, kvd = KVH * hs;
    const float scale = 1.0f / __builtin_sqrtf((float)hs);
    const struct layer_off* L = &g_lo[l];
    if (part == 0) {
        sec(S_PRE);
        for (uint32_t p = 0; p < P; p++) {
            norm(g_xb, g_x + p * dim, g_rms_att[l], dim);
            g_xs[p] = quantize(g_xq + p * dim, g_xb, dim);
        }
        mm(g_q, L->w[0], L->f[1], g_xq, g_xs, dim, dim, P);
        mm(g_work, L->w[1], L->f[2], g_xq, g_xs, kvd, dim, P);
        for (uint32_t p = 0; p < P; p++) {
            rope(g_q + p * dim, dim, g_rope + p * hs, hs);
            rope(g_work + p * kvd, kvd, g_rope + p * hs, hs);
            for (uint32_t h = 0; h < KVH; h++) g_ks[p * KVH + h] = quantize(g_kq + p * kvd + h * hs, g_work + p * kvd + h * hs, hs);
        }
        mm(g_work, L->w[2], L->f[3], g_xq, g_xs, kvd, dim, P);
        for (uint32_t p = 0; p < P; p++)
            for (uint32_t h = 0; h < KVH; h++) g_vs[p * KVH + h] = quantize(g_vq + p * kvd + h * hs, g_work + p * kvd + h * hs, hs);
    } else if (part == 1) {
        sec(S_ATT);
        for (uint32_t p = 0; p < P; p++) {
            for (uint32_t h = 0; h < H; h++) {
                const uint32_t kh = h / (H / KVH);
                k32_matvec_f32_i8(g_att, &g_kq[kh * hs], &g_ks[kh], g_q + p * dim + h * hs, scale, P, hs, kvd, KVH * 4);
                k32_softmax_f32(g_att, P);
                k32_vecmat_f32_i8(g_xb2 + h * hs, &g_vq[kh * hs], &g_vs[kh], g_att, P, hs, kvd, KVH * 4);
            }
            g_xs[p] = quantize(g_xq + p * dim, g_xb2, dim);
        }
    } else if (part == 2) {
        sec(S_ATT);
        mm(g_work, L->w[3], L->f[4], g_xq, g_xs, dim, dim, P);
        sec(S_ADD);
        for (uint32_t i = 0; i < P * dim; i++) g_x[i] += g_work[i];
        sec(S_PRE);
        for (uint32_t p = 0; p < P; p++) {
            norm(g_xb, g_x + p * dim, g_rms_ffn[l], dim);
            g_xs[p] = quantize(g_xq + p * dim, g_xb, dim);
        }
    } else {
        const uint32_t c0 = (part - 3) * FFN_CHUNK;
        float* h1 = g_work;
        float* h3 = g_work + FFN_CHUNK * hid;
        mm(h1, L->w[4], L->f[6], g_xq + c0 * dim, g_xs + c0, hid, dim, FFN_CHUNK);
        mm(h3, L->w[6], L->f[8], g_xq + c0 * dim, g_xs + c0, hid, dim, FFN_CHUNK);
        sec(S_FFN);
        for (uint32_t j = 0; j < FFN_CHUNK; j++) {
            float* a1 = h1 + j * hid;
            const float* a3 = h3 + j * hid;
            for (uint32_t i = 0; i < hid; i++) {
                float a = a1[i];
                a = a * (1.0f / (1.0f + expf_fast(-a)));
                a1[i] = a * a3[i];
            }
            g_xsh[j] = quantize(g_xqh + j * hid, a1, hid);
        }
        mm(g_work, L->w[5], L->f[7], g_xqh, g_xsh, dim, hid, FFN_CHUNK);
        sec(S_ADD);
        for (uint32_t i = 0; i < FFN_CHUNK * dim; i++) g_x[c0 * dim + i] += g_work[i];
    }
    sec(S_OTHER);
}

/* the candidates of the masked positions, then the most confident are revealed */
static NOINLINE void classify(void) {
    const uint32_t dim = g_dim, V = g_vocab - 1, MASK = g_vocab - 1;
    sec(S_CLS);
    const float inv_t = g_temp > 0.0f ? 1.0f / g_temp : 0.0f;
    uint32_t idx[CLS_CHUNK];
    uint32_t p = 0;
    for (;;) {
        uint32_t m = 0;
        while (p < P && m < CLS_CHUNK) {
            if (g_win[p] == MASK) idx[m++] = p;
            p++;
        }
        if (!m) break;
        for (uint32_t j = 0; j < m; j++) {
            norm(g_xb, g_x + idx[j] * dim, g_rms_final, dim);
            g_xs[j] = quantize(g_xq + j * dim, g_xb, dim);
        }
        mm(g_work, g_off_tok, g_off_tok_s, g_xq, g_xs, V, dim, m);
        for (uint32_t j = 0; j < m; j++) {
            float* lg = g_work + j * V;
            uint32_t c = 0;
            if (g_temp > 0.0f) {
                for (uint32_t i = 0; i < V; i++) lg[i] = lg[i] * inv_t;
                k32_softmax_f32(lg, V);
                const float u = uniform();
                float run = 0.0f;
                uint32_t i = 0, last = 0;
                for (; i < V; i++) {
                    if (lg[i] > 0.0f) last = i;
                    run = run + lg[i];
                    if (run > u) break;
                }
                c = i < V ? i : last;
            } else {
                for (uint32_t i = 1; i < V; i++)
                    if (lg[i] > lg[c]) c = i;
                k32_softmax_f32(lg, V);
            }
            g_cand[idx[j]] = c;
            g_conf[idx[j]] = lg[c];
        }
    }
    for (uint32_t k = 0; k < g_n_reveal; k++) {
        int32_t best = -1;
        for (uint32_t q = 0; q < P; q++)
            if (g_win[q] == MASK && (best < 0 || g_conf[q] > g_conf[best])) best = (int32_t)q;
        if (best < 0) break;
        g_win[best] = g_cand[best];
    }
    sec(S_OTHER);
}

static NOINLINE uint32_t count_masked(void) {
    uint32_t m = 0;
    for (uint32_t p = 0; p < P; p++) m += g_win[p] == g_vocab - 1;
    return m;
}

static NOINLINE void log_step(uint32_t revealed, uint32_t us) {
    if (g_step < MAX_STEPS) {
        for (uint32_t p = 0; p < P; p++) g_log_tok[g_step][p] = (uint16_t)g_win[p];
        g_log_us[g_step] = us;
        g_log_mm_us[g_step] = (uint32_t)(g_part[S_MM] - g_mm_base);
        g_log_rev[g_step] = (uint8_t)revealed;
        g_log_n = g_step + 1;
    }
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (g_state != RUNNING) return 0;
    const int64_t t0 = esp32_get_time_us();
    g_lap = t0;
    g_cur = S_OTHER;
    if (g_phase == 0) {
        g_mm_base = g_part[S_MM];
        g_step_us_acc = 0;
        const uint32_t M = g_masked0;
        g_n_reveal = M * (g_step + 1) / g_steps - M * g_step / g_steps;
        if (!count_masked() || !g_n_reveal) {   /* nothing to reveal this step */
            log_step(0, 0);
            g_step++;
            if (g_step >= g_steps) goto finished;
            return 1;
        }
        embed();
        if (g_state != RUNNING) return 0;
        g_phase = 1;
    } else if (g_phase <= g_layers * LAYER_PARTS) {
        layer((g_phase - 1) / LAYER_PARTS, (g_phase - 1) % LAYER_PARTS);
        g_phase++;
    } else {
        classify();
        const uint32_t us = g_step_us_acc + (uint32_t)(esp32_get_time_us() - t0);
        g_compute_us += us;
        g_rev_total += g_n_reveal;
        log_step(g_n_reveal, us);
        g_step++;
        g_phase = 0;
        if (g_step >= g_steps) goto finished;
        return 1;
    }
    g_step_us_acc += (uint32_t)(esp32_get_time_us() - t0);
    return 1;
finished:
    g_wall_us = (uint64_t)(esp32_get_time_us() - g_t_start);
    g_state = DONE;
    return 0;
}

/* ---- HTTP -------------------------------------------------------------------- */
static NOINLINE uint32_t query_uint(const char* q, uint32_t len, const char* key, uint32_t dflt) {
    uint32_t klen = 0;
    while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen < len; i++) {
        if ((i == 0 || q[i - 1] == '&') && q[i + klen] == '=') {
            uint32_t j = 0;
            while (j < klen && q[i + j] == key[j]) j++;
            if (j < klen) continue;
            uint32_t v = 0;
            for (uint32_t e = i + klen + 1; e < len && q[e] >= '0' && q[e] <= '9'; e++) v = v * 10u + (uint32_t)(q[e] - '0');
            return v;
        }
    }
    return dflt;
}

static NOINLINE int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1; }

/* the URL-decoded value of `key` in the query, into dst (at most cap bytes) */
static NOINLINE uint32_t query_text(const char* q, uint32_t len, const char* key, char* dst, uint32_t cap) {
    uint32_t klen = 0, n = 0;
    while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen < len; i++) {
        if ((i == 0 || q[i - 1] == '&') && q[i + klen] == '=') {
            uint32_t j = 0;
            while (j < klen && q[i + j] == key[j]) j++;
            if (j < klen) continue;
            for (uint32_t e = i + klen + 1; e < len && q[e] != '&' && n < cap; e++) {
                char c = q[e];
                if (c == '+') c = ' ';
                else if (c == '%' && e + 2 < len && hexv(q[e + 1]) >= 0 && hexv(q[e + 2]) >= 0) {
                    c = (char)(hexv(q[e + 1]) * 16 + hexv(q[e + 2]));
                    e += 2;
                }
                dst[n++] = c;
            }
            return n;
        }
    }
    return 0;
}

static NOINLINE void reply_begin(int32_t status) { k32_resp_begin(status, "application/json", 16); g_out_n = 0; }
static NOINLINE void reply_end(void) { flush(); k32_resp_end(); }
static NOINLINE void reply_error(int32_t status, const char* why) {
    reply_begin(status);
    put("{\"error\":\""); put(why); put("\"}");
    reply_end();
}

static NOINLINE void generate(const char* q, uint32_t qlen) {
    if (g_state == RUNNING) { reply_error(409, "a generation is running"); return; }
    if (g_vocab == 0 || !g_tok_loaded) { reply_error(503, g_load_error[0] ? g_load_error : "not loaded"); return; }
    char text[160];
    const uint32_t tn = query_text(q, qlen, "prompt", text, sizeof text);
    uint32_t steps = query_uint(q, qlen, "steps", 16);
    if (steps < 1 || steps > MAX_STEPS) { reply_error(400, "steps: 1 to 64"); return; }
    uint32_t prompt[P];
    const uint32_t pn = encode(text, tn, prompt, P);
    if (pn >= P) { reply_error(400, "the prompt fills the window"); return; }
    const uint32_t MASK = g_vocab - 1;
    for (uint32_t p = 0; p < P; p++) g_win[p] = p < pn ? prompt[p] : MASK;
    g_prompt_len = pn;
    g_masked0 = P - pn;
    g_steps = steps;
    g_step = 0;
    g_phase = 0;
    g_log_n = 0;
    g_temp = (float)query_uint(q, qlen, "temp", 0) / 100.0f;
    g_seed = query_uint(q, qlen, "seed", 1);
    g_rng = g_seed ? g_seed : 0x6D2B79F5u;
    g_compute_us = g_rev_total = g_wall_us = 0;
    for (uint32_t i = 0; i < S_COUNT; i++) g_part[i] = 0;
    g_run++;
    g_t_start = esp32_get_time_us();
    g_state = RUNNING;
    reply_begin(200);
    put("{\"run\":"); put_u(g_run); put(",\"state\":\"running\",\"steps\":"); put_u(steps);
    put(",\"prompt_tokens\":"); put_u(pn); put(",\"masked\":"); put_u(g_masked0); put("}");
    reply_end();
}

static NOINLINE void state(const char* q, uint32_t qlen) {
    const uint32_t from = query_uint(q, qlen, "from", 0);
    static const char* const names[] = {"idle", "running", "done"};
    reply_begin(200);
    put("{\"run\":"); put_u(g_run); put(",\"state\":\""); put(names[g_state]);
    put("\",\"steps\":"); put_u(g_steps); put(",\"step\":"); put_u(g_step);
    put(",\"prompt_tokens\":"); put_u(g_prompt_len);
    put(",\"log\":[");
    for (uint32_t i = from; i < g_log_n; i++) {
        if (i > from) put_c(',');
        put("{\"step\":"); put_u(i); put(",\"revealed\":"); put_u(g_log_rev[i]);
        put(",\"us\":"); put_u(g_log_us[i]); put(",\"matmul_us\":"); put_u(g_log_mm_us[i]);
        put(",\"text\":\""); put_text(g_log_tok[i]); put("\"}");
    }
    put_c(']');
    if (g_state == DONE) {
        const uint32_t ran = g_steps;
        uint32_t forwards = 0;
        for (uint32_t i = 0; i < g_log_n; i++) forwards += g_log_rev[i] != 0;
        put(",\"tokens\":[");
        for (uint32_t p = 0; p < P; p++) { if (p) put_c(','); put_u(g_win[p]); }
        put("],\"generated\":"); put_u(g_rev_total);
        put(",\"forward_passes\":"); put_u(forwards);
        put(",\"compute_us\":"); put_u(g_compute_us);
        put(",\"matmul_us\":"); put_u(g_part[S_MM]);
        put(",\"parts_us\":{\"embed\":"); put_u(g_part[S_EMB]); put(",\"norm_quant_rope\":"); put_u(g_part[S_PRE]);
        put(",\"attention\":"); put_u(g_part[S_ATT]); put(",\"residual\":"); put_u(g_part[S_ADD]);
        put(",\"silu_quant\":"); put_u(g_part[S_FFN]); put(",\"classifier_rest\":"); put_u(g_part[S_CLS]); put_c('}');
        put(",\"wall_us\":"); put_u(g_wall_us);
        put(",\"us_per_step\":"); put_u(forwards ? g_compute_us / forwards : 0);
        put(",\"tokens_per_s_x100\":"); put_u(g_compute_us ? g_rev_total * 100000000ull / g_compute_us : 0);
        put(",\"weight_bytes_read\":"); put_u((uint64_t)forwards * g_weight_bytes_step);
        (void)ran;
    }
    put_c('}');
    reply_end();
}

/* GET /bench?n=<vectors>&reps=<r>: the first feed-forward matrix (hidden x dim, read from the blob)
 * applied to n vectors, r times, with no generation running */
static NOINLINE void bench(const char* q, uint32_t qlen) {
    if (g_state == RUNNING || !g_vocab) { reply_error(409, "busy or not loaded"); return; }
    uint32_t n = query_uint(q, qlen, "n", 1), reps = query_uint(q, qlen, "reps", 1);
    if (n < 1) n = 1;
    if (n > FFN_CHUNK) n = FFN_CHUNK;
    const uint32_t mode = query_uint(q, qlen, "mode", 0);
    if (mode) {   /* interpreter loops over n * hidden values */
        const uint32_t cnt = n * g_hid;
        for (uint32_t i = 0; i < cnt; i++) g_work[i] = (float)((int32_t)(i * 2654435761u) >> 20) * 0.001f;
        volatile uint32_t sink = 0;
        const int64_t t1 = esp32_get_time_us();
        for (uint32_t k = 0; k < reps; k++) {
            if (mode == 1) {   /* the SiLU gate */
                for (uint32_t i = 0; i < cnt; i++) {
                    float a = g_work[i];
                    a = a * (1.0f / (1.0f + expf_fast(-a)));
                    g_work[i] = a;
                }
            } else if (mode == 2) {   /* integer loop */
                uint32_t s = 0;
                for (uint32_t i = 0; i < cnt; i++) s += i ^ (s >> 3);
                sink = s;
            } else if (mode == 3) {   /* float multiply-add loop */
                float s = 0.0f;
                for (uint32_t i = 0; i < cnt; i++) s += g_work[i] * g_work[i];
                sink = (uint32_t)s;
            } else if (mode == 5) {   /* integer loop over the values' bits */
                const uint32_t* u = (const uint32_t*)g_work;
                uint32_t s = 0;
                for (uint32_t i = 0; i < cnt; i++) s += u[i] ^ (s >> 3);
                sink = s;
            } else if (mode == 6) {   /* float loop with no memory access */
                float s = 0.0f;
                for (uint32_t i = 0; i < cnt; i++) s += (float)(i & 255u) * 0.5f;
                sink = (uint32_t)s;
            } else {   /* quantize rows of hidden values */
                for (uint32_t j = 0; j < n; j++) g_xsh[j] = quantize(g_xqh + j * g_hid, g_work + j * g_hid, g_hid);
            }
        }
        const uint64_t us1 = (uint64_t)(esp32_get_time_us() - t1);
        (void)sink;
        reply_begin(200);
        put("{\"mode\":"); put_u(mode); put(",\"values\":"); put_u((uint64_t)cnt * reps); put(",\"us\":"); put_u(us1); put("}");
        reply_end();
        return;
    }
    for (uint32_t i = 0; i < n * g_dim; i++) g_xq[i] = (int8_t)(i * 37u + 11u);
    for (uint32_t i = 0; i < n; i++) g_xs[i] = 0.01f;
    const int64_t t0 = esp32_get_time_us();
    for (uint32_t k = 0; k < reps; k++)
        if (k32_matmul_i8(g_work, (const int8_t*)K32_BLOB_ADDR(g_blob, g_lo[0].w[4]), (const float*)K32_BLOB_ADDR(g_blob, g_lo[0].f[6]),
                          g_xq, g_xs, g_hid, g_dim, 0, n) != K32_OK) { reply_error(500, "k32_matmul_i8"); return; }
    const uint64_t us = (uint64_t)(esp32_get_time_us() - t0);
    reply_begin(200);
    put("{\"n\":"); put_u(n); put(",\"reps\":"); put_u(reps); put(",\"weight_bytes\":"); put_u((uint64_t)g_hid * g_dim * reps);
    put(",\"macs\":"); put_u((uint64_t)g_hid * g_dim * n * reps); put(",\"us\":"); put_u(us); put("}");
    reply_end();
}

static NOINLINE int ends(const char* s, uint32_t n, const char* tail) {
    uint32_t t = 0;
    while (tail[t]) t++;
    if (n < t) return 0;
    for (uint32_t i = 0; i < t; i++) if (s[n - t + i] != tail[i]) return 0;
    return 1;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    uint32_t i = 0;
    while (i < len && r[i] != ' ') i++;
    const char* path = r + i + 1;
    uint32_t plen = 0;
    while (i + 1 + plen < len && path[plen] != ' ' && path[plen] != '\n') plen++;
    const char* q = path + plen;
    uint32_t qlen = 0, pathn = plen;
    for (uint32_t k = 0; k < plen; k++) {
        if (path[k] == '?') { q = path + k + 1; qlen = plen - k - 1; pathn = k; break; }
    }
    if (ends(path, pathn, "/bench")) bench(q, qlen);
    else if (ends(path, pathn, "/generate")) generate(q, qlen);
    else if (ends(path, pathn, "/state")) state(q, qlen);
    else {
        reply_begin(200);
        put("{\"model\":\"mdlm\",\"dim\":"); put_u(g_dim); put(",\"hidden\":"); put_u(g_hid); put(",\"layers\":"); put_u(g_layers);
        put(",\"window\":"); put_u(P); put(",\"vocab\":"); put_u(g_vocab);
        put(",\"blob_bytes\":"); put_u(g_blob_len); put(",\"weight_bytes_per_step\":"); put_u(g_weight_bytes_step);
        put(",\"loaded\":"); put(g_tok_loaded ? "true" : "false");
        put(",\"error\":\""); put(g_load_error); put("\"}");
        reply_end();
    }
    return 200;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char name[K32_BLOB_NAME_MAX + 1];
    int32_t n = k32_env_get("BLOB", 4, name, K32_BLOB_NAME_MAX);
    if (n <= 0 || n > K32_BLOB_NAME_MAX) {
        name[0] = 'm'; name[1] = 'd'; name[2] = 'l'; name[3] = 'm';
        n = 4;
    }
    g_blob = k32_blob_open(name, (uint32_t)n);
    if (g_blob < 0) { g_load_error = "blob not found"; k32_log("llm-diffusion: blob not found", 29); return 0; }
    g_blob_len = (uint32_t)k32_blob_size(g_blob);
    if (k32_blob_mapped(g_blob) != 1) { g_load_error = "blob not mapped"; k32_log("llm-diffusion: blob not mapped", 30); return 0; }
    if (load_model() != 0 || load_tokenizer() != 0) {
        if (!g_load_error[0]) g_load_error = "tokenizer";
        k32_log("llm-diffusion: load failed", 26);
        return 0;
    }
    k32_log("llm-diffusion: ready", 20);
    return 0;
}
