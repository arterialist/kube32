/* llm-speculative: greedy speculative decoding in one pod. The target model's
 * weights stay in a flash blob and the node's kernels read them in place;
 * so do the draft model's, in a blob of their own, and their norm weights and
 * rotary tables. The pod's memory holds the activations and both KV caches.
 *
 * A pass drafts k tokens with the draft model, then runs the target over the
 * newest token and the k drafts at once: k32_matmul_* reads each weight row
 * once for all k + 1 positions. The longest prefix of drafts the target
 * agrees with is kept, plus the target's own token after it. Thus the text
 * is the target's greedy text, token for token, and a pass that keeps more
 * than one token saves a read of the target's weights.
 *
 * Each position is computed as llm-shard computes it (apps/llm-pipeline,
 * llm.h): tools/spec.py gives the tokens, drafts and accepted counts of
 * every pass. Positions of one pass write their keys and values into the
 * cache before any of them attends, and each attends up to itself, so the
 * k + 1 positions get the bits of k + 1 single positions.
 *
 * Environment:
 *   TARGET     the target's blob (default target-lm), read in place
 *   DRAFT      the draft's blob (default stories260k), read in place
 *   TOKENIZER  the tokenizer's blob (default tok512)
 *   K          drafts a pass when a request names none (default 4, at most 7)
 *   STEPS      tokens to generate when a request names none (default 96)
 *   CONTEXT    positions at most (default and limit: what the memory and
 *              both models hold)
 *
 *   POST /generate?mode=speculative&k=4&steps=96   the prompt as the body;
 *        mode target runs the target alone, draft the draft alone;
 *        batch=0 runs the target's products one position at a time
 *   GET  /        state, settings, timing, acceptance, bytes read and text (JSON)
 *   GET  /text    the text so far */
#include "../llm-pipeline/tokenizer.h"

#define PMAX 8                 /* positions in one pass of a model */
#define KMAX (PMAX - 1)
#define CTX_MAX 512
#define CHUNK 32768            /* bytes a k32_blob_read copies */

extern unsigned char __heap_base;

/* ---- the models ---------------------------------------------------------- */
/* Per layer 9 float vectors, then 7 int8 or int4 matrices (llm.h). Each
 * tensor's shape is rows x columns of {1, dim, kv_dim, hidden}; the row
 * scales of matrix t are tensor t - 8 (t - 7 after RMS_FFN). */
enum { RMS_ATT, WQ_S, WK_S, WV_S, WO_S, RMS_FFN, W1_S, W2_S, W3_S, WQ, WK, WV, WO, W1, W2, W3, TENSORS };
static const uint8_t kShape[TENSORS] = {0x10, 0x10, 0x20, 0x20, 0x10, 0x10, 0x30, 0x10, 0x30,
                                        0x11, 0x21, 0x21, 0x11, 0x31, 0x13, 0x31};

struct model {
    int32_t blob;              /* the blob, open while it is read in place; -1 once in memory */
    uint32_t base;             /* the slice: a blob address or a memory address */
    uint32_t dim, hidden, heads, kvh, hs, kvd, vocab, seq_len, nl, bits;
    uint32_t dims[4];
    uint32_t off[LLM_MAX_LAYERS][TENSORS];   /* tensor offsets in the slice */
    uint32_t tok_q, tok_s, rope_off, rms_final, cls_s, cls_q;
    int8_t *kq, *vq;           /* [layer][position][kv_dim] */
    float *ks, *vs;            /* [layer][position][kv_heads] */
    uint32_t valid;            /* cache positions 0..valid-1 hold the current text */
    uint32_t bytes;            /* bytes of weights read since the counter was cleared */
    uint64_t mm_us;            /* time in the matrix products since then */
    uint64_t att_us;           /* time in attention since then */
};
static struct model g_target, g_draft;

/* activations of up to PMAX positions, sized for the larger model */
static float *X, *XB, *XB2, *Q, *K, *V, *HB, *HB2, *ATT, *LOGITS;
static float XS[PMAX];
static int8_t* XQ;
static uint32_t g_ctx, g_batch = 1;

static char g_name[3][48] = {"target-lm", "stories260k", "tok512"};
static uint32_t g_loaded, g_load_at, g_failed;

