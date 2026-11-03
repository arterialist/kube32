/* llm-coordinator: runs generations through the llm-shard pipeline.
 *
 * POST /generate takes a prompt, encodes it with the model's tokenizer
 * (fetched from llm-store at start) and feeds its tokens to the first stage
 * (SHARD_URL), several positions in flight so the stages work on the prompt
 * at the same time. The last stage posts each position back to
 * POST /result; for the last position it carries the token the model
 * picked, which the coordinator feeds next, until STEPS tokens, the context
 * or an end-of-text token. Each position carries its token's text and the
 * speed so far for a stage with a screen, and each token goes out as an
 * event on TOPIC (Server-Sent Events through an Ingress). The context is
 * CONTEXT or what the smallest stage cache holds, as the traces report.
 *
 * When no position comes back for STALL_MS (a stage restarted and lost its
 * cache), or the first stage answers 409, the coordinator replays the
 * prompt and the tokens so far in a new session; greedy decoding makes the
 * replay pick the same tokens again.
 *
 * Environment:
 *   SHARD_URL   the first stage (default http://llm-shard-0/x)
 *   STORE_URL   llm-store (default http://llm-store); TOKENIZER its file
 *   TOPIC       event topic (default llm.text)
 *   CONTEXT     positions per generation at most (default 128, up to 512)
 *   STEPS       tokens to generate when a request names none (default 96)
 *   PROMPT      generate this once the tokenizer is loaded (optional, up to
 *               159 bytes; POST /generate takes up to 512)
 *   STALL_MS    replay after this long without progress (default 10000)
 *
 *   POST /generate   {"prompt":"Once upon a time","steps":64} (or the prompt as text)
 *   GET  /           state, tokens, text, speed and per-stage timing (JSON)
 *   GET  /text       the text so far */
#include "tokenizer.h"

#define CTX_MAX 512
#define TOK_BYTES_MAX 16384
#define WINDOW 4
#define RETRY_MS 200
#define MAX_REPLAYS 12

extern unsigned char __heap_base;

static unsigned char g_block[2048];
static char g_shard[128] = "http://llm-shard-0/x";
static char g_store[96] = "http://llm-store";
static char g_tok_file[32] = "tokenizer.bin";
static char g_topic[48] = "llm.text";
static uint32_t g_shard_len, g_topic_len, g_ctx_limit = 128, g_default_steps = 96, g_stall_ms = 10000;
static char g_line[192];
static uint32_t g_line_len;

/* tokenizer */
static uint32_t g_tok_loaded, g_tok_at;

/* the generation */
enum { IDLE, RUNNING, DONE, FAILED };
static const char* const kStates[] = {"idle", "running", "done", "failed"};
static uint32_t g_state, g_session, g_replays, g_stalls, g_stage_ctx, g_shown, g_tail;
static uint16_t g_toks[CTX_MAX];
static uint32_t g_ntok, g_nprompt, g_steps, g_ctx, g_feed, g_done_pos, g_retry_at, g_last_progress;
static uint32_t g_sent_ms[CTX_MAX];
static uint32_t g_t_start, g_t_first, g_t_last;
static char g_prompt[512];
static uint32_t g_prompt_len, g_autostart;
static char g_text[4096];
static uint32_t g_text_len;

/* timing of generated tokens: per stage, and the whole round */
struct stage_stats {
    uint32_t used, layer_begin, layer_end, n;
    uint64_t wait_us, compute_us, call_us;
    char node[LLM_NODE_CHARS + 1];
};
static struct stage_stats g_stats[LLM_MAX_STAGES];
static uint64_t g_lat_sum, g_call_us;
static uint32_t g_lat_n, g_lat_min, g_lat_max, g_calls;

