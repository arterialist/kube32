/*
 * Guest entry points: k32_init / k32_alloc / k32_handle / k32_tick / k32_event.
 */
#include "app.h"
#include "k32_host.h"
#include "store.h"
#include "seed.h"
#include "index.h"
#include "router.h"

static u8 g_arena_mem[TESSERA_ARENA_BYTES] __attribute__((aligned(16)));
_Static_assert(TESSERA_ARENA_BYTES % 16 == 0, "the posting block walk needs a 4-byte aligned arena end");

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

/* Seeds one chunk and tells SSE clients / the HUD about it. */
static int seed_chunk(void) {
    u32 before = g.issue_count;
    int rc = seed_step(g.seed_chunk);
    if (rc < 0) { logf("tessera: seed failed rc=", rc); g.seed.phase = SEED_FAILED; return 0; }   /* stop; stats keeps showing done < total */
    if (g.issue_count > before) {
        /* one create event per chunk: the HUD bumps its counter and web views refetch */
        const issue_t* last = issue_slot(g.issue_count);
        events_issue("create", g.issue_count, last ? last->reporter : 0);
    }
    if (rc == 0) {
        logf("tessera: seeded issues=", g.issue_live);
        logf("tessera: comments=", g.comment_live);
        logf("tessera: terms=", g.term_count);
        logf("tessera: postings=", g.posting_count);
        logf("tessera: arena_used=", arena_used());
    }
    return rc;
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
    k32_arena_init(&g.arena, g_arena_mem, TESSERA_ARENA_BYTES);
    /* reserve the first bytes so arena offset 0 never names a live string */
    k32_arena_alloc(&g.arena, 16, 16);

    i64 seed_issues = env_i64("SEED_ISSUES", 3000);
    i64 seed_comments = env_i64("SEED_COMMENTS", 6000);
    i64 max_issues = env_i64("MAX_ISSUES", seed_issues + 500);
    i64 epoch = env_i64("EPOCH_BASE_MS", 1793059200000ll);
    i64 max_terms = env_i64("MAX_TERMS", 1024);
    i64 seed_chunk = env_i64("SEED_CHUNK", 10);
    if (seed_chunk < 1) seed_chunk = 1;
    if (seed_chunk > 60000) seed_chunk = 60000;
    if (seed_issues < 1) seed_issues = 1;
    if (seed_issues > 60000) seed_issues = 60000;
    if (seed_comments < 0) seed_comments = 0;
    if (seed_comments > 60000) seed_comments = 60000;
    if (max_issues < seed_issues) max_issues = seed_issues;
    if (max_issues > 60000) max_issues = 60000;
    if (max_terms < 256) max_terms = 256;
    if (max_terms > 30000) max_terms = 30000;
    i64 max_comments = seed_comments + 500;
    if (max_comments > 60000) max_comments = 60000;
    if (max_comments < 1) max_comments = 1;

    g.epoch_base = epoch;
    g.ts_base = epoch - 120 * T_DAY_MS;
    g.seed_issues = (u32)seed_issues; g.seed_comments = (u32)seed_comments; g.seed_chunk = (u32)seed_chunk;
    if (store_init((u32)max_issues, (u32)max_comments, (u32)max_terms, TESSERA_SCRATCH_BYTES) < 0) {
        k32_log("tessera: store_init failed (arena too small)", 44);
        return -1;
    }
    /* Seed relative to the synthetic epoch so the data set is byte-for-byte
     * deterministic. Only the first chunk is seeded here; k32_tick continues
     * (returning 1 while work remains) so the pod serves requests meanwhile. */
    seed_begin((u32)seed_issues, (u32)seed_comments, epoch);
    int rc = seed_step(g.seed_chunk);
    if (rc < 0) { logf("tessera: seed failed rc=", rc); return -2; }
    g.scratch_top = 0;
    g.scratch_fresh = 1;
    logf("tessera: ready issues=", g.issue_live);
    logf("tessera: seeding target=", g.seed_issues);
    logf("tessera: seed chunk=", g.seed_chunk);
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
        rq.uptime = t0; rq.t0_ms = t0; rq.now = g.epoch_base + t0;
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
    if (!seed_active()) return 0;
    g.scratch_top = 0;                 /* no request buffer is live between calls */
    int more = seed_chunk();
    g.scratch_top = 0;
    g.scratch_fresh = 1;
    return more > 0 ? 1 : 0;
}

/* ---- keyboard input path ------------------------------------------------- */
static void note_commit(void) {
    if (g.note_len == 0) return;
    u32 target = g.last_viewed && issue_by_id(g.last_viewed) ? g.last_viewed : 0;
    if (!target) target = issue_by_id(1) ? 1 : first_live_issue();
    if (target) issue_add_comment_indexed(target, 0, g.note, g.note_len, store_now());
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
    g.scratch_fresh = 1;   /* event buffers were copied out; let the next k32_alloc start over */
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