/* ---- text out ------------------------------------------------------------ */
static char g_line[256];
static uint32_t g_n;
static void put_n(const char* s, uint32_t n) { for (uint32_t i = 0; i < n && g_n < sizeof g_line; i++) g_line[g_n++] = s[i]; }
static void put(const char* s) { put_n(s, llm_strlen(s)); }
__attribute__((noinline)) static void put_u(uint32_t v) {
    char t[10]; uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && g_n < sizeof g_line) g_line[g_n++] = t[--n];
}
/* v / 10^d with d decimals */
__attribute__((noinline)) static void put_fixed(uint64_t v, uint32_t d) {
    uint64_t p = 1;
    for (uint32_t i = 0; i < d; i++) p *= 10;
    put_u((uint32_t)(v / p));
    if (!d) return;
    put(".");
    char t[8];
    uint64_t f = v % p;
    for (uint32_t i = d; i-- > 0;) { t[i] = (char)('0' + f % 10); f /= 10; }
    put_n(t, d);
}
static void log_line(void) { k32_log(g_line, g_n); g_n = 0; }
static void flush(void) { k32_resp_write(g_line, g_n); g_n = 0; }
/* "llm-speculative: <what><value>" to the log */
__attribute__((noinline)) static void say(const char* what, int32_t value) {
    g_n = 0;
    put("llm-speculative: "); put(what);
    if (value < 0) { put("-"); value = -value; }
    put_u((uint32_t)value);
    log_line();
}
static uint32_t now_us(void) { return (uint32_t)esp32_get_time_us(); }

/* ---- arithmetic: tools/llm.py repeats each operation in this order -------- */
/* The node's element-wise imports compute each step as llm-shard does
 * (k32_app_abi.h), for all positions of a pass in one call. */

/* a float vector of the slice: a blob address the imports read in place */
static const float* at(const struct model* m, uint32_t off) { return (const float*)(uintptr_t)(m->base + off); }

/* XQ, XS = the int8 vectors and scales of norm(src[p]) by the weights at off, n positions */
static void norm_quantize(const struct model* m, const float* src, uint32_t off, uint32_t n) {
    k32_rmsnorm_f32(XB, src, at(m, off), m->dim, n, 1e-5f);
    k32_quantize_i8(XQ, XS, XB, m->dim, n);
}

/* bytes of the slice at off into dst */
__attribute__((noinline)) static void fetch(const struct model* m, uint32_t off, void* dst, uint32_t n) {
    if (m->blob < 0) __builtin_memcpy(dst, (const uint8_t*)(uintptr_t)(m->base + off), n);
    else k32_blob_read(m->blob, off, dst, n);
}

/* out[p] = W_t of layer l times XQ[p] (cols values each) for n positions:
 * one call reads each row once for all of them (batch 0: a call a position) */
static void mm_calls(struct model* m, float* out, uint32_t w, uint32_t ws, uint32_t rows, uint32_t cols, uint32_t row_bytes, uint32_t n);
/* rows 0: the tensor's rows; more take the matrices that follow it in the slice */
__attribute__((noinline)) static void mm_rows(struct model* m, float* out, uint32_t l, uint32_t t, uint32_t rows, uint32_t n);
static void mm(struct model* m, float* out, uint32_t l, uint32_t t, uint32_t n) { mm_rows(m, out, l, t, 0, n); }
__attribute__((noinline)) static void mm_rows(struct model* m, float* out, uint32_t l, uint32_t t, uint32_t rows, uint32_t n) {
    const uint32_t cols = m->dims[kShape[t] & 15];
    if (!rows) rows = m->dims[kShape[t] >> 4];
    const uint32_t w = m->base + m->off[l][t], ws = m->base + m->off[l][t - (t >= W1 ? 7 : 8)];
    const uint32_t row_bytes = m->bits == 4 ? (cols + 1) / 2 : cols;
    const uint32_t t0 = now_us();
    mm_calls(m, out, w, ws, rows, cols, row_bytes, n);
    m->mm_us += now_us() - t0;
}

__attribute__((noinline)) static void mm_calls(struct model* m, float* out, uint32_t w, uint32_t ws, uint32_t rows, uint32_t cols,
                                               uint32_t row_bytes, uint32_t n) {
    if (g_batch) {
        if (m->bits == 4) k32_matmul_i4(out, (const void*)(uintptr_t)w, (const float*)(uintptr_t)ws, XQ, XS, rows, cols, 0, n);
        else k32_matmul_i8(out, (const int8_t*)(uintptr_t)w, (const float*)(uintptr_t)ws, XQ, XS, rows, cols, 0, n);
        m->bytes += rows * (row_bytes + 4);
        return;
    }
    for (uint32_t p = 0; p < n; p++) {
        if (m->bits == 4)
            k32_matvec_i4(out + p * rows, (const void*)(uintptr_t)w, (const float*)(uintptr_t)ws, XQ + p * cols, XS[p], rows, cols, 0);
        else
            k32_matvec_i8(out + p * rows, (const int8_t*)(uintptr_t)w, (const float*)(uintptr_t)ws, XQ + p * cols, XS[p], rows, cols, 0);
        m->bytes += rows * (row_bytes + 4);
    }
}

/* Each head of q attends to the cached positions 0..pos of its KV head
 * (scores, their softmax, the weighted values) into out */