/* ---- text ---------------------------------------------------------------- */
static void put_n(const char* s, uint32_t n) { for (uint32_t i = 0; i < n && g_line_len < sizeof g_line; i++) g_line[g_line_len++] = s[i]; }
static void put(const char* s) { put_n(s, llm_strlen(s)); }
static void put_u(uint32_t v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put_n(&t[--n], 1);
}
static void put_i(int32_t v) { if (v < 0) { put("-"); v = -v; } put_u((uint32_t)v); }
/* v / 10^d with d decimals */
static void put_fixed(uint64_t v, uint32_t d) {
    uint64_t p = 1;
    for (uint32_t i = 0; i < d; i++) p *= 10;
    put_u((uint32_t)(v / p));
    if (!d) return;
    put(".");
    uint64_t r = v % p;
    for (uint64_t q = p / 10; q; q /= 10) { char c = (char)('0' + (r / q) % 10); put_n(&c, 1); }
}
static void put_esc(const char* s, uint32_t n) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = (uint8_t)s[i];
        if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; put_n(e, 2); }
        else if (c == '\n') put("\\n");
        else if (c < 0x20 || c == 0x7f) { char e[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]}; put_n(e, 6); }
        else put_n((const char*)&c, 1);
    }
}
static void log_line(void) { k32_log(g_line, g_line_len); }
static void out(void) { k32_resp_write(g_line, g_line_len); g_line_len = 0; }
static uint32_t now_ms(void) { return (uint32_t)k32_time_ms(); }

/* ---- tokenizer ----------------------------------------------------------- */
/* tokenizer.bin from llm-store, parsed where it lands (tokenizer.h) */
static void load_tokenizer(void) {
    uint8_t* raw = (uint8_t*)(((uintptr_t)&__heap_base + 15u) & ~(uintptr_t)15u);
    char url[160];
    uint32_t ul = 0, n = 0;
    for (uint32_t i = 0; g_store[i] && ul < 120; i++) url[ul++] = g_store[i];
    url[ul++] = '/';
    for (uint32_t i = 0; g_tok_file[i] && ul < sizeof url; i++) url[ul++] = g_tok_file[i];
    int32_t rc = k32_http_call("GET", 3, url, ul, "", 0, 0, 0, raw, TOK_BYTES_MAX, &n, 10000);
    g_line_len = 0;
    put("llm-coordinator: ");
    if (rc != 200) {
        put("fetching "); put_n(url, ul); put(": "); put_i(rc);
        log_line();
        return;
    }
    if (!tok_parse(raw, n)) {
        put("bad tokenizer "); put_n(url, ul);
        log_line();
        return;
    }
    g_tok_loaded = 1;
    put("tokenizer: "); put_u(g_vocab); put(" tokens");
    log_line();
}

/* ---- events -------------------------------------------------------------- */
static void emit(void) { k32_event_emit(g_topic, g_topic_len, g_line, g_line_len); }

static uint32_t tokens_per_s_x100(void) {
    const uint32_t gen = g_ntok - g_nprompt;
    if (gen < 2 || g_t_last == g_t_first) return 0;
    return (uint32_t)((uint64_t)(gen - 1) * 100000u / (g_t_last - g_t_first));
}

static void emit_text(const char* kind, const char* s, uint32_t n) {
    g_line_len = 0;
    put("{\"t\":\"token\",\"s\":"); put_u(g_session);
    put(",\"k\":\""); put(kind); put("\",\"n\":"); put_u(g_ntok - g_nprompt);
    put(",\"r\":"); put_u(tokens_per_s_x100());
    put(",\"x\":\""); put_esc(s, n); put("\"}");
    emit();
}

/* Stage compute times for the display's status line: "L0-2 118|L3 41|L4 2" */
static void emit_stats(void) {
    g_line_len = 0;
    put("{\"t\":\"stats\",\"s\":"); put_u(g_session);
    put(",\"r\":"); put_u(tokens_per_s_x100());
    put(",\"st\":\"");
    for (uint32_t i = 0; i < LLM_MAX_STAGES; i++) {
        const struct stage_stats* st = &g_stats[i];
        if (!st->used || !st->n) continue;
        if (i) put("|");
        put("L"); put_u(st->layer_begin);
        if (st->layer_end - 1 > st->layer_begin) { put("-"); put_u(st->layer_end - 1); }
        put(" "); put_u((uint32_t)(st->compute_us / st->n / 1000));
    }
    put("\"}");
    emit();
}

static void text_add(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n && g_text_len < sizeof g_text; i++) g_text[g_text_len++] = s[i];
}

/* ---- generation ---------------------------------------------------------- */
static uint32_t new_session(void) {
    uint32_t s;
    do s = k32_random() & 0x7fffffffu; while (!s || s == g_session);
    return s;
}

