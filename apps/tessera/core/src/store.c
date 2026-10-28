#include "store.h"
#include "index.h"
#include "k32_host.h"

store_t g;

const user_t t_users[T_USERS_N] = {
    { "hud",    "HUD Keyboard",     "HK", "#8b8f9a" },
    { "mara",   "Mara Okafor",      "MO", "#f0a05a" },
    { "dimitri","Dimitri Volkov",   "DV", "#5b8def" },
    { "sana",   "Sana Iqbal",       "SI", "#3dbf8a" },
    { "leon",   "Leon Baptiste",    "LB", "#e5484d" },
    { "yuki",   "Yuki Tanaka",      "YT", "#b36ae2" },
    { "priya",  "Priya Raman",      "PR", "#e2b93b" },
    { "tomas",  "Tomas Lindqvist",  "TL", "#2bb7c9" },
    { "ines",   "Ines Carvalho",    "IC", "#f06292" },
};
const project_t t_projects[T_PROJECTS_N] = {
    { "TES", "Tessera Core",   "#5b8def" },
    { "HUD", "Tessera HUD",    "#3dbf8a" },
    { "ING", "Ingress & Link", "#f0a05a" },
};
const label_t t_labels[T_LABELS_N] = {
    { "bug", "#e5484d" }, { "feature", "#5b8def" }, { "perf", "#e2b93b" }, { "hardware", "#b36ae2" },
    { "wifi", "#2bb7c9" }, { "espnow", "#3dbf8a" }, { "display", "#f0a05a" }, { "keyboard", "#f06292" },
    { "docs", "#8b8f9a" }, { "infra", "#6c8ebf" }, { "security", "#c0392b" }, { "flaky", "#d4a017" },
};
const char* const t_status_names[T_STATUS_N] = { "backlog", "todo", "in_progress", "review", "done" };
const char* const t_prio_names[T_PRIO_N]     = { "none", "low", "medium", "high", "urgent" };
const char* const t_op_names[4]              = { "create", "update", "comment", "delete" };
const char* const t_field_names[8]           = { "", "title", "description", "status", "priority", "assignee", "labels", "estimate" };

/* ---- lifecycle ----------------------------------------------------------- */
int store_init(u32 max_issues, u32 max_comments, u32 max_terms, u32 scratch_size) {
    if (max_issues < 1 || max_issues > 60000) return -1;
    if (max_comments < 1 || max_comments > 60000) return -1;
    if (max_terms < 64 || max_terms > 30000) return -1;
    u32 before = g.arena.top;
    g.issues   = (issue_t*)   k32_arena_alloc(&g.arena, max_issues * (u32)sizeof(issue_t), 8);
    g.comments = (comment_t*) k32_arena_alloc(&g.arena, max_comments * (u32)sizeof(comment_t), 8);
    g.acts     = (act_t*)     k32_arena_alloc(&g.arena, T_ACT_RING * (u32)sizeof(act_t), 8);
    g.sessions = (session_t*) k32_arena_alloc(&g.arena, T_SESSIONS_N * (u32)sizeof(session_t), 8);
    g.terms    = (term_t*)    k32_arena_alloc(&g.arena, max_terms * (u32)sizeof(term_t), 4);
    g.term_order = (u16*)     k32_arena_alloc(&g.arena, max_terms * (u32)sizeof(u16), 4);
    u32 hcap = 64; while (hcap < max_terms * 2u) hcap <<= 1;      /* load factor <= 0.5 */
    g.term_hash = (u16*)      k32_arena_alloc(&g.arena, hcap * (u32)sizeof(u16), 4);
    g.term_hash_mask = hcap - 1;
    g.scratch  = (u8*)        k32_arena_alloc(&g.arena, scratch_size, 16);
    if (!g.issues || !g.comments || !g.acts || !g.sessions || !g.terms || !g.term_order || !g.term_hash || !g.scratch) return -1;
    g.issue_cap = max_issues; g.comment_cap = max_comments; g.term_cap = max_terms;
    g.scratch_size = scratch_size; g.scratch_top = 0; g.scratch_fresh = 1;
    g.issue_count = g.issue_live = 0;
    g.comment_count = g.comment_live = 0;
    g.act_count = 0; g.act_next_id = 1;
    g.term_count = 0; g.posting_count = 0; g.posting_bytes = 0; g.posting_leaked = 0;
    g.string_bytes = 0;
    g.fixed_bytes = g.arena.top - before;
    g.last_viewed = 0; g.note_len = 0;
    /* reserve offset 0 so that str offset 0 can mean "none" */
    return 0;
}