/* every head of positions pos0..pos0+n-1 over layer l's cache, in one call */
__attribute__((noinline)) static void attention(const struct model* m, uint32_t l, uint32_t pos0, uint32_t n, const float* q, float* out) {
    const float scale = 1.0f / __builtin_sqrtf((float)m->hs);
    const uint32_t base = l * g_ctx;
    k32_attention_f32_i8(out, m->kq + base * m->kvd, m->ks + base * m->kvh, m->vq + base * m->kvd, m->vs + base * m->kvh, q, scale,
                         m->heads, m->kvh, m->hs, m->kvd, pos0, n, ATT);
}

/* x += Wo attention(q, k, v) for positions pos0..pos0+n-1 */
__attribute__((noinline)) static void attention_block(struct model* m, uint32_t l, uint32_t pos0, uint32_t n) {
    const uint32_t dim = m->dim, kvd = m->kvd, hs = m->hs, row = l * g_ctx + pos0;
    norm_quantize(m, X, m->off[l][RMS_ATT], n);
    const float* cs = at(m, m->rope_off + 4 * pos0 * hs);
    if (n == 1) {
        /* one position: q, k and v in one product (Wq, Wk, Wv and their scales follow each other
         * in the slice) and one rotary call over q and k, the same pairs as two */
        mm_rows(m, Q, l, WQ, dim + 2 * kvd, 1);
        k32_rope_f32(Q, cs, dim + kvd, hs, 1);
        k32_quantize_i8(m->kq + row * kvd, m->ks + row * m->kvh, Q + dim, hs, m->kvh);
        k32_quantize_i8(m->vq + row * kvd, m->vs + row * m->kvh, Q + dim + kvd, hs, m->kvh);
    } else {
        mm(m, Q, l, WQ, n);
        mm(m, K, l, WK, n);
        mm(m, V, l, WV, n);
        k32_rope_f32(Q, cs, dim, hs, n);
        k32_rope_f32(K, cs, kvd, hs, n);
        /* K and V of the n positions into the cache, int8 with a scale per KV head */
        k32_quantize_i8(m->kq + row * kvd, m->ks + row * m->kvh, K, hs, n * m->kvh);
        k32_quantize_i8(m->vq + row * kvd, m->vs + row * m->kvh, V, hs, n * m->kvh);
    }
    const uint32_t t0 = now_us();
    attention(m, l, pos0, n, Q, XB2);
    m->att_us += now_us() - t0;
    k32_quantize_i8(XQ, XS, XB2, dim, n);
    mm(m, XB, l, WO, n);
    k32_add_f32(X, XB, n * dim);
}

/* x += W2 (silu(W1 norm(x)) * W3 norm(x)) */
__attribute__((noinline)) static void ffn_block(struct model* m, uint32_t l, uint32_t n) {
    norm_quantize(m, X, m->off[l][RMS_FFN], n);
    mm(m, HB, l, W1, n);
    mm(m, HB2, l, W3, n);
    k32_swiglu_f32(HB, HB2, n * m->hidden);
    k32_quantize_i8(XQ, XS, HB, m->hidden, n);
    mm(m, XB, l, W2, n);
    k32_add_f32(X, XB, n * m->dim);
}

/* The most likely token after positions first..n-1 (the first of equal
 * ones) into next[0..n-first) */
__attribute__((noinline)) static void classify(struct model* m, uint32_t first, uint32_t n, uint16_t* next) {
    const uint32_t dim = m->dim, cnt = n - first;
    norm_quantize(m, X + first * dim, m->rms_final, cnt);
    const uint32_t w = m->base + m->cls_q, ws = m->base + m->cls_s;
    const uint32_t t0 = now_us();
    if (g_batch) {
        k32_matmul_i8(LOGITS, (const int8_t*)(uintptr_t)w, (const float*)(uintptr_t)ws, XQ, XS, m->vocab, dim, 0, cnt);
        m->bytes += m->vocab * (dim + 4);
    } else {
        for (uint32_t i = 0; i < cnt; i++) {
            k32_matvec_i8(LOGITS + i * m->vocab, (const int8_t*)(uintptr_t)w, (const float*)(uintptr_t)ws, XQ + i * dim, XS[i], m->vocab, dim, 0);
            m->bytes += m->vocab * (dim + 4);
        }
    }
    m->mm_us += now_us() - t0;
    for (uint32_t i = 0; i < cnt; i++) next[i] = (uint16_t)k32_argmax_f32(LOGITS + i * m->vocab, m->vocab);
}

/* Positions pos0..pos0+n-1 (n <= PMAX) with input tokens toks[0..n) into the
 * cache; with next, the token the model picks after each position from
 * first on */
