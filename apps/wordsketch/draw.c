/* wordsketch-draw: the doodle of a word, step by step.
 *
 * An entry-point pod: it runs on its own thread of a board with PSRAM, so
 * a drawing computes beside the node's request pods (the story) instead of
 * taking turns with them on the shared interpreter. It waits on the channel
 * wordsketch.word for "W <id> <class> <seed> <steps>" and samples the class with models/doodle/doodle_infer.c (DDIM, K = 8 or
 * 16 steps, classifier-free guidance GUIDANCE). The starting noise and the
 * image after every step go out on wordsketch.frame, four events of 7 rows
 * each:
 *
 *   'F', 'a' + id % 16, 'a' + step, '0' + part, 196 levels '0' + 0..63
 *
 * a level being the pixel's value in [-1, 1] at 64 steps (-1 is paper).
 *
 * The weights stay in the blob wordsketch-doodle (doodle-l.bin, flash) and
 * k32_matvec_i8 reads them in place; scales, biases and the step and class
 * tables come into memory.
 *
 * Environment: GUIDANCE in tenths (default 20). */
#include "../../include/runtime/k32_app_abi.h"
#include "../../models/doodle/doodle_infer.h"
#include "blobload.h"

#define NOINLINE __attribute__((noinline))
#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

static struct doodle_model g_m;
static uint8_t g_arena[19348];   /* doodle-l.bin: everything but the weights */
static float g_x[784];
static char g_ev[200];
static uint32_t g_id;
static uint64_t g_t_word, g_t_last;
static uint32_t g_step_ms_max;

static char g_out[200];
static uint32_t g_out_len;
static NOINLINE void put(const char* s) {
    while (*s && g_out_len < sizeof g_out) g_out[g_out_len++] = *s++;
}
static NOINLINE void put_i(int64_t v) {
    char d[20];
    int k = 0;
    if (v < 0) { put("-"); v = -v; }
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && g_out_len < sizeof g_out) g_out[g_out_len++] = d[--k];
}
static NOINLINE void say(void) {
    k32_log(g_out, g_out_len);
    g_out_len = 0;
}

/* doodle_load's walk over the blob, the convolution weights left in it */
static NOINLINE int32_t load_model(void) {
    const int32_t b = k32_blob_open(LIT("wordsketch-doodle"));
    if (b < 0) return b;
    if (k32_blob_mapped(b) != 1) return K32_ERR_NOTSUP;
    struct blobload r;
    blobload_begin(&r, b, (uint32_t)k32_blob_size(b), g_arena, sizeof g_arena);
    const uint32_t* h = blobload_copy(&r, 64);
    if (!h || h[0] != 0x5532334Bu || h[1] != 1 || h[9] != r.size || h[2] > DOODLE_C0_MAX || h[3] > DOODLE_C1_MAX ||
        h[5] > DOODLE_MAX_CONV || h[5] != 6 + h[4] || h[7] > 4)
        return K32_ERR_INVAL;
    g_m.c0 = h[2]; g_m.c1 = h[3]; g_m.mids = h[4]; g_m.n_conv = h[5];
    g_m.n_cond = h[6]; g_m.n_sched = h[7]; g_m.width = h[8];
    for (uint32_t i = 0; i < g_m.n_conv; i++) {
        struct doodle_conv* c = &g_m.conv[i];
        const uint32_t* d = blobload_copy(&r, 16);
        const float* s = blobload_copy(&r, 8);
        if (!d || !s) return K32_ERR_INVAL;
        c->cin = d[0]; c->cout = d[1]; c->stride = d[2]; c->flags = d[3];
        c->scale = s[0];
        c->inv = s[1];
        c->ws = blobload_copy(&r, 4 * c->cout);
        c->b = blobload_copy(&r, 4 * c->cout);
        c->w = blobload_map(&r, c->cout * 9 * c->cin);
    }
    g_m.class_q = blobload_copy(&r, g_m.n_cond * g_m.width);
    g_m.class_s = blobload_copy(&r, 4 * g_m.n_cond);
    for (uint32_t k = 0; k < g_m.n_sched; k++) {
        struct doodle_sched* s = &g_m.sched[k];
        const uint32_t* n = blobload_copy(&r, 4);
        if (!n) return K32_ERR_INVAL;
        s->steps = *n;
        s->alpha = blobload_copy(&r, 4 * (s->steps + 1));
        s->sigma = blobload_copy(&r, 4 * (s->steps + 1));
        s->tq = blobload_copy(&r, s->steps * g_m.width);
        s->ts = blobload_copy(&r, 4 * s->steps);
    }
    return blobload_end(&r) ? K32_ERR_INVAL : 0;
}