i64 store_now(void) { return g.epoch_base + k32_time_ms(); }
u32 arena_used(void) { return k32_arena_used(&g.arena); }
u32 ts_pack(i64 ms) {
    i64 s = (ms - g.ts_base) / 1000;
    if (s < 0) return 0;
    if (s > 0xFFFFFFFFll) return 0xFFFFFFFFu;
    return (u32)s;
}
i64 ts_ms(u32 packed) { return g.ts_base + (i64)packed * 1000; }

/* ---- scratch ------------------------------------------------------------- */
void* scratch_alloc(u32 size) {
    u32 top = (g.scratch_top + 7u) & ~7u;
    if (top < g.scratch_top) return 0;
    if (size > g.scratch_size || top > g.scratch_size - size) return 0;
    void* p = g.scratch + top;
    g.scratch_top = top + size;
    return p;
}

/* ---- strings ------------------------------------------------------------- */
u32 str_store(const char* s, u32 len) {
    char* p = (char*)k32_arena_alloc(&g.arena, len ? len : 1, 1);
    if (!p) {
        /* the two ends met: reclaim dead posting blocks at the top, then retry */
        if (index_compact() < 0) return 0;
        p = (char*)k32_arena_alloc(&g.arena, len ? len : 1, 1);
        if (!p) return 0;
    }
    memcpy(p, s, len);
    g.string_bytes += len ? len : 1;
    return (u32)(p - (char*)g.arena.base);
}
const char* str_at(u32 off) { return (const char*)g.arena.base + off; }
int str_replace(u32* off, u16* len, const char* s, u32 n, u32 cap_len) {
    if (n > cap_len) return -1;
    if (*off && n <= *len) {            /* fits in place */
        memcpy((char*)g.arena.base + *off, s, n);
        *len = (u16)n;
        return 0;
    }
    u32 o = str_store(s, n);
    if (!o) return -2;
    *off = o; *len = (u16)n;
    return 0;
}