__attribute__((noinline)) static void forward(struct model* m, const uint16_t* toks, uint32_t pos0, uint32_t n, uint32_t first, uint16_t* next) {
    const uint32_t dim = m->dim;
    int8_t row[512];
    for (uint32_t i = 0; i < n; i++) {
        float s;
        fetch(m, m->tok_q + toks[i] * dim, row, dim);
        fetch(m, m->tok_s + 4 * toks[i], &s, 4);
        for (uint32_t c = 0; c < dim; c++) X[i * dim + c] = (float)row[c] * s;
        m->bytes += dim + 4;
    }
    for (uint32_t l = 0; l < m->nl; l++) {
        attention_block(m, l, pos0, n);
        ffn_block(m, l, n);
    }
    if (next) classify(m, first, n, next);
}

/* ---- loading ------------------------------------------------------------- */
static uint8_t* g_heap;
static uint32_t g_heap_end;

static uint8_t* take(uint32_t n) {
    uint8_t* p = g_heap;
    g_heap += (n + 15u) & ~15u;
    return p;
}

/* A model slice's header and tensor offsets; 0 when it is not a whole model */
__attribute__((noinline)) static int parse(struct model* m, const struct llm_blob* b, uint32_t size) {
    if (size < sizeof *b || b->magic != LLM_BLOB_MAGIC || b->bytes != size || !b->n_heads || !b->n_kv_heads ||
        b->layer_begin != 0 || b->layer_end != b->n_layers || b->n_layers > LLM_MAX_LAYERS ||
        (b->flags & (LLM_BLOB_EMBED | LLM_BLOB_HEAD | LLM_BLOB_TIED | LLM_BLOB_BIDIR)) != (LLM_BLOB_EMBED | LLM_BLOB_HEAD) ||
        (b->bits != 8 && b->bits != 4) || b->dim > 512 || (b->dim | b->hidden) & 3)
        return 0;
    m->dim = b->dim; m->hidden = b->hidden; m->heads = b->n_heads; m->kvh = b->n_kv_heads;
    m->vocab = b->vocab; m->seq_len = b->seq_len; m->nl = b->n_layers; m->bits = b->bits;
    m->hs = m->dim / m->heads;
    m->kvd = m->kvh * m->hs;
    m->dims[0] = 1; m->dims[1] = m->dim; m->dims[2] = m->kvd; m->dims[3] = m->hidden;
    uint32_t c = sizeof *b;
    m->tok_q = c; c += m->vocab * m->dim;
    m->tok_s = c; c += 4 * m->vocab;
    m->rope_off = c; c += 4 * m->seq_len * m->hs;
    for (uint32_t l = 0; l < m->nl; l++)
        for (uint32_t t = 0; t < TENSORS; t++) {
            m->off[l][t] = c;
            c += m->dims[kShape[t] >> 4] * m->dims[kShape[t] & 15] * (t < WQ ? 32 : m->bits) / 8;
        }
    m->rms_final = c; c += 4 * m->dim;
    m->cls_s = c; c += 4 * m->vocab;
    m->cls_q = c; c += m->vocab * m->dim;
    return c == size;
}

/* Opens a blob; with copy, reads it into memory and closes it. Returns its
 * size, 0 when it cannot be read */
__attribute__((noinline)) static uint32_t open_blob(const char* name, int32_t* handle, uint8_t** copy) {
    const int32_t h = k32_blob_open(name, llm_strlen(name));
    if (h < 0) {
        g_n = 0; put("llm-speculative: blob "); put(name); put(": error -"); put_u((uint32_t)-h); log_line();
        return 0;
    }
    const uint32_t size = (uint32_t)k32_blob_size(h);
    *handle = h;
    if (!copy) return size;
    if (size > g_heap_end - (uint32_t)(uintptr_t)g_heap) {
        g_n = 0; put("llm-speculative: no memory for blob "); put(name); put(", bytes: "); put_u(size); log_line();
        k32_blob_close(h);
        return 0;
    }
    *copy = take(size);
    for (uint32_t o = 0; o < size; o += CHUNK) k32_blob_read(h, o, *copy + o, size - o < CHUNK ? size - o : CHUNK);
    k32_blob_close(h);
    *handle = -1;
    return size;
}

/* the KV cache of a model */
static void place(struct model* m) {
    const uint32_t kvn = m->nl * m->kvd * g_ctx, ksn = 4 * m->nl * m->kvh * g_ctx;
    m->kq = (int8_t*)take(kvn);
    m->vq = (int8_t*)take(kvn);
    m->ks = (float*)take(ksn);
    m->vs = (float*)take(ksn);
}

