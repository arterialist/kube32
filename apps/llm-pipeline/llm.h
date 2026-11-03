/* llm.h: wire formats and arithmetic shared by the pods of llm-pipeline.
 *
 * Every float operation of the pods is a single IEEE-754 binary32 operation
 * in a fixed order (no fused multiply-add, no reassociation, exp from its
 * own polynomial), so they compute the same bits on an ESP32-S3, in
 * kube32-sim on any host, and in tools/llm.py's reference. Weights are int8
 * (or int4) with one scale per output row; each activation vector is
 * quantized to int8 with one scale before a matrix product, so the products
 * accumulate as exact integers.
 *
 * Model blob (a shard's slice of the model, as tools/llm.py writes it):
 *   struct llm_blob header (64 bytes)
 *   EMBED: int8 tok_q[vocab][dim], float tok_s[vocab],
 *          float rope[seq_len][head_size] (cos, sin of each pair)
 *   each layer: float rms_att[dim], wq_s[dim], wk_s[kv_dim], wv_s[kv_dim],
 *          wo_s[dim], rms_ffn[dim], w1_s[hidden], w2_s[dim], w3_s[hidden],
 *          then the matrices wq[dim][dim], wk[kv_dim][dim], wv[kv_dim][dim],
 *          wo[dim][dim], w1[hidden][dim], w2[dim][hidden], w3[hidden][dim]
 *          as int8, or with bits 4 two values a byte (column 2j in the low
 *          nibble, 2j+1 in the high one, each -7..7)
 *   HEAD:  float rms_final[dim], cls_s[vocab], int8 cls_q[vocab][dim]
 *          (with TIED only rms_final: the classifier is tok_q and tok_s)
 * Matrices are row-major, one row per output. A blob with BIDIR is a masked
 * diffusion model: every position attends to every position of its window,
 * and the last vocabulary entry is MASK, an input the classifier never
 * picks. */
#ifndef LLM_H
#define LLM_H

#include "../../include/runtime/k32_app_abi.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);   /* microseconds since the node booted */

#define LLM_BLOB_MAGIC   0x4d32334bu   /* "K32M" */
#define LLM_MSG_MAGIC    0x4132334bu   /* "K32A" */
#define LLM_BLOB_EMBED   1u            /* token embedding and rotary table */
#define LLM_BLOB_HEAD    2u            /* final norm and classifier */
#define LLM_BLOB_TIED    4u            /* the classifier is the embedding, stored once */
#define LLM_BLOB_BIDIR   8u            /* masked diffusion: attention over the whole window */
#define LLM_MSG_SAMPLE   1u            /* the last stage picks the next token */
#define LLM_MSG_SHOW     2u            /* a stage with a screen shows text */
#define LLM_MSG_PROMPT   4u            /* text is the prompt's */
#define LLM_MSG_CLEAR    8u            /* a new generation: clear the screen first */
#define LLM_NO_TOKEN     0xffffffffu
#define LLM_MAX_LAYERS   8
#define LLM_MAX_STAGES   4
#define LLM_NODE_CHARS   20
#define LLM_BOS          1u

struct llm_blob {
    uint32_t magic, version;
    uint32_t dim, hidden, n_heads, n_kv_heads, vocab, seq_len;
    uint32_t layer_begin, layer_end, n_layers, flags;
    uint32_t bytes;                   /* the whole blob, header included */
    uint32_t bits;                    /* of the layers' matrices: 8 or 4 */
    uint32_t reserved[2];
};

/* What a stage did with one position: filled in by the stage itself */
struct llm_trace {
    uint8_t stage, layer_begin, layer_end, used;
    uint16_t context;                 /* positions the stage's cache holds */
    uint16_t reserved;
    uint32_t wait_us;                /* queued before the stage computed it */
    uint32_t compute_us;              /* the stage's layers (and head) */
    uint32_t call_us;                 /* the stage's previous forward call, round trip */
    char node[LLM_NODE_CHARS];        /* the node the stage ran on */
};

/* One position on its way through the pipeline (POST body, little-endian).
 * After the header: float rope[rope] (cos, sin of each pair of a head, for
 * this position; from the first stage on) and float x[dim] (the residual
 * stream; none before the first stage and after the last). */
struct llm_msg {
    uint32_t magic;
    uint32_t session;                 /* one generation; a new one starts at pos 0 */
    uint32_t pos;
    uint32_t token;                   /* the input token at pos */
    uint32_t next;                    /* the token the last stage picked, or LLM_NO_TOKEN */
    uint32_t flags;
    uint32_t rope;                    /* floats of rotary coefficients that follow */
    uint32_t dim;                     /* floats of residual stream after them */
    uint8_t text_len, reserved[3];
    char text[16];                    /* the input token's text, for a screen */
    char status[24];                  /* the coordinator's speed and count, for a screen */
    struct llm_trace trace[LLM_MAX_STAGES];
};

static inline uint32_t llm_msg_bytes(uint32_t rope, uint32_t dim) {
    return (uint32_t)sizeof(struct llm_msg) + 4u * (rope + dim);
}
static inline float* llm_msg_floats(struct llm_msg* m) { return (float*)(m + 1); }

/* ---- arithmetic -------------------------------------------------------- */

/* e^x as the node's k32_softmax_f32 computes it (k32_expf) */
__attribute__((noinline, unused)) static float llm_exp(float x) { return k32_expf(x); }

/* ---- text -------------------------------------------------------------- */
static inline uint32_t llm_strlen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static inline int llm_eq(const char* a, uint32_t an, const char* b) {
    uint32_t i = 0;
    for (; i < an && b[i]; i++) if (a[i] != b[i]) return 0;
    return i == an && !b[i];
}

/* Environment value into out (NUL terminated) and its length; 0 and out
 * as it was when the variable is unset, empty or too long */
static inline uint32_t llm_env(const char* key, char* out, uint32_t cap) {
    char tmp[160];
    if (cap > sizeof tmp) cap = sizeof tmp;
    int32_t n = k32_env_get(key, llm_strlen(key), tmp, cap - 1);
    if (n <= 0 || (uint32_t)n >= cap) return 0;
    for (int32_t i = 0; i < n; i++) out[i] = tmp[i];
    out[n] = 0;
    return (uint32_t)n;
}

/* path ends with s ("/llm/generate" ends with "/generate") */
static inline int llm_ends(const char* path, uint32_t n, const char* s) {
    const uint32_t sn = llm_strlen(s);
    return n >= sn && llm_eq(path + n - sn, sn, s);
}

static inline uint32_t llm_atou(const char* s, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (uint32_t)(s[i] - '0');
    return v;
}

/* The path of a request block's first line ("POST /x HTTP" -> "/x"), and
 * where its body starts */
static inline uint32_t llm_request(const char* req, uint32_t len, const char** path, uint32_t* body) {
    uint32_t i = 0;
    while (i < len && req[i] != ' ') i++;
    const uint32_t p = ++i;
    while (i < len && req[i] != ' ' && req[i] != '\n' && req[i] != '?') i++;
    *path = req + p;
    const uint32_t plen = i - p;
    /* the body follows the first empty line */
    *body = len;
    for (uint32_t k = 0; k + 1 < len; k++) {
        if (req[k] == '\n' && req[k + 1] == '\n') { *body = k + 2; break; }
    }
    return plen;
}

#endif