/* ---- issues -------------------------------------------------------------- */
issue_t* issue_slot(u32 id) {
    if (id == 0 || id > g.issue_count) return 0;
    return &g.issues[id - 1];
}
issue_t* issue_by_id(u32 id) {
    issue_t* it = issue_slot(id);
    if (!it || (it->flags & ISSUE_DELETED)) return 0;
    return it;
}
int issue_parse_key(const char* key, u32 len, u32* out_id) {
    u32 dash = 0;
    while (dash < len && key[dash] != '-') dash++;
    if (dash == 0 || dash >= len) return 0;
    int proj = -1;
    for (int p = 0; p < T_PROJECTS_N; p++) {
        if (k32_streq(key, dash, t_projects[p].key)) { proj = p + 1; break; }
    }
    if (proj < 0) return 0;
    u32 id;
    if (!k32_parse_u32(key + dash + 1, len - dash - 1, &id) || id == 0) return 0;
    issue_t* it = issue_slot(id);
    if (!it || it->project != (u8)proj) return 0;
    *out_id = id;
    return 1;
}
u32 issue_key(u32 id, char* out, u32 cap) {
    issue_t* it = issue_slot(id);
    if (!it || cap < 16) return 0;
    const char* k = t_projects[(it->project >= 1 && it->project <= T_PROJECTS_N) ? it->project - 1 : 0].key;
    u32 n = (u32)strlen(k);
    memcpy(out, k, n);
    out[n++] = '-';
    n += k32_fmt_u32(out + n, cap - n, id);
    return n;
}
int issue_create(u8 project, const char* title, u32 tlen, const char* desc, u32 dlen,
                 u8 status, u8 priority, u8 assignee, u8 reporter, u16 labels, u8 estimate, i64 created) {
    if (g.issue_count >= g.issue_cap) return -1;
    if (tlen == 0 || tlen > T_TITLE_MAX || dlen > T_DESC_MAX) return -2;
    u32 toff = str_store(title, tlen);
    if (!toff) return -3;
    u32 doff = dlen ? str_store(desc, dlen) : 0;
    if (dlen && !doff) return -3;
    issue_t* it = &g.issues[g.issue_count];
    memset(it, 0, sizeof *it);
    it->created = ts_pack(created); it->updated = it->created;
    it->resolved = (status == ST_DONE) ? it->created : 0;
    it->title_off = toff; it->title_len = (u8)tlen;
    it->desc_off = doff; it->desc_len = (u16)dlen;
    it->labels = labels & ((1u << T_LABELS_N) - 1u);
    it->project = project; it->status = status; it->priority = priority;
    it->assignee = assignee; it->reporter = reporter; it->estimate = estimate;
    if (k32_jw_needs_escape(title, tlen)) it->flags |= ISSUE_TITLE_ESC;
    if (dlen && k32_jw_needs_escape(desc, dlen)) it->flags |= ISSUE_DESC_ESC;
    g.issue_count++; g.issue_live++;
    return (int)g.issue_count;
}
void issue_set_status(issue_t* it, u8 status, i64 ts) {
    u8 old = it->status;
    it->status = status;
    if (status == ST_DONE && old != ST_DONE) it->resolved = ts_pack(ts);
    if (status != ST_DONE) it->resolved = 0;
}

/* ---- comments ------------------------------------------------------------ */
comment_t* comment_by_id(u32 id) {
    if (id == 0 || id > g.comment_count) return 0;
    return &g.comments[id - 1];
}
int comment_add(u32 issue_id, u8 author, const char* body, u32 len, i64 ts) {
    issue_t* it = issue_by_id(issue_id);
    if (!it) return -1;
    if (g.comment_count >= g.comment_cap) return -2;
    if (len == 0 || len > T_COMMENT_MAX) return -3;
    if (it->comment_count >= 60000) return -2;
    u32 off = str_store(body, len);
    if (!off) return -4;
    u32 cid = g.comment_count + 1;
    comment_t* c = &g.comments[cid - 1];
    memset(c, 0, sizeof *c);
    c->created = ts_pack(ts); c->body_off = off; c->body_len = (u16)len;
    c->issue = (u16)issue_id; c->author = author; c->next = 0;
    if (k32_jw_needs_escape(body, len)) c->flags |= COMMENT_ESC;
    if (it->comment_tail) {
        comment_t* t = comment_by_id(it->comment_tail);
        if (t) t->next = (u16)cid;
    } else it->comment_head = (u16)cid;
    it->comment_tail = (u16)cid;
    it->comment_count++;
    if (c->created > it->updated) it->updated = c->created;
    g.comment_count++; g.comment_live++;
    return (int)cid;
}

/* ---- activity ring ------------------------------------------------------- */
void act_push(i64 ts, u8 actor, u8 op, u16 issue, u8 field, i32 from, i32 to) {
    act_t* a = &g.acts[g.act_count % T_ACT_RING];
    a->ts = ts; a->id = g.act_next_id++; a->from = from; a->to = to;
    a->issue = issue; a->actor = actor; a->op = op; a->field = field;
    g.act_count++;
}
u32 act_available(void) { return g.act_count < T_ACT_RING ? g.act_count : T_ACT_RING; }
const act_t* act_at(u32 i) {
    if (i >= act_available()) return 0;
    u32 idx = (g.act_count - 1 - i) % T_ACT_RING;
    return &g.acts[idx];
}