/* A model read in place from its blob; its size, 0 when it is none */
__attribute__((noinline)) static uint32_t in_place(struct model* m, const char* name) {
    struct llm_blob hdr;
    const uint32_t n = open_blob(name, &m->blob, 0);
    if (n) k32_blob_read(m->blob, 0, &hdr, sizeof hdr);
    if (!n || !parse(m, &hdr, n)) {
        g_n = 0; put("llm-speculative: blob "); put(name); put(" is not a whole model, bytes: "); put_u(n); log_line();
        return 0;
    }
    if (!k32_blob_mapped(m->blob)) {
        g_n = 0; put("llm-speculative: blob "); put(name); put(" is not mapped: store it in the node's flash"); log_line();
        return 0;
    }
    m->base = (uint32_t)(uintptr_t)K32_BLOB_ADDR(m->blob, 0);
    return n;
}

static uint32_t per_position(const struct model* m) { return 2 * m->nl * (m->kvd + 4 * m->kvh); }

__attribute__((noinline)) static void load(uint32_t ctx_limit) {
    uint32_t used, limit;
    k32_mem_stats(&used, &limit);
    uint32_t pages = (uint32_t)__builtin_wasm_memory_size(0);
    const uint32_t want = (limit + 65535u) / 65536u;
    if (want > pages && __builtin_wasm_memory_grow(0, want - pages) != (__SIZE_TYPE__)-1) pages = want;
    g_heap_end = limit && limit < pages * 65536u ? limit : pages * 65536u;
    g_heap = (uint8_t*)(((uintptr_t)&__heap_base + 15u) & ~(uintptr_t)15u);

    int32_t h;
    uint8_t* raw;
    uint32_t n = open_blob(g_name[2], &h, &raw);
    if (!n || !tok_parse(raw, n)) { say("not a tokenizer, bytes: ", (int32_t)n); g_failed = 1; return; }
    if (!in_place(&g_draft, g_name[1]) || !(n = in_place(&g_target, g_name[0]))) { g_failed = 1; return; }
    if (g_target.vocab != g_draft.vocab || g_target.vocab != g_vocab) { say("the models' vocabularies differ: ", (int32_t)g_target.vocab); g_failed = 1; return; }

    /* activations for PMAX positions of the larger model */
    const uint32_t dim = g_target.dim > g_draft.dim ? g_target.dim : g_draft.dim;
    const uint32_t hidden = g_target.hidden > g_draft.hidden ? g_target.hidden : g_draft.hidden;
    float** f[] = {&X, &XB, &XB2, &Q, &K, &V, &HB, &HB2, &LOGITS};
    const uint32_t fn[] = {dim, dim, dim, dim, dim, dim, hidden, hidden, g_target.vocab};
    for (uint32_t i = 0; i < 9; i++) *f[i] = (float*)take(4 * PMAX * fn[i]);
    XQ = (int8_t*)take(PMAX * (dim > hidden ? dim : hidden));
    /* the rest: both caches and the scores, a position at a time */
    const uint32_t fixed = 256;
    const uint32_t left = g_heap_end - (uint32_t)(uintptr_t)g_heap;
    g_ctx = left > fixed ? (left - fixed) / (per_position(&g_target) + per_position(&g_draft) + 4) : 0;
    if (g_ctx > g_target.seq_len) g_ctx = g_target.seq_len;
    if (g_ctx > g_draft.seq_len) g_ctx = g_draft.seq_len;
    if (ctx_limit && g_ctx > ctx_limit) g_ctx = ctx_limit;
    if (g_ctx > CTX_MAX) g_ctx = CTX_MAX;
    if (g_ctx < 16) { say("the memory limit leaves room for positions: ", (int32_t)g_ctx); g_failed = 1; return; }
    ATT = (float*)take(4 * g_ctx);
    place(&g_target);
    place(&g_draft);
    g_loaded = 1;
    g_n = 0;
    put("llm-speculative: target "); put_u(g_target.nl); put(" layers dim "); put_u(g_target.dim); put(" int"); put_u(g_target.bits);
    put(", "); put_u(n); put(" B; draft "); put_u(g_draft.nl); put(" layers dim "); put_u(g_draft.dim);
    put(", both in place; context "); put_u(g_ctx); put(", memory used "); put_u((uint32_t)(uintptr_t)g_heap); put(" of "); put_u(g_heap_end);
    log_line();
}

/* ---- generation ---------------------------------------------------------- */
enum { IDLE, PREFILL, DECODE, DONE, FAILED };
static const char* const kStates[] = {"idle", "prefill", "decode", "done", "failed"};
enum { SPECULATIVE, TARGET, DRAFT };
static const char* const kModes[] = {"speculative", "target", "draft"};

static uint32_t g_state, g_mode, g_k = 4, g_steps, g_default_k = 4, g_default_steps = 96, g_ctx_limit;
static uint16_t g_toks[CTX_MAX];
static uint32_t g_ntok, g_nprompt, g_stop;
static char g_text[4096];
static uint32_t g_text_len;
static unsigned char g_block[2048];   /* k32_alloc: one request */

