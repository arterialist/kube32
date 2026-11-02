/* llm-shard: a run of transformer layers of llm-pipeline, as one pod.
 *
 * At start the shard fetches its slice of the model (MODEL_URL, a file of
 * llm-store) into its linear memory and sizes its KV cache from what the
 * memory limit leaves. Then it receives positions with POST /x: the first
 * stage (the slice with the embedding) gets a token, every other stage the
 * residual stream of the stage before. k32_handle only queues a position;
 * k32_tick computes the oldest one through the shard's layers and forwards
 * it to NEXT_URL, so no pod waits on another stage while it computes and
 * the pods of one node take turns on its interpreter. The last stage (the
 * slice with the classifier) picks the next token (greedy) and posts the
 * position to NEXT_URL, the coordinator.
 *
 * A stage takes the positions of one session in order. Position 0 of a new
 * session starts over. A position it cannot continue from (a gap, or a
 * session it never saw from position 0) gets 409, and the stage before
 * drops it: the coordinator replays the session.
 *
 * Environment:
 *   STAGE      this stage's index in the pipeline (0 = first)
 *   MODEL_URL  the slice (http://llm-store/shard-0.bin)
 *   NEXT_URL   the next stage (http://llm-shard-1/x), or for the last stage
 *              the coordinator (http://llm-coordinator/result)
 *   NODE_NAME  reported in each position's trace (fieldRef spec.nodeName)
 *   SCREEN     "1": show the text of the positions that pass through on the
 *              node's display (needs the DISPLAY capability): a title row
 *              with the speed, then the text, prompt in grey, wrapped at
 *              words; a full screen starts over at the top
 *
 * GET / answers {"stage":S,"context":N,"next":P}: the positions the cache
 * holds (0 until the slice is loaded) and the next one it takes.
 *
 * The code is kept small: on a board without PSRAM the pod pool holds its
 * compiled form next to the linear memory. */
#include "llm.h"

#define QUEUE 2
#define RETRY_MS 200
#define ACTIVE_MS 3000

extern unsigned char __heap_base;

static unsigned char g_block[768];            /* k32_alloc: one request */
static char g_url[128], g_next[128], g_line[96];
static uint32_t g_url_len, g_next_len, g_stage, g_n, g_screen, g_col, g_row;

/* The slice: per layer 9 float vectors, then 7 int8 or int4 matrices (llm.h).
 * Each tensor's size is rows x columns of {1, dim, kv_dim, hidden}; the
 * row scales of matrix t are tensor t - 8 (t - 7 after RMS_FFN). */
enum { RMS_ATT, WQ_S, WK_S, WV_S, WO_S, RMS_FFN, W1_S, W2_S, W3_S, WQ, WK, WV, WO, W1, W2, W3, TENSORS };
static const uint8_t kShape[TENSORS] = {0x10, 0x10, 0x20, 0x20, 0x10, 0x10, 0x30, 0x10, 0x30,
                                        0x11, 0x21, 0x21, 0x11, 0x31, 0x13, 0x31};
static const uint8_t* g_t[LLM_MAX_LAYERS][TENSORS];
static const struct llm_blob* B;
static uint32_t g_dims[4];
static uint32_t g_nl, g_dim, g_hidden, g_heads, g_kvh, g_hs, g_kvd, g_vocab, g_l, g_bits;
static const int8_t *g_tok_q, *g_cls_q;
static const float *g_tok_s, *g_rope, *g_rms_final, *g_cls_s;
static uint32_t g_loaded, g_load_at;

/* activations and the cache */
static float *x, *xb, *xb2, *q, *k, *v, *hb, *hb2, *att, g_xs;
static int8_t *xq, *g_kq, *g_vq;  /* [layer][pos][kv_dim] */
static float *g_ks, *g_vs;        /* [layer][pos][kv_heads] */
static uint32_t g_ctx;

/* positions: the queue, then the one on its way out */
static uint8_t* g_queue;
static uint32_t g_msg_bytes, g_qhead, g_qcount, g_out_len, g_out_pending, g_retry_at;
static uint32_t g_queued_at[QUEUE];
static uint32_t g_session, g_next_pos;          /* what POST /x takes next */
static uint32_t g_last_call_us, g_last_work;
static struct llm_trace g_trace;                /* this stage's part of each trace */