/* ---- sessions ------------------------------------------------------------ */
session_t* session_create(u8 user, i64 uptime_ms) {
    session_t* s = 0;
    for (int i = 0; i < T_SESSIONS_N; i++) if (!g.sessions[i].used) { s = &g.sessions[i]; break; }
    if (!s) {   /* evict the least recently used */
        s = &g.sessions[0];
        for (int i = 1; i < T_SESSIONS_N; i++) if (g.sessions[i].last_seen < s->last_seen) s = &g.sessions[i];
    }
    memset(s, 0, sizeof *s);
    k32_fmt_hex32(s->token, 8, k32_random());
    k32_fmt_hex32(s->token + 8, 8, k32_random());
    s->user = user; s->used = 1; s->last_seen = uptime_ms;
    return s;
}
session_t* session_find(const char* tok, u32 len) {
    if (len != 16) return 0;
    for (int i = 0; i < T_SESSIONS_N; i++) {
        session_t* s = &g.sessions[i];
        if (s->used && memcmp(s->token, tok, 16) == 0) return s;
    }
    return 0;
}
void session_drop(session_t* s) { memset(s, 0, sizeof *s); }
u32 session_expire(i64 uptime_ms) {
    u32 n = 0;
    for (int i = 0; i < T_SESSIONS_N; i++) {
        session_t* s = &g.sessions[i];
        if (s->used && uptime_ms - s->last_seen > T_SESSION_IDLE_MS) { session_drop(s); n++; }
    }
    return n;
}
u32 session_active(void) {
    u32 n = 0;
    for (int i = 0; i < T_SESSIONS_N; i++) if (g.sessions[i].used) n++;
    return n;
}

/* ---- lookups ------------------------------------------------------------- */
int status_from_name(const char* s, u32 len) {
    for (int i = 0; i < T_STATUS_N; i++) if (k32_streq(s, len, t_status_names[i])) return i;
    return -1;
}
int user_from_name(const char* s, u32 len) {
    for (int i = 0; i < T_USERS_N; i++) if (k32_streq(s, len, t_users[i].username)) return i;
    return -1;
}

/* ---- latency stats ------------------------------------------------------- */
void stats_record_request(u32 dur_us, int is_error) {
    g.req_total++;
    if (is_error) g.req_errors++;
    g.req_sum_us += dur_us;
    g.req_last_us = dur_us;
    g.lat_ring[g.lat_n % T_LAT_RING] = dur_us;
    g.lat_n++;
}

#if defined(K32_NATIVE)
int store_verify_counts(void) {
    u32 live = 0, clive = 0;
    for (u32 id = 1; id <= g.issue_count; id++) {
        const issue_t* it = &g.issues[id - 1];
        /* recount this issue's comments from the comment table */
        u32 n = 0, last = 0;
        for (u32 cid = 1; cid <= g.comment_count; cid++) if (g.comments[cid - 1].issue == id) { n++; last = cid; }
        if (n != it->comment_count) return -1;
        if ((n == 0) != (it->comment_head == 0)) return -2;
        if (it->comment_tail != last) return -3;
        /* the chain visits exactly those comments, in id order */
        u32 walked = 0, prev = 0, cid = it->comment_head;
        while (cid) { if (cid <= prev || g.comments[cid - 1].issue != id) return -4; walked++; prev = cid; cid = g.comments[cid - 1].next; }
        if (walked != n) return -5;
        if (!(it->flags & ISSUE_DELETED)) { live++; clive += n; }
    }
    if (live != g.issue_live) return -6;
    if (clive != g.comment_live) return -7;
    return 0;
}
#endif