static void replay(const char* why) {
    if (++g_stalls > MAX_REPLAYS) {
        g_state = FAILED;
        g_line_len = 0;
        put("llm-coordinator: "); put(why); put(" after "); put_u(MAX_REPLAYS); put(" replays: giving up");
        log_line();
        return;
    }
    g_session = new_session();
    g_feed = 0;
    g_done_pos = 0;
    g_replays++;
    g_last_progress = now_ms();
    g_line_len = 0;
    put("llm-coordinator: "); put(why); put(": replaying "); put_u(g_ntok); put(" tokens in session "); put_u(g_session);
    log_line();
}

static int start(const char* prompt, uint32_t n, uint32_t steps) {
    if (!g_tok_loaded) return 503;
    if (n > sizeof g_prompt) n = sizeof g_prompt;
    g_ctx = g_stage_ctx && g_stage_ctx < g_ctx_limit ? g_stage_ctx : g_ctx_limit;
    g_nprompt = tok_encode(prompt, n, g_toks, g_ctx);
    if (g_nprompt >= g_ctx) return 413;
    for (uint32_t i = 0; i < n; i++) g_prompt[i] = prompt[i];
    g_prompt_len = n;
    g_ntok = g_nprompt;
    g_steps = steps;
    g_session = new_session();
    g_feed = g_done_pos = 0;
    g_shown = LLM_NO_TOKEN;
    g_state = RUNNING;
    g_replays = g_stalls = 0;
    g_t_start = g_last_progress = now_ms();
    g_t_first = g_t_last = 0;
    g_text_len = 0;
    for (uint32_t i = 0; i < LLM_MAX_STAGES; i++) g_stats[i].used = g_stats[i].n = 0;
    for (uint32_t i = 0; i < LLM_MAX_STAGES; i++) g_stats[i].wait_us = g_stats[i].compute_us = g_stats[i].call_us = 0;
    g_lat_sum = g_call_us = 0;
    g_lat_n = g_lat_max = g_calls = 0;
    g_lat_min = 0xffffffffu;
    g_line_len = 0;
    put("{\"t\":\"start\",\"s\":"); put_u(g_session); put(",\"c\":"); put_u(g_ctx); put("}");
    emit();
    char pc[256];
    uint32_t pn = 0;
    for (uint32_t i = 1; i < g_nprompt; i++) {
        uint32_t m = tok_piece(g_toks[i - 1], g_toks[i], pc + pn);
        text_add(pc + pn, m);
        pn += m;
        if (pn >= 40 || i + 1 == g_nprompt) {
            emit_text("p", pc, pn);
            pn = 0;
        }
    }
    g_line_len = 0;
    put("llm-coordinator: session "); put_u(g_session); put(", "); put_u(g_nprompt); put(" prompt tokens, ");
    put_u(steps); put(" to generate");
    log_line();
    return 200;
}

static void finish(const char* why) {
    g_state = DONE;
    /* the last token has no position of its own: one more, for the screen */
    g_tail = g_feed + 1 == g_ntok && g_ntok > g_nprompt;
    g_line_len = 0;
    put("{\"t\":\"done\",\"s\":"); put_u(g_session); put(",\"n\":"); put_u(g_ntok - g_nprompt);
    put(",\"r\":"); put_u(tokens_per_s_x100()); put("}");
    emit();
    emit_stats();
    g_line_len = 0;
    put("llm-coordinator: done ("); put(why); put("), "); put_u(g_ntok - g_nprompt); put(" tokens, ");
    put_fixed(tokens_per_s_x100(), 2); put(" tokens/s");
    log_line();
}