/* the image so far as four events */
static NOINLINE void publish(uint32_t step, uint32_t steps, const float* x) {
    const uint64_t now = k32_time_ms();
    if (step) {
        const uint32_t ms = (uint32_t)(now - g_t_last);
        if (ms > g_step_ms_max) g_step_ms_max = ms;
    }
    g_t_last = now;
    for (uint32_t part = 0; part < 4; part++) {
        g_ev[0] = 'F';
        g_ev[1] = (char)('a' + (g_id & 15u));
        g_ev[2] = (char)('a' + step);
        g_ev[3] = (char)('0' + part);
        for (uint32_t i = 0; i < 196; i++) {
            float v = x[part * 196 + i];
            v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
            g_ev[4 + i] = (char)('0' + (int32_t)((v + 1.0f) * 31.5f + 0.5f));
        }
        k32_event_emit(LIT("wordsketch.frame"), g_ev, sizeof g_ev);
    }
    (void)steps;
}

K32_EXPORT("app_main") int32_t app_main(void) {
    char num[8];
    const int32_t gn = k32_env_get(LIT("GUIDANCE"), num, sizeof num);
    uint32_t g10 = 20;
    if (gn > 0) {
        g10 = 0;
        for (int32_t i = 0; i < gn && num[i] >= '0' && num[i] <= '9'; i++) g10 = g10 * 10u + (uint32_t)(num[i] - '0');
    }
    const float guidance = (float)g10 / 10.0f;
    const int32_t rc = load_model();
    put("wordsketch-draw: model ");
    if (rc) { put("not loaded: "); put_i(rc); say(); return 1; }
    put("loaded: "); put_i(g_m.c0); put("/"); put_i(g_m.c1); put(" channels, "); put_i(g_m.n_conv); put(" convolutions, guidance ");
    put_i(g10); put("/10");
    say();
    doodle_on_step = publish;
    const int32_t ch = kube32_channel_open("wordsketch.word", 0);
    if (ch < 0) { put("wordsketch-draw: channel wordsketch.word: "); put_i(ch); say(); return 1; }
    char msg[256];
    uint32_t last_id = 0;
    for (;;) {
        const int32_t n = kube32_channel_recv(ch, msg, sizeof msg, 5000);
        if (n <= 0) continue;
        /* "W <id> <class> <seed> <steps>" */
        uint32_t v[4] = {0, 99, 1, 16}, k = 0;
        for (int32_t i = 1; i < n && k < 4; k++) {
            while (i < n && msg[i] == ' ') i++;
            if (i < n) v[k] = 0;
            while (i < n && msg[i] >= '0' && msg[i] <= '9') v[k] = v[k] * 10u + (uint32_t)(msg[i++] - '0');
        }
        const uint32_t id = v[0], cls = v[1], seed = v[2], steps = v[3];
        if (msg[0] != 'W' || !id || id == last_id || cls >= DOODLE_NONE) continue;
        last_id = id;
        g_id = id;
        g_step_ms_max = 0;
        g_t_word = k32_time_ms();
        const int32_t src = doodle_sample(&g_m, cls, seed, steps, guidance, g_x);
        const uint64_t t1 = k32_time_ms();
        /* a hash of the image's bits, to compare with the reference */
        uint32_t hsh = 2166136261u;
        const uint8_t* xb = (const uint8_t*)g_x;
        for (uint32_t i = 0; i < sizeof g_x; i++) hsh = (hsh ^ xb[i]) * 16777619u;
        put("wordsketch-draw: word "); put_i(id); put(" class "); put_i(cls); put(" seed "); put_i(seed);
        if (src) { put(": no schedule of "); put_i(steps); put(" steps"); say(); continue; }
        put(": "); put_i(steps); put(" steps in "); put_i((int64_t)(t1 - g_t_word)); put(" ms, slowest step ");
        put_i(g_step_ms_max); put(" ms, image fnv1a 0x");
        for (int s = 28; s >= 0; s -= 4) { const char hx[] = "0123456789abcdef"; char c[2] = {hx[(hsh >> s) & 15u], 0}; put(c); }
        say();
    }
}