/* the generation's figures */
static uint64_t g_prefill_us, g_decode_us, g_verify_us, g_draft_us, g_t0;
static uint32_t g_passes, g_proposed, g_accepted, g_draft_positions, g_target_positions;
static uint64_t g_target_bytes, g_draft_bytes;

static void text_add(uint32_t prev, uint32_t tok) {
    char pc[64];
    const uint32_t n = tok_piece(prev, tok, pc);
    for (uint32_t i = 0; i < n && g_text_len < sizeof g_text; i++) g_text[g_text_len++] = pc[i];
}

static int start(const char* prompt, uint32_t n) {
    g_ntok = tok_encode(prompt, n, g_toks, g_ctx);
    g_nprompt = g_ntok;
    if (g_ntok >= g_ctx) return 413;
    g_text_len = 0;
    for (uint32_t i = 1; i < g_ntok; i++) text_add(g_toks[i - 1], g_toks[i]);
    g_target.valid = g_draft.valid = 0;
    g_prefill_us = g_decode_us = g_verify_us = g_draft_us = 0;
    g_passes = g_proposed = g_accepted = g_draft_positions = g_target_positions = 0;
    g_target_bytes = g_draft_bytes = 0;
    g_stop = 0;
    g_state = PREFILL;
    g_t0 = (uint64_t)esp32_get_time_us();
    return 200;
}

/* positions m->valid..upto-1 of the text, PMAX at a time, without tokens out */
static void catch_up(struct model* m, uint32_t upto) {
    while (m->valid < upto) {
        const uint32_t n = upto - m->valid < PMAX ? upto - m->valid : PMAX;
        forward(m, g_toks + m->valid, m->valid, n, 0, 0);
        m->valid += n;
    }
}

static void append(uint32_t tok) {
    if (tok == LLM_BOS) { g_stop = 1; return; }
    text_add(g_toks[g_ntok - 1], tok);
    g_toks[g_ntok++] = (uint16_t)tok;
}

/* The prompt's positions but the last, a chunk a tick */
static void prefill(void) {
    const uint32_t t0 = now_us(), upto = g_nprompt - 1;
    struct model* m = g_mode != DRAFT && g_target.valid < upto ? &g_target : g_mode != TARGET && g_draft.valid < upto ? &g_draft : 0;
    if (m) {
        const uint32_t n = upto - m->valid < PMAX ? upto - m->valid : PMAX;
        forward(m, g_toks + m->valid, m->valid, n, 0, 0);
        m->valid += n;
    }
    g_prefill_us += now_us() - t0;
    if (!m) {
        g_state = DECODE;
        g_t0 = (uint64_t)esp32_get_time_us();
        g_target.bytes = g_draft.bytes = 0;
        g_target.mm_us = g_draft.mm_us = 0;
        g_target.att_us = g_draft.att_us = 0;
    }
}

/* One pass: k drafts, the target over the newest token and the drafts, the
 * accepted ones and the target's token after them (tools/spec.py speculate) */
__attribute__((noinline)) static void pass(void) {
    const uint32_t len = g_ntok, gen = g_ntok - g_nprompt;
    uint16_t d[KMAX + 1], outs[PMAX];
    if (g_mode == DRAFT) {
        const uint32_t t0 = now_us();
        g_draft.valid = len - 1;
        forward(&g_draft, g_toks + len - 1, len - 1, 1, 0, outs);
        g_draft.valid = len;
        g_draft_us += now_us() - t0;
        g_draft_positions++;
        g_passes++;
        append(outs[0]);
        return;
    }
    uint32_t room = g_mode == TARGET ? 0 : g_k;
    if (room > g_steps - gen - 1) room = g_steps - gen - 1;
    if (room > g_ctx - len - 1) room = g_ctx - len - 1;
    if (room) {
        /* the draft catches up on the text, then proposes room tokens */
        const uint32_t t0 = now_us();
        if (g_draft.valid > len - 1) g_draft.valid = len - 1;
        catch_up(&g_draft, len - 1);
        forward(&g_draft, g_toks + len - 1, len - 1, 1, 0, outs);
        d[0] = outs[0];
        for (uint32_t j = 1; j < room; j++) {
            forward(&g_draft, &d[j - 1], len + j - 1, 1, 0, outs);
            d[j] = outs[0];
        }
        g_draft_positions += room;
        g_draft.valid = len + room - 1;
        g_draft_us += now_us() - t0;
    }
    /* one target pass over positions len-1..len-1+room */
    const uint32_t t1 = now_us();
    if (g_target.valid > len - 1) g_target.valid = len - 1;
    catch_up(&g_target, len - 1);
    uint16_t in[PMAX];
    in[0] = g_toks[len - 1];
    for (uint32_t j = 0; j < room; j++) in[j + 1] = d[j];
    forward(&g_target, in, len - 1, room + 1, 0, outs);
    g_verify_us += now_us() - t1;
    g_target_positions += room + 1;
    uint32_t n = 0;
    while (n < room && d[n] == outs[n]) n++;
    g_passes++;
    g_proposed += room;
    g_accepted += n;
    /* the caches hold the accepted text; later positions are computed again */
    g_target.valid = len + n;
    if (g_draft.valid > len + n) g_draft.valid = len + n;
    for (uint32_t j = 0; j < n && !g_stop; j++) append(d[j]);
    if (!g_stop) append(outs[n]);
}