static uint32_t now_us(void) { return (uint32_t)esp32_get_time_us(); }
static uint8_t* out_msg(void) { return g_queue + QUEUE * g_msg_bytes; }

static void put(const char* s) { while (*s && g_n < sizeof g_line) g_line[g_n++] = *s++; }
__attribute__((noinline)) static void put_u(uint32_t v) {
    char t[10]; uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && g_n < sizeof g_line) g_line[g_n++] = t[--n];
}
/* "llm-shard: <what><value>" to the log */
__attribute__((noinline)) static void say(const char* what, int32_t value) {
    g_n = 0;
    put("llm-shard: "); put(what);
    if (value < 0) { put("-"); value = -value; }
    put_u((uint32_t)value);
    k32_log(g_line, g_n);
}

/* ---- arithmetic (llm.h) -------------------------------------------------- */
/* xb = x / rms(x) * w */
__attribute__((noinline)) static void norm(const float* w) {
    float ss = 0.0f;
    for (uint32_t i = 0; i < g_dim; i++) ss += x[i] * x[i];
    ss = ss / (float)g_dim;
    ss = ss + 1e-5f;
    ss = 1.0f / __builtin_sqrtf(ss);
    for (uint32_t i = 0; i < g_dim; i++) xb[i] = w[i] * (ss * x[i]);
}

