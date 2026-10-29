/*
 * Guest entry points: k32_init / k32_alloc / k32_handle / k32_tick / k32_event.
 */
#include "app.h"
#include "store.h"
#include "index.h"
#include "router.h"
#include "storage.h"
#include "../../common/k32_host.h"

static u8 g_arena_mem[TESSERA_ARENA_BYTES] __attribute__((aligned(16)));
_Static_assert(TESSERA_ARENA_BYTES % 16 == 0, "the posting block walk needs a 4-byte aligned arena end");

static struct { u32 issues, comments, terms; } g_caps;

static void logf(const char* msg, i64 v) {
    char buf[96]; u32 n = (u32)strlen(msg);
    if (n > 72) n = 72;
    memcpy(buf, msg, n);
    n += k32_fmt_i64(buf + n, sizeof buf - n, v);
    k32_log(buf, n);
}
static i64 env_i64(const char* key, i64 dflt) {
    char buf[32];
    int32_t n = k32_env_get(key, (u32)strlen(key), buf, sizeof buf);
    i64 v;
    if (n <= 0 || n > (int32_t)sizeof buf || !k32_parse_i64(buf, (u32)n, &v)) return dflt;
    return v;
}
static i64 clamp(i64 v, i64 lo, i64 hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Carves the tables out of a fresh arena. */
static int reset_tables(void) {
    k32_arena_init(&g.arena, g_arena_mem, TESSERA_ARENA_BYTES);
    /* reserve the first bytes so arena offset 0 never names a live string */
    k32_arena_alloc(&g.arena, 16, 16);
    return store_init(g_caps.issues, g_caps.comments, g_caps.terms, TESSERA_SCRATCH_BYTES);
}
/* Empties the store for a new load (storage.c, after the log changed). */
void app_reset_store(void) {
    reset_tables();
    g.stg.state = STG_INIT;
    g.stg.seq = g.stg.target = 0;
    g.stg.page_len = g.stg.page_pos = 0;
    g.stg.damaged = 0;
    g.stg.retry_at = 0;
    g.scratch_top = 0;
    g.scratch_fresh = 1;
}

u8* tessera_resolve(u32 ptr) {
#if defined(__wasm32__)
    return (u8*)(uintptr_t)ptr;
#else
    return g.membase + ptr;
#endif
}
static u32 to_abi_ptr(const void* p) {
#if defined(__wasm32__)
    return (u32)(uintptr_t)p;
#else
    return (u32)((const u8*)p - g.membase);
#endif
}

int32_t k32_init(void) {
    memset(&g, 0, sizeof g);
#if defined(__wasm32__)
    g.membase = 0;
#else
    g.membase = g_arena_mem;
#endif
    i64 epoch = env_i64("EPOCH_BASE_MS", 1793059200000ll);
    g_caps.issues = (u32)clamp(env_i64("MAX_ISSUES", 2900), 1, 60000);
    g_caps.comments = (u32)clamp(env_i64("MAX_COMMENTS", 5300), 1, 60000);
    g_caps.terms = (u32)clamp(env_i64("MAX_TERMS", 1024), 256, 30000);
    g.epoch_base = epoch;
    g.clock_base = epoch;
    g.ts_base = epoch - 120 * T_DAY_MS;
    if (reset_tables() < 0) {
        k32_log("tessera: store_init failed (arena too small)", 44);
        return -1;
    }
    /* The data comes from tessera-db: k32_tick loads it while requests are served. */
    storage_init();
    g.scratch_top = 0;
    g.scratch_fresh = 1;
    logf("tessera: capacity issues=", g_caps.issues);
    logf("tessera: capacity comments=", g_caps.comments);
    logf("tessera: arena free=", k32_arena_free(&g.arena));
    return 0;
}

uint32_t k32_alloc(uint32_t size) {
    if (g.scratch_fresh) { g.scratch_top = 0; g.scratch_fresh = 0; }
    void* p = scratch_alloc(size ? size : 1);
    if (!p) return 0;
    return to_abi_ptr(p);
}

int32_t k32_handle(uint32_t ptr, uint32_t len) {
    i64 t0 = k32_time_ms();
    u8* buf = tessera_resolve(ptr);
    /* keep the request buffer alive if it lives in scratch; reset everything after it */
    if (buf >= g.scratch && buf <= g.scratch + g.scratch_size) {
        u32 end = (u32)(buf - g.scratch) + len;
        if (end > g.scratch_size) { g.scratch_fresh = 1; return K32_HANDLE_ERR_BAD_REQUEST; }
        g.scratch_top = end;
    } else {
        g.scratch_top = 0;
    }
    g.scratch_fresh = 0;
    int status;
    req_t rq;
    if (len == 0 || len > K32_MAX_REQUEST_BYTES || router_parse((char*)buf, len, &rq) < 0) {
        memset(&rq, 0, sizeof rq);
        rq.now = store_now(); rq.uptime = t0; rq.t0_ms = t0; rq.user = -1;
        k32_resp_begin(400, "application/json", 16);
        const char* b = "{\"error\":\"bad_request\",\"message\":\"malformed request\"}";
        k32_resp_write(b, (u32)strlen(b));
        k32_resp_end();
        status = 400;
    } else {
        rq.uptime = t0; rq.t0_ms = t0; rq.now = g.clock_base + t0;
        status = router_dispatch(&rq);
    }
    i64 dur = k32_time_ms() - t0;
    if (dur < 0) dur = 0;
    stats_record_request((u32)(dur * 1000), status >= 400);
    g.scratch_fresh = 1;
    return status;
}

int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    session_expire(k32_time_ms());
    g.scratch_top = 0;                 /* no request buffer is live between calls */
    int more = storage_tick();
    g.scratch_top = 0;
    g.scratch_fresh = 1;
    return more > 0 ? 1 : 0;
}