__attribute__((noinline)) static void finish(void) {
    g_decode_us = (uint64_t)esp32_get_time_us() - g_t0;
    g_target_bytes = g_target.bytes;
    g_draft_bytes = g_draft.bytes;
    g_state = DONE;
    const uint32_t gen = g_ntok - g_nprompt;
    g_n = 0;
    put("llm-speculative: "); put(kModes[g_mode]);
    if (g_mode == SPECULATIVE) { put(" k="); put_u(g_k); }
    put(" tokens "); put_u(gen);
    put(" in "); put_fixed(g_decode_us / 1000, 0); put(" ms, tok/s ");
    put_fixed(g_decode_us ? (uint64_t)gen * 100000000ull / g_decode_us : 0, 2);
    put(", passes "); put_u(g_passes);
    if (g_proposed) { put(", acceptance "); put_fixed((uint64_t)g_accepted * 1000 / g_proposed, 3); }
    if (g_passes && g_mode != DRAFT) { put(", verify ms "); put_fixed(g_verify_us / g_passes / 100, 1); }
    if (g_draft_positions) { put(", draft ms/token "); put_fixed(g_draft_us / g_draft_positions / 100, 1); }
    if (gen) { put(", target bytes/token "); put_u((uint32_t)(g_target_bytes / gen)); }
    log_line();
}

/* ---- exports ------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char num[12];
    uint32_t n;
    llm_env("TARGET", g_name[0], sizeof g_name[0]);
    llm_env("DRAFT", g_name[1], sizeof g_name[1]);
    llm_env("TOKENIZER", g_name[2], sizeof g_name[2]);
    if ((n = llm_env("K", num, sizeof num))) g_default_k = llm_atou(num, n);
    if (g_default_k > KMAX) g_default_k = KMAX;
    if ((n = llm_env("STEPS", num, sizeof num))) g_default_steps = llm_atou(num, n);
    if ((n = llm_env("CONTEXT", num, sizeof num))) g_ctx_limit = llm_atou(num, n);
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    if (!g_loaded) {
        if (!g_failed && (int32_t)(uptime_ms - g_load_at) >= 0) {
            load(g_ctx_limit);
            g_load_at = uptime_ms + 5000;
        }
        return 0;
    }
    if (g_state == PREFILL) {
        prefill();
        return 1;
    }
    if (g_state != DECODE) return 0;
    if (g_stop || g_ntok - g_nprompt >= g_steps || g_ntok >= g_ctx) {
        finish();
        return 0;
    }
    pass();
    return 1;
}

/* A query parameter's value ("4" for k in "POST /generate?k=4&steps=64 HTTP/1.1") */
static uint32_t query(const char* req, uint32_t len, const char* key, char* out, uint32_t cap) {
    uint32_t i = 0, end = 0;
    while (end < len && req[end] != '\n') end++;
    while (i < end && req[i] != '?') i++;
    const uint32_t kl = llm_strlen(key);
    while (i < end && req[i] != ' ') {
        i++;   /* past '?' or '&' */
        uint32_t j = 0, n = 0;
        while (j < kl && i + j < end && req[i + j] == key[j]) j++;
        const int hit = j == kl && i + kl < end && req[i + kl] == '=';
        if (hit) i += kl + 1;
        for (; i < end && req[i] != '&' && req[i] != ' '; i++)
            if (hit && n + 1 < cap) out[n++] = req[i];
        if (hit) { out[n] = 0; return n; }
    }
    return 0;
}

/* A JSON string's contents, written out in pieces that fit the line */
static void text_out(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (g_n + 8 > sizeof g_line) flush();
        const char c = s[i];
        if (c == '"' || c == '\\') { g_line[g_n++] = '\\'; g_line[g_n++] = c; }
        else if (c == '\n') { g_line[g_n++] = '\\'; g_line[g_n++] = 'n'; }
        else if ((uint8_t)c < 0x20) g_line[g_n++] = ' ';
        else g_line[g_n++] = c;
    }
}

static void field(const char* name, uint64_t v, uint32_t decimals) {
    put(",\""); put(name); put("\":"); put_fixed(v, decimals);
    flush();
}