/* q[0..n) = round(src * 127 / max|src|); returns the scale */
__attribute__((noinline)) static float quantize(int8_t* qd, const float* src, uint32_t n) {
    float amax = 0.0f;
    for (uint32_t i = 0; i < n; i++) {
        const float a = __builtin_fabsf(src[i]);
        if (a > amax) amax = a;
    }
    const float inv = amax == 0.0f ? 0.0f : 127.0f / amax;
    for (uint32_t i = 0; i < n; i++) {
        const float t = src[i] * inv;
        int32_t r = (int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
        qd[i] = (int8_t)(r > 127 ? 127 : r < -127 ? -127 : r);
    }
    return amax / 127.0f;
}

__attribute__((noinline)) static int32_t dot(const int8_t* a, const int8_t* b, uint32_t n) {
    int32_t s = 0;
    for (uint32_t i = 0; i < n; i += 4)
        s += a[i] * b[i] + a[i + 1] * b[i + 1] + a[i + 2] * b[i + 2] + a[i + 3] * b[i + 3];
    return s;
}

/* a: n int4 values, two a byte */
__attribute__((noinline)) static int32_t dot4(const int8_t* a, const int8_t* b, uint32_t n) {
    int32_t s = 0;
    for (uint32_t i = 0; i < n; i += 2) {
        const int32_t w = a[i >> 1];
        s += ((int32_t)((uint32_t)w << 28) >> 28) * b[i] + (w >> 4) * b[i + 1];
    }
    return s;
}

/* out = W_t xq of layer g_l, with W's row scales and g_xs */
__attribute__((noinline)) static void proj(float* out, uint32_t t) {
    const int8_t* w = (const int8_t*)g_t[g_l][t];
    const float* ws = (const float*)g_t[g_l][t - (t >= W1 ? 7 : 8)];
    const uint32_t rows = g_dims[kShape[t] >> 4], n = g_dims[kShape[t] & 15];
    for (uint32_t r = 0; r < rows; r++)
        out[r] = (float)(g_bits == 4 ? dot4(w + r * n / 2, xq, n) : dot(w + r * n, xq, n)) * (ws[r] * g_xs);
}

/* the rotary embedding of every head of a (n floats) for this position */
__attribute__((noinline)) static void rope(float* a, uint32_t n, const float* cs) {
    for (uint32_t i = 0; i < n; i += 2) {
        const float c = cs[i % g_hs], s = cs[i % g_hs + 1];
        const float a0 = a[i], a1 = a[i + 1];
        a[i] = a0 * c - a1 * s;
        a[i + 1] = a0 * s + a1 * c;
    }
}

/* The functions on the path of a compute stay short: wasm3 keeps a native
 * frame for each operation a function has run until it returns, and a
 * board without PSRAM gives a call about 22 KB of native stack. */

/* att[t] = q_h . k_t / sqrt(head_size) for t <= pos; returns the largest */
__attribute__((noinline)) static float scores(uint32_t h, uint32_t pos) {
    const float scale = 1.0f / __builtin_sqrtf((float)g_hs);
    const uint32_t kh = h / (g_heads / g_kvh), base = g_l * g_ctx;
    const float* qh = q + h * g_hs;
    float m = 0.0f;
    for (uint32_t t = 0; t <= pos; t++) {
        const int8_t* kt = g_kq + (base + t) * g_kvd + kh * g_hs;
        float d = 0.0f;
        for (uint32_t i = 0; i < g_hs; i++) d += qh[i] * (float)kt[i];
        att[t] = d * g_ks[(base + t) * g_kvh + kh] * scale;
        if (t == 0 || att[t] > m) m = att[t];
    }
    return m;
}

/* att[0..n) = exp(att - m) / their sum */
__attribute__((noinline)) static void softmax(uint32_t n, float m) {
    float sum = 0.0f;
    for (uint32_t t = 0; t < n; t++) {
        att[t] = llm_exp(att[t] - m);
        sum += att[t];
    }
    for (uint32_t t = 0; t < n; t++) att[t] = att[t] / sum;
}

/* head h of xb2 = sum over t <= pos of att[t] v_t */
__attribute__((noinline)) static void mix(uint32_t h, uint32_t pos) {
    const uint32_t kh = h / (g_heads / g_kvh), base = g_l * g_ctx;
    float* o = xb2 + h * g_hs;
    for (uint32_t i = 0; i < g_hs; i++) o[i] = 0.0f;
    for (uint32_t t = 0; t <= pos; t++) {
        const float a = att[t] * g_vs[(base + t) * g_kvh + kh];
        const int8_t* vt = g_vq + (base + t) * g_kvd + kh * g_hs;
        for (uint32_t i = 0; i < g_hs; i++) o[i] += a * (float)vt[i];
    }
}

__attribute__((noinline)) static void attention(uint32_t pos) {
    for (uint32_t h = 0; h < g_heads; h++) {
        softmax(pos + 1, scores(h, pos));
        mix(h, pos);
    }
}

static void residual(void) { for (uint32_t i = 0; i < g_dim; i++) x[i] += xb[i]; }

/* q, k, v of norm(x); k and v into the cache at pos */
__attribute__((noinline)) static void qkv(uint32_t pos, const float* cs) {
    norm((const float*)g_t[g_l][RMS_ATT]);
    g_xs = quantize(xq, xb, g_dim);
    proj(q, WQ);
    proj(k, WK);
    proj(v, WV);
    rope(q, g_dim, cs);
    rope(k, g_kvd, cs);
    const uint32_t row = g_l * g_ctx + pos;
    for (uint32_t h = 0; h < g_kvh; h++) {
        const uint32_t o = h * g_hs;
        g_ks[row * g_kvh + h] = quantize(g_kq + row * g_kvd + o, k + o, g_hs);
        g_vs[row * g_kvh + h] = quantize(g_vq + row * g_kvd + o, v + o, g_hs);
    }
}

/* x += Wo attention(q, k, v) */
__attribute__((noinline)) static void attention_block(uint32_t pos, const float* cs) {
    qkv(pos, cs);
    attention(pos);
    g_xs = quantize(xq, xb2, g_dim);
    proj(xb, WO);
    residual();
}

/* hb = silu(hb) * hb2 */
__attribute__((noinline)) static void silu(void) {
    for (uint32_t i = 0; i < g_hidden; i++) {
        float a = hb[i];
        a = a * (1.0f / (1.0f + llm_exp(-a)));
        hb[i] = a * hb2[i];
    }
}

/* x += W2 (silu(W1 norm(x)) * W3 norm(x)) */
__attribute__((noinline)) static void ffn_block(void) {
    norm((const float*)g_t[g_l][RMS_FFN]);
    g_xs = quantize(xq, xb, g_dim);
    proj(hb, W1);
    proj(hb2, W3);
    silu();
    g_xs = quantize(xq, hb, g_hidden);
    proj(xb, W2);
    residual();
}

/* the most likely next token, the first of equal ones */
__attribute__((noinline)) static uint32_t classify(void) {
    norm(g_rms_final);
    const float s = quantize(xq, xb, g_dim);
    uint32_t best = 0;
    float best_logit = 0.0f;
    for (uint32_t r = 0; r < g_vocab; r++) {
        const float logit = (float)dot(g_cls_q + r * g_dim, xq, g_dim) * (g_cls_s[r] * s);
        if (r == 0 || logit > best_logit) { best = r; best_logit = logit; }
    }
    return best;
}

/* ---- loading ------------------------------------------------------------- */
__attribute__((noinline)) static void load(void) {
    /* The linear memory grows to the pod's limit: the slice, then the cache */
    uint32_t used, limit;
    k32_mem_stats(&used, &limit);
    uint32_t pages = (uint32_t)__builtin_wasm_memory_size(0);
    const uint32_t want = (limit + 65535u) / 65536u;
    if (want > pages) {
        /* above one page, a limit in whole pages: memory.grow stops at the limit */
        if (__builtin_wasm_memory_grow(0, want - pages) != (__SIZE_TYPE__)-1) pages = want;
        else say("the linear memory does not grow to the limit, 64 KiB pages: ", (int32_t)want);
    }
    const uint32_t end = limit && limit < pages * 65536u ? limit : pages * 65536u;
    uint8_t* p = (uint8_t*)(((uintptr_t)&__heap_base + 15u) & ~(uintptr_t)15u);
    uint32_t n = 0;
    const int32_t rc = k32_http_call("GET", 3, g_url, g_url_len, "", 0, 0, 0, p, end - (uint32_t)(uintptr_t)p, &n, 30000);
    if (rc != 200) { say("fetching the model slice: ", rc); return; }
    B = (const struct llm_blob*)p;
    g_dim = B->dim; g_hidden = B->hidden; g_heads = B->n_heads; g_kvh = B->n_kv_heads; g_vocab = B->vocab;
    g_nl = B->layer_end - B->layer_begin;
    g_bits = B->bits;
    if (n < sizeof *B || B->magic != LLM_BLOB_MAGIC || B->bytes != n || !g_heads || !g_kvh || g_nl > LLM_MAX_LAYERS ||
        (g_bits != 8 && g_bits != 4)) {
        say("not a model slice, bytes: ", (int32_t)n);
        return;
    }
    g_hs = g_dim / g_heads;
    g_kvd = g_kvh * g_hs;
    g_dims[0] = 1; g_dims[1] = g_dim; g_dims[2] = g_kvd; g_dims[3] = g_hidden;
    const uint8_t* c = p + sizeof *B;
    if (B->flags & LLM_BLOB_EMBED) {
        g_tok_q = (const int8_t*)c; c += g_vocab * g_dim;
        g_tok_s = (const float*)c; c += 4 * g_vocab;
        g_rope = (const float*)c; c += 4 * B->seq_len * g_hs;
    }
    for (uint32_t l = 0; l < g_nl; l++)
        for (uint32_t t = 0; t < TENSORS; t++) {
            g_t[l][t] = c;
            c += g_dims[kShape[t] >> 4] * g_dims[kShape[t] & 15] * (t < WQ ? 32 : g_bits) / 8;
        }
    if (B->flags & LLM_BLOB_HEAD) {
        g_rms_final = (const float*)c; c += 4 * g_dim;
        g_cls_s = (const float*)c; c += 4 * g_vocab;
        g_cls_q = (const int8_t*)c; c += g_vocab * g_dim;
    }
    if (c != p + n || (g_dim | g_hidden) & 3) { say("not a model slice for this shard, bytes: ", (int32_t)n); return; }
    /* After the slice: activations, the queue, then the cache: per position
     * and layer K and V as int8 with a scale per KV head, and one score */
    float** f[] = {&x, &xb, &xb2, &q, &k, &v, &hb, &hb2};
    const uint32_t fn[] = {g_dim, g_dim, g_dim, g_dim, g_kvd, g_kvd, g_hidden, g_hidden};
    for (uint32_t i = 0; i < 8; i++) { *f[i] = (float*)c; c += 4 * fn[i]; }
    xq = (int8_t*)c; c += g_hidden + g_dim;
    g_msg_bytes = llm_msg_bytes(g_hs, g_dim);
    g_queue = (uint8_t*)c; c += (QUEUE + 1) * g_msg_bytes;
    const uint32_t kvn = g_nl * g_kvd, ksn = g_nl * g_kvh;
    const uint32_t left = end - (uint32_t)(uintptr_t)c;
    g_ctx = (uint32_t)(uintptr_t)c < end ? left / (2 * kvn + 8 * ksn + 4) : 0;
    if (g_ctx > B->seq_len) g_ctx = B->seq_len;
    if (g_ctx < 8) { say("the memory limit leaves room for positions: ", (int32_t)g_ctx); return; }
    att = (float*)c; c += 4 * g_ctx;
    g_ks = (float*)c; c += 4 * ksn * g_ctx;
    g_vs = (float*)c; c += 4 * ksn * g_ctx;
    g_kq = (int8_t*)c; c += kvn * g_ctx;
    g_vq = (int8_t*)c;
    g_loaded = 1;
    g_trace.stage = (uint8_t)g_stage;
    g_trace.layer_begin = (uint8_t)B->layer_begin;
    g_trace.layer_end = (uint8_t)B->layer_end;
    g_trace.used = 1;
    g_trace.context = (uint16_t)g_ctx;
    say("model slice loaded, context positions: ", (int32_t)g_ctx);
}

/* ---- the screen ---------------------------------------------------------- */
#define COLS 40
#define ROWS 15

static void newline(void) {
    g_col = 0;
    if (++g_row > ROWS) {
        k32_display_rect(0, 8, 240, 127, 0);
        g_row = 1;
    }
}

/* The title row and the text of the position just computed */
__attribute__((noinline)) static void screen(const struct llm_msg* m) {
    if (m->flags & LLM_MSG_CLEAR) {
        k32_display_rect(0, 0, 240, 135, 0);
        g_col = 0;
        g_row = 1;
    }
    g_n = 0;
    put(" kube32 llm  ");
    for (uint32_t i = 0; i < sizeof m->status && m->status[i]; i++) g_line[g_n++] = m->status[i];
    while (g_n < COLS) g_line[g_n++] = ' ';
    k32_display_text(0, 0, g_line, COLS, 0x0000, 0x0451);
    const char* s = m->text;
    uint32_t n = m->text_len > sizeof m->text ? sizeof m->text : m->text_len;
    if (n && s[0] == ' ' && g_col + n > COLS) { s++; n--; newline(); }
    while (n) {
        uint32_t k = 0;
        while (k < n && s[k] != '\n' && g_col + k < COLS) k++;
        if (k) k32_display_text((int32_t)g_col * 6, (int32_t)g_row * 8, s, k, m->flags & LLM_MSG_PROMPT ? 0x8c71 : 0xffff, 0);
        g_col += k; s += k; n -= k;
        if (n) {
            if (*s == '\n') { s++; n--; }
            newline();
        }
    }
}

/* ---- the pipeline -------------------------------------------------------- */
/* x and the rotary coefficients of the oldest queued position */
__attribute__((noinline)) static struct llm_msg* begin(void) {
    struct llm_msg* m = (struct llm_msg*)(g_queue + g_qhead * g_msg_bytes);
    float* f = llm_msg_floats(m);
    if (B->flags & LLM_BLOB_EMBED) {
        for (uint32_t i = 0; i < g_dim; i++) x[i] = (float)g_tok_q[m->token * g_dim + i] * g_tok_s[m->token];
        __builtin_memcpy(f, g_rope + m->pos * g_hs, 4 * g_hs);
    } else {
        __builtin_memcpy(x, f + g_hs, 4 * g_dim);
    }
    return m;
}

/* The position's output and this stage's trace into the outgoing slot */
__attribute__((noinline)) static void end(struct llm_msg* m, uint32_t t0) {
    float* f = llm_msg_floats(m);
    m->rope = g_hs;
    m->dim = g_dim;
    m->next = LLM_NO_TOKEN;
    if (B->flags & LLM_BLOB_HEAD) {
        if (m->flags & LLM_MSG_SAMPLE) m->next = classify();
        m->dim = 0;
    }
    __builtin_memcpy(f + g_hs, x, 4 * m->dim);
    struct llm_trace* t = &m->trace[g_stage & (LLM_MAX_STAGES - 1)];
    *t = g_trace;
    t->wait_us = t0 - g_queued_at[g_qhead];
    t->compute_us = now_us() - t0;
    t->call_us = g_last_call_us;
    g_out_len = llm_msg_bytes(g_hs, m->dim);
    __builtin_memcpy(out_msg(), m, g_out_len);
    g_out_pending = 1;
    g_qhead = (g_qhead + 1) % QUEUE;
    g_qcount--;
}

/* The oldest queued position through this stage */
__attribute__((noinline)) static void compute(void) {
    const uint32_t t0 = now_us();
    struct llm_msg* m = begin();
    for (g_l = 0; g_l < g_nl; g_l++) {
        attention_block(m->pos, llm_msg_floats(m));
        ffn_block();
    }
    end(m, t0);
}

/* The outgoing position to the next stage: 0 to try again later */
__attribute__((noinline)) static int forward(void) {
    uint32_t got;
    char resp[48];
    const uint32_t t0 = now_us();
    const int32_t rc = k32_http_call("POST", 4, g_next, g_next_len, "", 0, out_msg(), g_out_len, (uint8_t*)resp, sizeof resp, &got, 5000);
    if (rc == 200) g_last_call_us = now_us() - t0;
    else if (rc == 409 || rc == 400 || rc == 413) say("the next stage refused a position: ", rc);
    else {
        if (rc != 503) say("the next stage did not answer: ", rc);
        return 0;
    }
    g_out_pending = 0;
    return 1;
}

/* ---- exports ------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

/* Settings from the environment; after a move NODE_NAME names the new node */
K32_EXPORT("k32_init") int32_t k32_init(void) {
    char num[4];
    g_stage = llm_atou(num, llm_env("STAGE", num, sizeof num));
    g_url_len = llm_env("MODEL_URL", g_url, sizeof g_url);
    g_next_len = llm_env("NEXT_URL", g_next, sizeof g_next);
    llm_env("NODE_NAME", g_trace.node, sizeof g_trace.node);
    g_screen = llm_env("SCREEN", num, sizeof num) && num[0] == '1';
    g_retry_at = 0;
    return 0;
}

K32_EXPORT("k32_restored") int32_t k32_restored(void) { return k32_init(); }

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    if (!g_loaded) {
        if ((int32_t)(now - g_load_at) >= 0) {
            load();
            g_load_at = now + 2000;
        }
        return 0;
    }
    /* Ticks every 10 ms while positions come, at the pod's tick interval
     * after ACTIVE_MS without one */
    if (g_qcount || g_out_pending) g_last_work = now;
    if (g_out_pending && (int32_t)(now - g_retry_at) >= 0 && !forward()) g_retry_at = now + RETRY_MS;
    if (!g_out_pending && g_qcount) {
        compute();
        if (!forward()) g_retry_at = now + RETRY_MS;
        const struct llm_msg* m = (const struct llm_msg*)out_msg();
        if (g_screen && (m->flags & LLM_MSG_SHOW)) screen(m);
    }
    return now - g_last_work < ACTIVE_MS;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    const char* path;
    uint32_t body;
    llm_request(req, len, &path, &body);
    struct llm_msg* m = (struct llm_msg*)(req + body);
    const uint32_t n = len - body;
    int32_t status = 200;
    if (req[0] == 'G') {
    } else if (!g_loaded || g_qcount == QUEUE) {
        status = 503;
    } else if (n < sizeof *m || m->magic != LLM_MSG_MAGIC || n != llm_msg_bytes(m->rope, m->dim) ||
               ((B->flags & LLM_BLOB_EMBED) ? m->token >= g_vocab : (m->dim != g_dim || m->rope != g_hs))) {
        status = 400;
    } else if (m->pos >= g_ctx) {
        status = 413;
    } else if (m->pos == 0 && m->session != g_session) {
        g_qcount = g_out_pending = 0;   /* a new session: the old one's positions go */
        g_session = m->session;
        g_next_pos = 0;
    } else if (m->session != g_session || m->pos > g_next_pos) {
        status = 409;
    }
    if (status == 200 && req[0] == 'P' && m->pos == g_next_pos) {
        const uint32_t slot = (g_qhead + g_qcount) % QUEUE;
        __builtin_memcpy(g_queue + slot * g_msg_bytes, m, n);
        g_queued_at[slot] = now_us();
        g_qcount++;
        g_next_pos++;
    }
    /* a position taken before answers 200 too: the sender missed the answer */
    g_n = 0;
    put("{\"stage\":"); put_u(g_stage);
    put(",\"context\":"); put_u(g_ctx);
    put(",\"next\":"); put_u(g_next_pos); put("}");
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(g_line, g_n);
    k32_resp_end();
    return status;
}