/* One position back from the last stage */
static void result(const struct llm_msg* m) {
    if (g_state != RUNNING || m->session != g_session || m->pos != g_done_pos) return;
    const uint32_t now = now_ms();
    g_done_pos = m->pos + 1;
    g_last_progress = now;
    g_stalls = 0;
    /* The context is what the smallest stage cache holds */
    uint32_t cap = 0;
    for (uint32_t i = 0; i < LLM_MAX_STAGES; i++) {
        const uint32_t c = m->trace[i].used ? m->trace[i].context : 0;
        if (c && (!cap || c < cap)) cap = c;
    }
    if (cap) {
        g_stage_ctx = cap;
        if (cap < g_ctx) g_ctx = cap;
        if (g_nprompt >= g_ctx) { finish("the prompt does not fit the context"); return; }
    }
    if (!(m->flags & LLM_MSG_SAMPLE)) return;
    for (uint32_t i = 0; i < LLM_MAX_STAGES; i++) {
        const struct llm_trace* t = &m->trace[i];
        if (!t->used) continue;
        struct stage_stats* st = &g_stats[i];
        st->used = 1;
        st->layer_begin = t->layer_begin;
        st->layer_end = t->layer_end;
        for (uint32_t c = 0; c < LLM_NODE_CHARS; c++) st->node[c] = t->node[c];
        st->node[LLM_NODE_CHARS] = 0;
        st->n++;
        st->wait_us += t->wait_us;
        st->compute_us += t->compute_us;
        st->call_us += t->call_us;
    }
    const uint32_t lat = now - g_sent_ms[m->pos];
    g_lat_sum += lat;
    g_lat_n++;
    if (lat < g_lat_min) g_lat_min = lat;
    if (lat > g_lat_max) g_lat_max = lat;
    const uint32_t tok = m->next;
    if (tok >= g_vocab) { finish("bad token"); return; }
    if (g_ntok == g_nprompt) g_t_first = now;
    g_t_last = now;
    if (tok == LLM_BOS) { finish("end of text"); return; }
    g_toks[g_ntok++] = (uint16_t)tok;
    char pc[64];
    const uint32_t pn = tok_piece(g_toks[g_ntok - 2], tok, pc);
    text_add(pc, pn);
    emit_text("g", pc, pn);
    if (((g_ntok - g_nprompt) & 3) == 0) emit_stats();
    if (g_ntok - g_nprompt >= g_steps) finish("steps");
    else if (g_ntok >= g_ctx) finish("context full");
}

/* Position pos to the first stage with flags (SAMPLE for the last token);
 * a stage with a screen shows each position's token once */
static int32_t post(uint32_t pos, uint32_t flags, uint32_t* us) {
    struct llm_msg m;
    __builtin_memset(&m, 0, sizeof m);
    m.magic = LLM_MSG_MAGIC;
    m.session = g_session;
    m.pos = pos;
    m.token = g_toks[pos];
    m.next = LLM_NO_TOKEN;
    m.flags = flags;
    if (g_shown == LLM_NO_TOKEN || pos > g_shown) {
        m.flags |= LLM_MSG_SHOW | (pos < g_nprompt ? LLM_MSG_PROMPT : 0) | (pos == 0 ? LLM_MSG_CLEAR : 0);
        if (pos) {
            char pc[64];
            const uint32_t n = tok_piece(g_toks[pos - 1], g_toks[pos], pc);
            m.text_len = (uint8_t)(n < sizeof m.text ? n : sizeof m.text);
            __builtin_memcpy(m.text, pc, m.text_len);
        }
    }
    /* "3.21 tok/s  42 tokens" */
    g_line_len = 0;
    put_fixed(tokens_per_s_x100(), 2); put(" tok/s  "); put_u(g_ntok - g_nprompt); put(" tokens");
    __builtin_memcpy(m.status, g_line, g_line_len < sizeof m.status ? g_line_len : sizeof m.status);
    char resp[96];
    uint32_t got = 0;
    const uint32_t t0 = (uint32_t)esp32_get_time_us();
    const int32_t rc = k32_http_call("POST", 4, g_shard, g_shard_len, "Content-Type: application/octet-stream\r\n", 40,
                                     (const uint8_t*)&m, sizeof m, (uint8_t*)resp, sizeof resp, &got, 5000);
    *us = (uint32_t)esp32_get_time_us() - t0;
    if (rc == 200 && (m.flags & LLM_MSG_SHOW)) g_shown = pos;
    return rc;
}