__attribute__((noinline)) static void status(void) {
    const uint32_t gen = g_ntok > g_nprompt ? g_ntok - g_nprompt : 0;
    const uint64_t decode_us = g_state == DONE ? g_decode_us : g_state == DECODE ? (uint64_t)esp32_get_time_us() - g_t0 : 0;
    const uint64_t tbytes = g_state == DONE ? g_target_bytes : g_target.bytes;
    k32_resp_begin(200, "application/json", 16);
    g_n = 0;
    put("{\"state\":\""); put(g_failed ? "failed" : g_loaded ? kStates[g_state] : "loading");
    put("\",\"mode\":\""); put(kModes[g_mode]); put("\"");
    field("k", g_mode == SPECULATIVE ? g_k : 0, 0);
    field("batch", g_batch, 0);
    field("context", g_ctx, 0);
    field("target_bits", g_target.bits, 0);
    field("prompt_tokens", g_nprompt, 0);
    field("tokens", gen, 0);
    field("prefill_ms", g_prefill_us / 1000, 0);
    field("decode_ms", decode_us / 1000, 0);
    field("tokens_per_s", decode_us ? (uint64_t)gen * 1000000000ull / decode_us : 0, 3);
    field("passes", g_passes, 0);
    field("proposed", g_proposed, 0);
    field("accepted", g_accepted, 0);
    field("acceptance", g_proposed ? (uint64_t)g_accepted * 1000 / g_proposed : 0, 3);
    field("tokens_per_pass", g_passes ? (uint64_t)gen * 1000 / g_passes : 0, 3);
    field("verify_ms_per_pass", g_passes && g_mode != DRAFT ? g_verify_us / g_passes : 0, 3);
    field("draft_ms_per_token", g_draft_positions ? g_draft_us / g_draft_positions : 0, 3);
    field("draft_ms", g_draft_us / 1000, 0);
    field("verify_ms", g_verify_us / 1000, 0);
    field("target_positions", g_target_positions, 0);
    field("target_matmul_ms", g_target.mm_us / 1000, 0);
    field("draft_matmul_ms", g_draft.mm_us / 1000, 0);
    field("target_attention_ms", g_target.att_us / 1000, 0);
    field("draft_attention_ms", g_draft.att_us / 1000, 0);
    field("target_bytes", tbytes, 0);
    field("target_bytes_per_token", gen ? tbytes / gen : 0, 0);
    put(",\"tokens_list\":[");
    for (uint32_t i = 0; i < g_ntok; i++) {
        if (g_n + 8 > sizeof g_line) flush();
        if (i) put(",");
        put_u(g_toks[i]);
    }
    flush();   /* the token list can fill the line */
    put("],\"text\":\"");
    text_out(g_text, g_text_len);
    put("\"}\n");
    flush();
    k32_resp_end();
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    const char* path;
    uint32_t body;
    const uint32_t plen = llm_request(req, len, &path, &body);
    if (req[0] == 'P' && llm_ends(path, plen, "/generate")) {
        int32_t rc = 200;
        char v[16];
        uint32_t n;
        if (!g_loaded) rc = 503;
        else if (g_state == PREFILL || g_state == DECODE) rc = 409;
        else {
            g_mode = SPECULATIVE;
            if ((n = query(req, len, "mode", v, sizeof v))) g_mode = llm_eq(v, n, "target") ? TARGET : llm_eq(v, n, "draft") ? DRAFT : SPECULATIVE;
            g_k = (n = query(req, len, "k", v, sizeof v)) ? llm_atou(v, n) : g_default_k;
            if (g_k > KMAX) g_k = KMAX;
            if (g_k == 0 && g_mode == SPECULATIVE) g_mode = TARGET;
            g_steps = (n = query(req, len, "steps", v, sizeof v)) ? llm_atou(v, n) : g_default_steps;
            if (!g_steps) g_steps = 1;
            g_batch = !((n = query(req, len, "batch", v, sizeof v)) && v[0] == '0');
            rc = start(req + body, len - body);
        }
        k32_resp_begin(rc, "application/json", 16);
        g_n = 0;
        put(rc == 200 ? "{\"started\":true}\n" : rc == 409 ? "{\"error\":\"a generation is running\"}\n" :
            rc == 413 ? "{\"error\":\"the prompt does not fit the context\"}\n" : "{\"error\":\"the models are loading\"}\n");
        flush();
        k32_resp_end();
        return rc;
    }
    if (req[0] == 'G' && llm_ends(path, plen, "/text")) {
        k32_resp_begin(200, "text/plain; charset=utf-8", 25);
        k32_resp_write(g_text, g_text_len);
        k32_resp_end();
        return 200;
    }
    status();
    return 200;
}