/* ---- keyboard input path ------------------------------------------------- */
static void note_commit(void) {
    if (g.note_len == 0) return;
    u32 target = g.last_viewed && issue_by_id(g.last_viewed) ? g.last_viewed : 0;
    if (!target) target = issue_by_id(1) ? 1 : first_live_issue();
    if (!target) return;
    int cid = comment_commit(target, 0, g.note, g.note_len, store_now());
    /* the HUD clears its line on Enter either way; a note that could not be stored is dropped */
    if (cid < 0) logf("tessera: keypad comment not stored, status ", -cid);
    g.note_len = 0;
}
static void note_backspace(void) {
    if (g.note_len == 0) return;
    g.note_len--;
    while (g.note_len > 0 && ((u8)g.note[g.note_len] & 0xC0) == 0x80) g.note_len--;
}
static void note_append(const char* c, u32 len) {
    if (len == 0 || len > 4) return;
    if (g.note_len + len > T_NOTE_MAX) return;
    memcpy(g.note + g.note_len, c, len);
    g.note_len += len;
}

int32_t k32_event(uint32_t topic_ptr, uint32_t topic_len, uint32_t ptr, uint32_t len) {
    /* The host placed topic and payload with k32_alloc. Whatever this call
     * returns, the next k32_alloc starts a fresh scratch region; the buffers
     * stay readable until then. */
    g.scratch_fresh = 1;
    const char* topic = (const char*)tessera_resolve(topic_ptr);
    if (!k32_streq(topic, topic_len, "tessera.input")) return 0;
    if (len == 0 || len > 512) return K32_ERR_INVAL;
    char buf[512];
    memcpy(buf, tessera_resolve(ptr), len);
    k32_jfield f[6];
    int n = k32_json_parse_flat(buf, len, f, 6);
    if (n < 0) return K32_ERR_INVAL;
    const k32_jfield* t = k32_jf_get(f, n, "t");
    const k32_jfield* c = k32_jf_get(f, n, "c");
    if (t && t->type == K32_J_STR && !k32_streq(t->s, t->slen, "key")) return 0;
    if (!c || c->type != K32_J_STR) return K32_ERR_INVAL;
    const char* s = c->s; u32 sl = c->slen;
    if (k32_streq(s, sl, "\n") || k32_streq(s, sl, "\r") || k32_streq(s, sl, "Enter") || k32_streq(s, sl, "enter")) {
        note_commit();
    } else if (k32_streq(s, sl, "\b") || k32_streq(s, sl, "\x7f") || k32_streq(s, sl, "Backspace") || k32_streq(s, sl, "backspace")) {
        note_backspace();
    } else if (sl == 1 && ((u8)s[0] < 0x20)) {
        return 0;   /* other control characters are ignored */
    } else {
        note_append(s, sl);
    }
    events_hud(g.note, g.note_len);
    return 0;
}

void tessera_layout_get(tessera_layout* o) {
    memset(o, 0, sizeof *o);
    o->arena_size = g.arena.size; o->arena_used = arena_used();
    o->tables = g.fixed_bytes - TESSERA_SCRATCH_BYTES; o->scratch = TESSERA_SCRATCH_BYTES;
    o->strings = g.string_bytes; o->postings = g.posting_bytes; o->postings_leaked = g.posting_leaked;
    o->issue_cap = g.issue_cap; o->issue_count = g.issue_count;
    o->comment_cap = g.comment_cap; o->comment_count = g.comment_count;
    o->term_cap = g.term_cap; o->term_count = g.term_count; o->posting_count = g.posting_count;
    o->issue_rec = sizeof(issue_t); o->comment_rec = sizeof(comment_t); o->act_rec = sizeof(act_t);
    o->session_rec = sizeof(session_t); o->term_rec = sizeof(term_t);
}