static void feed(uint32_t now) {
    while (g_state == RUNNING && g_feed < g_ntok && g_feed - g_done_pos < WINDOW && (int32_t)(now - g_retry_at) >= 0) {
        const uint32_t flags = g_feed + 1 == g_ntok ? LLM_MSG_SAMPLE : 0;
        uint32_t dt;
        const int32_t rc = post(g_feed, flags, &dt);
        if (rc == 200) {
            g_sent_ms[g_feed] = now_ms();
            if (flags) { g_call_us += dt; g_calls++; }
            g_feed++;
            continue;
        }
        if (rc == 409) { replay("the first stage lost the session"); return; }
        if (rc == 413) { finish("context full"); return; }
        g_retry_at = now + RETRY_MS;
        if (rc != 503) {
            g_line_len = 0;
            put("llm-coordinator: position "); put_u(g_feed); put(": "); put_i(rc); put(", retrying");
            log_line();
        }
        return;
    }
}

/* ---- exports ------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char num[12];
    uint32_t n = llm_env("SHARD_URL", g_shard, sizeof g_shard);
    g_shard_len = n ? n : llm_strlen(g_shard);
    llm_env("STORE_URL", g_store, sizeof g_store);
    llm_env("TOKENIZER", g_tok_file, sizeof g_tok_file);
    llm_env("TOPIC", g_topic, sizeof g_topic);
    g_topic_len = llm_strlen(g_topic);
    if ((n = llm_env("CONTEXT", num, sizeof num))) g_ctx_limit = llm_atou(num, n);
    if (g_ctx_limit < 8) g_ctx_limit = 8;
    if (g_ctx_limit > CTX_MAX) g_ctx_limit = CTX_MAX;
    if ((n = llm_env("STEPS", num, sizeof num))) g_default_steps = llm_atou(num, n);
    if ((n = llm_env("STALL_MS", num, sizeof num))) g_stall_ms = llm_atou(num, n);
    if (g_stall_ms < 1000) g_stall_ms = 1000;
    g_prompt_len = llm_env("PROMPT", g_prompt, sizeof g_prompt);
    g_autostart = g_prompt_len > 0;
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    if (!g_tok_loaded) {
        if ((int32_t)(uptime_ms - g_tok_at) >= 0) {
            load_tokenizer();
            g_tok_at = uptime_ms + 2000;
        }
        if (!g_tok_loaded) return 0;
    }
    if (g_autostart) {
        g_autostart = 0;
        char p[sizeof g_prompt];
        for (uint32_t i = 0; i < g_prompt_len; i++) p[i] = g_prompt[i];
        start(p, g_prompt_len, g_default_steps);
    }
    if (g_tail) {
        uint32_t dt;
        g_tail = 0;
        if (g_ntok <= g_ctx) post(g_ntok - 1, 0, &dt);
    }
    if (g_state != RUNNING) return 0;
    const uint32_t now = now_ms();   /* the clock the results are timed with */
    if (g_feed > g_done_pos && now - g_last_progress > g_stall_ms) replay("no progress");
    feed(now);
    return 1;
}

/* A JSON string's contents, written out in pieces that fit the line */
static void text_out(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i += 24) {
        put_esc(s + i, n - i < 24 ? n - i : 24);
        out();
    }
}

static void status(void) {
    k32_resp_begin(200, "application/json", 16);
    g_line_len = 0;
    put("{\"state\":\""); put(kStates[g_state]);
    put("\",\"session\":"); put_u(g_session);
    put(",\"prompt\":\"");
    out();
    text_out(g_prompt, g_prompt_len);
    put("\",\"prompt_tokens\":"); put_u(g_nprompt);
    put(",\"generated\":"); put_u(g_ntok > g_nprompt ? g_ntok - g_nprompt : 0);
    put(",\"context\":"); put_u(g_ctx);
    put(",\"replays\":"); put_u(g_replays);
    out();
    put(",\"tokens\":[");
    for (uint32_t i = 0; i < g_ntok; i++) {
        if (i) put(",");
        put_u(g_toks[i]);
        if (g_line_len > sizeof g_line - 8) out();
    }
    out();
    put("],\"text\":\"");
    out();
    text_out(g_text, g_text_len);
    put("\",\"prefill_ms\":"); put_u(g_t_first ? g_t_first - g_t_start : 0);
    put(",\"tokens_per_s\":"); put_fixed(tokens_per_s_x100(), 2);
    put(",\"token_ms\":{\"mean\":"); put_fixed(g_lat_n ? g_lat_sum * 10 / g_lat_n : 0, 1);
    put(",\"min\":"); put_u(g_lat_n ? g_lat_min : 0); put(",\"max\":"); put_u(g_lat_max);
    put("},\"first_hop_ms\":"); put_fixed(g_calls ? g_call_us / g_calls / 100 : 0, 1);
    put(",\"stages\":[");
    out();
    uint32_t first = 1;
    for (uint32_t i = 0; i < LLM_MAX_STAGES; i++) {
        const struct stage_stats* st = &g_stats[i];
        if (!st->used || !st->n) continue;
        if (!first) put(",");
        first = 0;
        put("{\"stage\":"); put_u(i);
        put(",\"layers\":["); put_u(st->layer_begin); put(","); put_u(st->layer_end);
        put("],\"node\":\""); put_esc(st->node, llm_strlen(st->node));
        put("\",\"compute_ms\":"); put_fixed(st->compute_us / st->n / 100, 1);
        put(",\"wait_ms\":"); put_fixed(st->wait_us / st->n / 100, 1);
        put(",\"hop_ms\":"); put_fixed(st->call_us / st->n / 100, 1);
        put("}");
        out();
    }
    put("]}");
    out();
    k32_resp_end();
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    const char* path;
    uint32_t body;
    const uint32_t plen = llm_request(req, len, &path, &body);
    const char* b = req + body;
    const uint32_t blen = len - body;
    if (req[0] == 'P' && llm_ends(path, plen, "/result")) {
        if (blen >= sizeof(struct llm_msg) && ((const struct llm_msg*)b)->magic == LLM_MSG_MAGIC) {
            struct llm_msg m;
            __builtin_memcpy(&m, b, sizeof m);
            result(&m);
            k32_resp_begin(200, "application/json", 16);
            k32_resp_write("{\"ok\":true}", 11);
            k32_resp_end();
            return 200;
        }
        k32_resp_begin(400, "application/json", 16);
        k32_resp_end();
        return 400;
    }
    if (req[0] == 'P' && llm_ends(path, plen, "/generate")) {
        const char* prompt = b;
        uint32_t pn = blen, steps = g_default_steps;
        /* {"prompt":"...","steps":N}: the two fields, \" \\ \n escapes */
        char unq[sizeof g_prompt];
        if (blen && b[0] == '{') {
            pn = 0;
            prompt = unq;
            for (uint32_t i = 0; i + 9 < blen; i++) {
                if (b[i] == '"' && llm_eq(b + i, 9, "\"prompt\":")) {
                    uint32_t j = i + 9;
                    while (j < blen && b[j] == ' ') j++;
                    for (j++; j < blen && b[j] != '"' && pn < sizeof unq; j++) {
                        char c = b[j];
                        if (c == '\\' && j + 1 < blen) { c = b[++j]; if (c == 'n') c = '\n'; }
                        unq[pn++] = c;
                    }
                }
                if (b[i] == '"' && llm_eq(b + i, 8, "\"steps\":")) {
                    uint32_t j = i + 8;
                    while (j < blen && b[j] == ' ') j++;
                    steps = llm_atou(b + j, blen - j);
                }
            }
        }
        while (pn && (prompt[pn - 1] == '\n' || prompt[pn - 1] == '\r')) pn--;
        const int rc = start(prompt, pn, steps ? steps : g_default_steps);
        k32_resp_begin(rc, "application/json", 16);
        g_line_len = 0;
        if (rc == 200) {
            put("{\"session\":"); put_u(g_session); put(",\"prompt_tokens\":"); put_u(g_nprompt);
            put(",\"steps\":"); put_u(g_steps); put(",\"context\":"); put_u(g_ctx); put("}");
        } else {
            put(rc == 503 ? "{\"error\":\"the tokenizer is not loaded yet\"}" : "{\"error\":\"the prompt does not fit the context\"}");
        }
        out();
        k32_resp_end();
        return rc;
    }
    if (req[0] == 'G' && llm_ends(path, plen, "/text")) {
        k32_resp_begin(200, "text/plain; charset=utf-8", 25);
        k32_resp_write(g_text, g_text_len);
        k32_resp_write("\n", 1);
        k32_resp_end();
        return 200;
    }
    if (req[0] == 'G') {
        status();
        return 200;
    }
    k32_resp_begin(404, "application/json", 16);
    k32_resp_end();
    return 404;
}
