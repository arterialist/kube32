#include "router.h"
#include "index.h"
#include "seed.h"
#include "k32_host.h"

#define RESP_BUF 4096
#define BODY_MAX 8192
#define LIST_LIMIT_MAX 50
#define LIST_LIMIT_DEFAULT 25
#define BOARD_LIMIT 40
#define SEARCH_LIMIT_DEFAULT 20
#define ACTIVITY_LIMIT_DEFAULT 50
#define ACTIVITY_LIMIT_MAX 200
#define MAX_FIELDS 12

static const char k_json[] = "application/json";

/* ---- response plumbing --------------------------------------------------- */
static int resp_flush(void* ctx, const char* data, u32 len) { (void)ctx; return k32_resp_write(data, len); }

static int resp_begin_json(k32_jw* w, int status) {
    char* buf = (char*)scratch_alloc(RESP_BUF);
    static char fallback[256];
    if (!buf) { buf = fallback; }
    k32_jw_init(w, buf, buf == fallback ? (u32)sizeof fallback : RESP_BUF, resp_flush, 0);
    k32_resp_begin(status, k_json, (u32)sizeof(k_json) - 1);
    return status;
}
static int resp_finish(k32_jw* w) { k32_jw_flush(w); k32_resp_end(); return 0; }

static int resp_error(int status, const char* code, const char* msg, const char* detail, u32 dlen) {
    k32_jw w;
    resp_begin_json(&w, status);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "error"); k32_jw_cstr(&w, code);
    k32_jw_keylit(&w, "message");
    if (detail) {
        k32_jw_raw(&w, "\"", 1); k32_jw_escaped(&w, msg, (u32)strlen(msg)); k32_jw_escaped(&w, detail, dlen); k32_jw_raw(&w, "\"", 1);
    } else k32_jw_cstr(&w, msg);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return status;
}
static int resp_empty(int status) {
    k32_resp_begin(status, k_json, (u32)sizeof(k_json) - 1);
    k32_resp_end();
    return status;
}

/* ---- JSON object writers ------------------------------------------------- */
static void write_user(k32_jw* w, int id) {
    const user_t* u = &t_users[id];
    k32_jw_obj_begin(w);
    k32_jw_keylit(w, "id"); k32_jw_int(w, id);
    k32_jw_keylit(w, "username"); k32_jw_cstr(w, u->username);
    k32_jw_keylit(w, "name"); k32_jw_cstr(w, u->name);
    k32_jw_keylit(w, "initials"); k32_jw_cstr(w, u->initials);
    k32_jw_keylit(w, "color"); k32_jw_cstr(w, u->color);
    k32_jw_obj_end(w);
}
static void write_key(k32_jw* w, u32 id) {
    char kb[24]; u32 kl = issue_key(id, kb, sizeof kb);
    k32_jw_str_raw(w, kb, kl);      /* "<PROJECT>-<id>": never needs escaping */
}
static void write_assignee(k32_jw* w, const issue_t* it) {
    if (it->assignee == T_NO_USER) k32_jw_null(w); else k32_jw_int(w, it->assignee);
}
static void write_labels(k32_jw* w, u16 mask) {
    k32_jw_arr_begin(w);
    for (int i = 0; i < T_MAX_LABELS; i++) { K32_PROF(loop_iters); if (mask & (1u << i)) k32_jw_int(w, i); }
    k32_jw_arr_end(w);
}
static void write_text(k32_jw* w, const char* s, u32 len, int needs_escape) {
    if (needs_escape) k32_jw_str(w, s, len); else k32_jw_str_raw(w, s, len);
}
static void write_issue_fields(k32_jw* w, u32 id, const issue_t* it, int full) {
    k32_jw_keylit(w, "id"); k32_jw_int(w, id);
    k32_jw_keylit(w, "key"); write_key(w, id);
    k32_jw_keylit(w, "project"); k32_jw_int(w, it->project);
    k32_jw_keylit(w, "title"); write_text(w, str_at(it->title_off), it->title_len, it->flags & ISSUE_TITLE_ESC);
    if (full) { k32_jw_keylit(w, "description"); write_text(w, it->desc_len ? str_at(it->desc_off) : "", it->desc_len, it->flags & ISSUE_DESC_ESC); }
    k32_jw_keylit(w, "status"); k32_jw_cstr(w, t_status_names[it->status < T_STATUS_N ? it->status : 0]);
    k32_jw_keylit(w, "priority"); k32_jw_int(w, it->priority);
    k32_jw_keylit(w, "assignee"); write_assignee(w, it);
    k32_jw_keylit(w, "reporter"); k32_jw_int(w, it->reporter);
    k32_jw_keylit(w, "labels"); write_labels(w, it->labels);
    k32_jw_keylit(w, "estimate"); k32_jw_int(w, it->estimate);
    k32_jw_keylit(w, "created"); k32_jw_int(w, ts_ms(it->created));
    k32_jw_keylit(w, "updated"); k32_jw_int(w, ts_ms(it->updated));
    k32_jw_keylit(w, "comment_count"); k32_jw_int(w, it->comment_count);
}
static void write_issue(k32_jw* w, u32 id, const issue_t* it, int full) {
    k32_jw_obj_begin(w);
    write_issue_fields(w, id, it, full);
    k32_jw_obj_end(w);
}
static void write_comment(k32_jw* w, u32 cid, const comment_t* c) {
    k32_jw_obj_begin(w);
    k32_jw_keylit(w, "id"); k32_jw_int(w, cid);
    k32_jw_keylit(w, "issue"); k32_jw_int(w, c->issue);
    k32_jw_keylit(w, "author"); k32_jw_int(w, c->author);
    k32_jw_keylit(w, "body"); write_text(w, str_at(c->body_off), c->body_len, c->flags & COMMENT_ESC);
    k32_jw_keylit(w, "created"); k32_jw_int(w, ts_ms(c->created));
    k32_jw_obj_end(w);
}
static void write_act_value(k32_jw* w, u8 field, i32 v) {
    char tmp[16];
    switch (field) {
        case F_STATUS:   k32_jw_cstr(w, (v >= 0 && v < T_STATUS_N) ? t_status_names[v] : ""); break;
        case F_PRIORITY: k32_jw_cstr(w, (v >= 0 && v < T_PRIO_N) ? t_prio_names[v] : ""); break;
        case F_ASSIGNEE: k32_jw_cstr(w, (v >= 0 && v < T_USERS_N) ? t_users[v].username : ""); break;
        case F_ESTIMATE:
        case F_LABELS:   k32_jw_str(w, tmp, k32_fmt_i64(tmp, sizeof tmp, v)); break;
        default:         k32_jw_cstr(w, ""); break;
    }
}
static void write_activity(k32_jw* w, const act_t* a) {
    k32_jw_obj_begin(w);
    k32_jw_keylit(w, "id"); k32_jw_int(w, a->id);
    k32_jw_keylit(w, "ts"); k32_jw_int(w, a->ts);
    k32_jw_keylit(w, "actor"); k32_jw_int(w, a->actor);
    k32_jw_keylit(w, "issue"); k32_jw_int(w, a->issue);
    k32_jw_keylit(w, "key"); write_key(w, a->issue);
    k32_jw_keylit(w, "op"); k32_jw_cstr(w, t_op_names[a->op < 4 ? a->op : 0]);
    k32_jw_keylit(w, "field"); k32_jw_cstr(w, t_field_names[a->field < 8 ? a->field : 0]);
    if (a->op == OP_COMMENT) {
        char tmp[16];
        k32_jw_keylit(w, "from"); k32_jw_cstr(w, "");
        k32_jw_keylit(w, "to"); k32_jw_str(w, tmp, k32_fmt_i64(tmp, sizeof tmp, a->to));
    } else {
        k32_jw_keylit(w, "from"); write_act_value(w, a->field, a->from);
        k32_jw_keylit(w, "to"); write_act_value(w, a->field, a->to);
    }
    k32_jw_obj_end(w);
}

/* ---- events -------------------------------------------------------------- */
static const char k_topic[] = "tessera.events";
static void emit(k32_jw* w) {
    if (w->truncated) return;
    k32_event_emit(k_topic, (u32)sizeof(k_topic) - 1, w->buf, w->len);
}
void events_issue(const char* op, u32 id, u8 actor) {
    const issue_t* it = issue_slot(id);
    if (!it) return;
    char buf[K32_MAX_EVENT_BYTES]; k32_jw w;
    k32_jw_init(&w, buf, sizeof buf, 0, 0);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "type"); k32_jw_cstr(&w, "issue");
    k32_jw_keylit(&w, "op"); k32_jw_cstr(&w, op);
    k32_jw_keylit(&w, "key"); write_key(&w, id);
    k32_jw_keylit(&w, "status"); k32_jw_cstr(&w, t_status_names[it->status < T_STATUS_N ? it->status : 0]);
    k32_jw_keylit(&w, "actor"); k32_jw_int(&w, actor);
    k32_jw_keylit(&w, "title"); k32_jw_str_max(&w, str_at(it->title_off), it->title_len, 96);
    k32_jw_obj_end(&w);
    emit(&w);
}
void events_comment(u32 issue_id, u32 comment_id, u8 author) {
    char buf[K32_MAX_EVENT_BYTES]; k32_jw w;
    k32_jw_init(&w, buf, sizeof buf, 0, 0);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "type"); k32_jw_cstr(&w, "comment");
    k32_jw_keylit(&w, "key"); write_key(&w, issue_id);
    k32_jw_keylit(&w, "author"); k32_jw_int(&w, author);
    k32_jw_keylit(&w, "id"); k32_jw_int(&w, comment_id);
    k32_jw_obj_end(&w);
    emit(&w);
}
void events_hud(const char* text, u32 len) {
    char buf[K32_MAX_EVENT_BYTES]; k32_jw w;
    k32_jw_init(&w, buf, sizeof buf, 0, 0);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "type"); k32_jw_cstr(&w, "hud");
    k32_jw_keylit(&w, "text"); k32_jw_str_max(&w, text, len, 160);
    k32_jw_obj_end(&w);
    emit(&w);
}

/* ---- shared mutations ---------------------------------------------------- */
u32 first_live_issue(void) {
    for (u32 id = 1; id <= g.issue_count; id++) if (issue_by_id(id)) return id;
    return 0;
}
int issue_add_comment_indexed(u32 issue_id, u8 author, const char* body, u32 len, i64 now) {
    if (!issue_by_id(issue_id)) return -1;
    int cid = comment_add(issue_id, author, body, len, now);
    if (cid < 0) return cid;
    index_add_comment(issue_id, body, len);   /* merges the new tokens; same result as remove + re-add */
    act_push(now, author, OP_COMMENT, (u16)issue_id, F_NONE, 0, cid);
    events_comment(issue_id, (u32)cid, author);
    return cid;
}

/* ---- request parsing ----------------------------------------------------- */
int router_parse(char* buf, u32 len, req_t* rq) {
    memset(rq, 0, sizeof *rq);
    rq->user = -1;
    u32 i = 0;
    rq->method = buf;
    while (i < len && buf[i] != ' ' && buf[i] != '\n') i++;
    if (i >= len || buf[i] != ' ') return -1;
    rq->mlen = i; i++;
    rq->path = buf + i;
    u32 ps = i;
    while (i < len && buf[i] != '\n' && buf[i] != '?' && buf[i] != ' ') i++;
    rq->plen = i - ps;
    if (i < len && buf[i] == '?') {
        i++;
        rq->query = buf + i; u32 qs = i;
        while (i < len && buf[i] != '\n' && buf[i] != ' ') i++;
        rq->qlen = i - qs;
    }
    while (i < len && buf[i] != '\n') i++;   /* tolerate "HTTP/1.1" trailing token */
    if (i >= len) return rq->plen ? 0 : -1;
    i++;
    /* headers */
    while (i < len) {
        if (buf[i] == '\n') { i++; break; }
        u32 hs = i;
        while (i < len && buf[i] != '\n') i++;
        u32 he = i;
        if (i < len) i++;
        u32 colon = hs;
        while (colon < he && buf[colon] != ':') colon++;
        if (colon >= he) continue;
        u32 vs = colon + 1;
        while (vs < he && buf[vs] == ' ') vs++;
        u32 ve = he;
        while (ve > vs && (buf[ve - 1] == '\r' || buf[ve - 1] == ' ')) ve--;
        /* case-insensitive compare with "Cookie" */
        if (colon - hs == 6) {
            int ok = 1; const char* cn = "cookie";
            for (u32 k = 0; k < 6; k++) { char c = buf[hs + k]; if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if (c != cn[k]) { ok = 0; break; } }
            if (ok) { rq->cookie = buf + vs; rq->clen = ve - vs; }
        }
    }
    rq->body = buf + i;
    rq->blen = len - i;
    if (rq->plen == 0 || rq->mlen == 0) return -1;
    return 0;
}

static int cookie_get(const req_t* rq, const char* name, const char** val, u32* vlen) {
    u32 nlen = (u32)strlen(name);
    const char* c = rq->cookie; u32 len = rq->clen; u32 i = 0;
    while (i < len) {
        while (i < len && (c[i] == ' ' || c[i] == ';')) i++;
        u32 s = i;
        while (i < len && c[i] != ';') i++;
        u32 e = i;
        u32 eq = s;
        while (eq < e && c[eq] != '=') eq++;
        if (eq - s == nlen && memcmp(c + s, name, nlen) == 0 && eq < e) {
            u32 vs = eq + 1, ve = e;
            while (ve > vs && c[ve - 1] == ' ') ve--;
            *val = c + vs; *vlen = ve - vs;
            return 1;
        }
    }
    return 0;
}

/* ---- query helpers ------------------------------------------------------- */
static int q_u32(const req_t* rq, const char* key, u32* out) {
    const char* v; u32 vl;
    if (!k32_query_get(rq->query, rq->qlen, key, &v, &vl) || vl == 0) return 0;
    return k32_parse_u32(v, vl, out);
}
static int q_str(const req_t* rq, const char* key, const char** v, u32* vl) {
    return k32_query_get(rq->query, rq->qlen, key, v, vl) && *vl > 0;
}

/* ---- sorting contexts ---------------------------------------------------- */
typedef struct { u8 key; u8 desc; const i32* scores; } sort_ctx;
enum { SORT_UPDATED = 0, SORT_CREATED, SORT_PRIORITY, SORT_KEY, SORT_BOARD, SORT_SCORE };

static int cmp_i64(i64 a, i64 b) { return a < b ? -1 : (a > b ? 1 : 0); }
/* Strict total order for every key: the issue id is always the final
 * tiebreaker (flipped with the rest under desc), so a bounded top-k
 * selection yields exactly the page a full sort would. */
static int cmp_issue(u16 a, u16 b, void* vctx) {
    const sort_ctx* c = (const sort_ctx*)vctx;
    const issue_t* x = &g.issues[a - 1];
    const issue_t* y = &g.issues[b - 1];
    int r = 0;
    K32_PROF(cmp_calls);
    switch (c->key) {
        case SORT_UPDATED:  r = cmp_i64(x->updated, y->updated); break;
        case SORT_CREATED:  r = cmp_i64(x->created, y->created); break;
        case SORT_PRIORITY: r = (int)x->priority - (int)y->priority; if (!r) r = cmp_i64(x->updated, y->updated); break;
        case SORT_KEY:      r = 0; break;
        case SORT_BOARD:    /* priority desc, updated desc regardless of order flag */
            r = (int)y->priority - (int)x->priority;
            if (!r) r = cmp_i64(y->updated, x->updated);
            if (!r) r = (int)b - (int)a;
            return r;
        case SORT_SCORE:
            r = c->scores[b] - c->scores[a];
            if (!r) r = cmp_i64(y->updated, x->updated);
            if (!r) r = (int)b - (int)a;
            return r;
    }
    if (!r) r = (int)a - (int)b;
    return c->desc ? -r : r;
}

/* The same order as cmp_issue as a (primary, secondary, id) triple of u32s,
 * so the candidate scan can reject against the current k-th best with one
 * or two 32-bit compares (no interpreted indirect call, no i64 math).
 * Not used for SORT_SCORE. */
typedef struct { u32 a, b; u16 id; } skey_t;
static inline skey_t sort_key(u8 key, const issue_t* x, u16 id) {
    skey_t k; k.id = id;
    switch (key) {
        case SORT_CREATED:  k.a = x->created;  k.b = 0; break;
        case SORT_KEY:      k.a = id;          k.b = 0; break;
        case SORT_PRIORITY:
        case SORT_BOARD:    k.a = x->priority; k.b = x->updated; break;
        default:            k.a = x->updated;  k.b = 0; break;
    }
    return k;
}
/* 1 when candidate c beats the current k-th best k under the order direction */
static inline int beats_kth(int desc, const skey_t* c, const skey_t* k) {
    if (c->a != k->a) return desc ? c->a > k->a : c->a < k->a;
    if (c->b != k->b) return desc ? c->b > k->b : c->b < k->b;
    return desc ? c->id > k->id : c->id < k->id;
}

/* ---- handlers ------------------------------------------------------------ */
static int h_login(req_t* rq) {
    k32_jfield f[MAX_FIELDS];
    if (rq->blen == 0 || rq->blen > BODY_MAX) return resp_error(400, "bad_request", "json body required", 0, 0);
    int n = k32_json_parse_flat(rq->body, rq->blen, f, MAX_FIELDS);
    if (n < 0) return resp_error(400, "bad_request", "malformed json", 0, 0);
    const k32_jfield* u = k32_jf_get(f, n, "username");
    const k32_jfield* p = k32_jf_get(f, n, "password");
    if (!u || u->type != K32_J_STR || !p || p->type != K32_J_STR)
        return resp_error(400, "validation", "username and password are required", 0, 0);
    int uid = user_from_name(u->s, u->slen);
    if (uid < 1 || !k32_streq(p->s, p->slen, "demo"))
        return resp_error(401, "invalid_credentials", "unknown user or wrong password", 0, 0);
    session_t* s = session_create((u8)uid, rq->uptime);
    char cookie[80]; u32 cl = 0;
    memcpy(cookie, "tsid=", 5); cl = 5;
    memcpy(cookie + cl, s->token, 16); cl += 16;
    const char* tail = "; Path=/; HttpOnly; SameSite=Lax";
    memcpy(cookie + cl, tail, strlen(tail)); cl += (u32)strlen(tail);
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_resp_header("Set-Cookie", 10, cookie, cl);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "user"); write_user(&w, uid);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}
static int h_logout(req_t* rq) {
    if (rq->session) session_drop(rq->session);
    k32_resp_begin(204, k_json, (u32)sizeof(k_json) - 1);
    const char* c = "tsid=; Path=/; HttpOnly; Max-Age=0";
    k32_resp_header("Set-Cookie", 10, c, (u32)strlen(c));
    k32_resp_end();
    return 204;
}
static int h_me(req_t* rq) {
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "user"); write_user(&w, rq->user);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}
static int h_bootstrap(req_t* rq) {
    u32 by_status[T_STATUS_N] = { 0 };
    for (u32 id = 1; id <= g.issue_count; id++) {
        const issue_t* it = issue_by_id(id);
        if (it && it->status < T_STATUS_N) by_status[it->status]++;
    }
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_keylit(&w, "users"); k32_jw_arr_begin(&w);
    for (int i = 0; i < T_USERS_N; i++) write_user(&w, i);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "projects"); k32_jw_arr_begin(&w);
    for (int i = 0; i < T_PROJECTS_N; i++) {
        k32_jw_obj_begin(&w);
        k32_jw_keylit(&w, "id"); k32_jw_int(&w, i + 1);
        k32_jw_keylit(&w, "key"); k32_jw_cstr(&w, t_projects[i].key);
        k32_jw_keylit(&w, "name"); k32_jw_cstr(&w, t_projects[i].name);
        k32_jw_keylit(&w, "color"); k32_jw_cstr(&w, t_projects[i].color);
        k32_jw_obj_end(&w);
    }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "labels"); k32_jw_arr_begin(&w);
    for (int i = 0; i < T_LABELS_N; i++) {
        k32_jw_obj_begin(&w);
        k32_jw_keylit(&w, "id"); k32_jw_int(&w, i);
        k32_jw_keylit(&w, "name"); k32_jw_cstr(&w, t_labels[i].name);
        k32_jw_keylit(&w, "color"); k32_jw_cstr(&w, t_labels[i].color);
        k32_jw_obj_end(&w);
    }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "statuses"); k32_jw_arr_begin(&w);
    for (int i = 0; i < T_STATUS_N; i++) k32_jw_cstr(&w, t_status_names[i]);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "priorities"); k32_jw_arr_begin(&w);
    for (int i = 0; i < T_PRIO_N; i++) k32_jw_cstr(&w, t_prio_names[i]);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "counts"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "open"); k32_jw_int(&w, (i64)g.issue_live - by_status[ST_DONE]);
    k32_jw_keylit(&w, "total"); k32_jw_int(&w, g.issue_live);
    k32_jw_keylit(&w, "by_status"); k32_jw_obj_begin(&w);
    for (int i = 0; i < T_STATUS_N; i++) { k32_jw_key(&w, t_status_names[i]); k32_jw_int(&w, by_status[i]); }
    k32_jw_obj_end(&w);
    k32_jw_obj_end(&w);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

typedef struct {
    u32 project; u32 status_mask; int assignee; int label; int priority;
    u8* qmask;   /* non-NULL when q given: qmask[id] != 0 means matched */
} filter_t;

static int issue_matches(const issue_t* it, u32 id, const filter_t* f) {
    if (f->project && it->project != f->project) return 0;
    if (f->status_mask && !(f->status_mask & (1u << it->status))) return 0;
    if (f->assignee >= 0 && it->assignee != (u8)f->assignee) return 0;
    if (f->assignee == -2 && it->assignee != T_NO_USER) return 0;
    if (f->label >= 0 && !(it->labels & (1u << f->label))) return 0;
    if (f->priority >= 0 && it->priority != (u8)f->priority) return 0;
    if (f->qmask && !f->qmask[id]) return 0;
    return 1;
}
static int parse_filters(req_t* rq, filter_t* f, const char** err) {
    memset(f, 0, sizeof *f);
    f->assignee = -1; f->label = -1; f->priority = -1;
    u32 v; const char* s; u32 sl;
    if (q_u32(rq, "project", &v)) { if (v > T_PROJECTS_N) { *err = "unknown project"; return -1; } f->project = v; }
    if (q_str(rq, "status", &s, &sl)) {
        u32 i = 0;
        while (i < sl) {
            u32 st = i; while (i < sl && s[i] != ',') i++;
            int code = status_from_name(s + st, i - st);
            if (code < 0) { *err = "unknown status"; return -1; }
            f->status_mask |= 1u << code;
            i++;
        }
    }
    if (q_str(rq, "assignee", &s, &sl)) {
        if (k32_streq(s, sl, "none") || k32_streq(s, sl, "null")) f->assignee = -2;
        else if (!k32_parse_u32(s, sl, &v) || v >= T_USERS_N) { *err = "unknown assignee"; return -1; }
        else f->assignee = (int)v;
    }
    if (q_str(rq, "label", &s, &sl)) {
        if (!k32_parse_u32(s, sl, &v) || v >= T_LABELS_N) { *err = "unknown label"; return -1; }
        f->label = (int)v;
    }
    if (q_str(rq, "priority", &s, &sl)) {
        if (!k32_parse_u32(s, sl, &v) || v >= T_PRIO_N) { *err = "priority out of range"; return -1; }
        f->priority = (int)v;
    }
    if (q_str(rq, "q", &s, &sl)) {
        char* qb = (char*)scratch_alloc(256);
        u8* mask = (u8*)scratch_alloc(g.issue_cap + 1);
        if (!qb || !mask) { *err = "out of scratch"; return -2; }
        u32 ql = k32_url_decode(s, sl, qb, 255);
        u16* ids; i32* scores; u32 scanned;
        int hits = index_search(qb, ql, &ids, &scores, &scanned, rq->now);
        if (hits < 0) { *err = "out of scratch"; return -2; }
        memset(mask, 0, g.issue_cap + 1);
        for (int i = 0; i < hits; i++) mask[ids[i]] = 1;
        f->qmask = mask;
    }
    return 0;
}

static int h_issues_list(req_t* rq) {
    filter_t f; const char* err = 0;
    int rc = parse_filters(rq, &f, &err);
    if (rc == -1) return resp_error(400, "validation", err, 0, 0);
    if (rc == -2) return resp_error(500, "internal", err, 0, 0);
    sort_ctx sc = { SORT_UPDATED, 1, 0 };
    const char* s; u32 sl;
    if (q_str(rq, "sort", &s, &sl)) {
        if (k32_streq(s, sl, "updated")) sc.key = SORT_UPDATED;
        else if (k32_streq(s, sl, "created")) sc.key = SORT_CREATED;
        else if (k32_streq(s, sl, "priority")) sc.key = SORT_PRIORITY;
        else if (k32_streq(s, sl, "key")) sc.key = SORT_KEY;
        else return resp_error(400, "validation", "unknown sort", 0, 0);
    }
    if (q_str(rq, "order", &s, &sl)) {
        if (k32_streq(s, sl, "asc")) sc.desc = 0;
        else if (k32_streq(s, sl, "desc")) sc.desc = 1;
        else return resp_error(400, "validation", "order must be asc or desc", 0, 0);
    }
    u32 page = 1, limit = LIST_LIMIT_DEFAULT;
    if (q_u32(rq, "page", &page) && page == 0) page = 1;
    if (q_u32(rq, "limit", &limit)) { if (limit == 0) limit = 1; if (limit > LIST_LIMIT_MAX) limit = LIST_LIMIT_MAX; }
    /* bounded selection: only the first offset + limit issues in sort order are kept */
    u32 start = (page - 1) > (0xFFFFFFFFu / limit) ? 0xFFFFFFFFu : (page - 1) * limit;
    u32 k = start > 0xFFFFFFFFu - limit ? 0xFFFFFFFFu : start + limit;
    if (k > g.issue_count) k = g.issue_count;
    u16* cand = (u16*)scratch_alloc((k ? k : 1) * sizeof(u16));
    if (!cand) return resp_error(500, "internal", "out of scratch", 0, 0);
    k32_topk top; k32_topk_init(&top, cand, k, cmp_issue, &sc);
    u32 n = 0; skey_t kth = { 0, 0, 0 };
    const int any_filter = f.project || f.status_mask || f.assignee != -1 || f.label >= 0 || f.priority >= 0 || f.qmask;
    const issue_t* issues = g.issues; const u32 count = g.issue_count;
    /* Scan newest-first for descending orders: arrivals then tend to follow
     * the sort order, so most candidates fail the k-th-best compare. The
     * selection itself does not depend on the scan order. Everything in the
     * loop is inline: this runs once per issue in the table. */
    for (u32 i = 0; i < count; i++) {
        K32_PROF(loop_iters); K32_PROF(scan_issues);
        u32 id = sc.desc ? count - i : i + 1;
        const issue_t* it = &issues[id - 1];
        if (it->flags & ISSUE_DELETED) continue;
        if (any_filter && !issue_matches(it, id, &f)) continue;
        n++;
        if (top.n == top.k) { skey_t c = sort_key(sc.key, it, (u16)id); if (!beats_kth(sc.desc, &c, &kth)) continue; }
        k32_topk_push(&top, (u16)id);
        if (top.n == top.k) { u16 last = top.a[top.n - 1]; kth = sort_key(sc.key, &issues[last - 1], last); }
    }
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "items"); k32_jw_arr_begin(&w);
    for (u32 i = start; i < top.n && i < start + limit; i++) write_issue(&w, cand[i], &g.issues[cand[i] - 1], 0);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "total"); k32_jw_int(&w, n);
    k32_jw_keylit(&w, "page"); k32_jw_int(&w, page);
    k32_jw_keylit(&w, "limit"); k32_jw_int(&w, limit);
    k32_jw_keylit(&w, "took_us"); k32_jw_int(&w, (k32_time_ms() - rq->t0_ms) * 1000);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

static int h_issue_get(req_t* rq, u32 id) {
    const issue_t* it = issue_by_id(id);
    if (!it) return resp_error(404, "not_found", "issue does not exist", 0, 0);
    g.last_viewed = (u16)id;
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    write_issue_fields(&w, id, it, 1);
    k32_jw_keylit(&w, "comments"); k32_jw_arr_begin(&w);
    u32 cid = it->comment_head, guard = 0;
    while (cid && guard++ < 65535) {
        const comment_t* c = comment_by_id(cid);
        if (!c) break;
        write_comment(&w, cid, c);
        cid = c->next;
    }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "activity"); k32_jw_arr_begin(&w);
    u32 avail = act_available(), shown = 0;
    for (u32 i = 0; i < avail && shown < 20; i++) {
        const act_t* a = act_at(i);
        if (a && a->issue == id) { write_activity(&w, a); shown++; }
    }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

/* validated issue input (POST and PATCH share it) */
typedef struct {
    const char* title; u32 tlen; const char* desc; u32 dlen;
    int project, status, priority, assignee, estimate; int labels; /* -1 = not given */
    int has_title, has_desc, has_labels;
    u16 label_mask;
} issue_in;

static const char* parse_issue_in(const k32_jfield* f, int n, issue_in* in) {
    memset(in, 0, sizeof *in);
    in->project = in->status = in->priority = in->estimate = -1; in->assignee = -3;
    const k32_jfield* x;
    if ((x = k32_jf_get(f, n, "title"))) {
        if (x->type != K32_J_STR) return "title must be a string";
        if (x->slen < 1 || x->slen > T_TITLE_MAX) return "title must be 1..120 characters";
        in->title = x->s; in->tlen = x->slen; in->has_title = 1;
    }
    if ((x = k32_jf_get(f, n, "description"))) {
        if (x->type == K32_J_NULL) { in->desc = ""; in->dlen = 0; in->has_desc = 1; }
        else {
            if (x->type != K32_J_STR) return "description must be a string";
            if (x->slen > T_DESC_MAX) return "description must be at most 1000 characters";
            in->desc = x->s; in->dlen = x->slen; in->has_desc = 1;
        }
    }
    if ((x = k32_jf_get(f, n, "project"))) {
        if (x->type != K32_J_INT || x->i < 1 || x->i > T_PROJECTS_N) return "project must be 1..3";
        in->project = (int)x->i;
    }
    if ((x = k32_jf_get(f, n, "status"))) {
        if (x->type == K32_J_STR) { int c = status_from_name(x->s, x->slen); if (c < 0) return "unknown status"; in->status = c; }
        else if (x->type == K32_J_INT && x->i >= 0 && x->i < T_STATUS_N) in->status = (int)x->i;
        else return "unknown status";
    }
    if ((x = k32_jf_get(f, n, "priority"))) {
        if (x->type != K32_J_INT || x->i < 0 || x->i >= T_PRIO_N) return "priority must be 0..4";
        in->priority = (int)x->i;
    }
    if ((x = k32_jf_get(f, n, "assignee"))) {
        if (x->type == K32_J_NULL) in->assignee = -1;
        else if (x->type == K32_J_INT && x->i >= -1 && x->i < T_USERS_N) in->assignee = (int)x->i;
        else if (x->type == K32_J_STR) { int u = user_from_name(x->s, x->slen); if (u < 0) return "unknown assignee"; in->assignee = u; }
        else return "assignee must be a user id or null";
    }
    if ((x = k32_jf_get(f, n, "labels"))) {
        if (x->type == K32_J_NULL) { in->has_labels = 1; in->label_mask = 0; }
        else if (x->type != K32_J_ARR) return "labels must be an array";
        else {
            u16 m = 0;
            for (int i = 0; i < x->arr_n; i++) {
                const k32_jelem* e = &x->arr[i];
                if (e->is_int) { if (e->i < 0 || e->i >= T_LABELS_N) return "unknown label"; m |= (u16)(1u << e->i); }
                else if (e->is_str) {
                    int found = -1;
                    for (int l = 0; l < T_LABELS_N; l++) if (k32_streq(e->s, e->slen, t_labels[l].name)) { found = l; break; }
                    if (found < 0) return "unknown label";
                    m |= (u16)(1u << found);
                } else return "labels must be ids or names";
            }
            in->has_labels = 1; in->label_mask = m;
        }
    }
    if ((x = k32_jf_get(f, n, "estimate"))) {
        if (x->type == K32_J_NULL) in->estimate = 0;
        else if (x->type != K32_J_INT || x->i < 0 || x->i > 255) return "estimate must be 0..255";
        else in->estimate = (int)x->i;
    }
    return 0;
}

static int parse_body(req_t* rq, k32_jfield* f, int* n) {
    if (rq->blen == 0) { resp_error(400, "bad_request", "json body required", 0, 0); return -1; }
    if (rq->blen > BODY_MAX) { resp_error(400, "payload_too_large", "body exceeds 8192 bytes", 0, 0); return -1; }
    *n = k32_json_parse_flat(rq->body, rq->blen, f, MAX_FIELDS);
    if (*n < 0) { resp_error(400, "bad_request", "malformed json", 0, 0); return -1; }
    return 0;
}

static int h_issue_create(req_t* rq) {
    k32_jfield f[MAX_FIELDS]; int n;
    if (parse_body(rq, f, &n) < 0) return 400;
    issue_in in;
    const char* err = parse_issue_in(f, n, &in);
    if (err) return resp_error(400, "validation", err, 0, 0);
    if (!in.has_title) return resp_error(400, "validation", "title is required", 0, 0);
    if (g.issue_count >= g.issue_cap) return resp_error(507, "insufficient_storage", "issue table is full", 0, 0);
    u8 assignee = (in.assignee == -3 || in.assignee == -1) ? T_NO_USER : (u8)in.assignee;
    int id = issue_create((u8)(in.project < 0 ? 1 : in.project), in.title, in.tlen, in.has_desc ? in.desc : "", in.has_desc ? in.dlen : 0,
                          (u8)(in.status < 0 ? ST_BACKLOG : in.status), (u8)(in.priority < 0 ? 0 : in.priority),
                          assignee, (u8)rq->user, in.has_labels ? in.label_mask : 0,
                          (u8)(in.estimate < 0 ? 0 : in.estimate), rq->now);
    if (id == -1) return resp_error(507, "insufficient_storage", "issue table is full", 0, 0);
    if (id < 0) return resp_error(507, "insufficient_storage", "string arena is full", 0, 0);
    index_add_issue((u32)id);
    act_push(rq->now, (u8)rq->user, OP_CREATE, (u16)id, F_NONE, 0, 0);
    events_issue("create", (u32)id, (u8)rq->user);
    k32_jw w;
    resp_begin_json(&w, 201);
    write_issue(&w, (u32)id, issue_slot((u32)id), 1);
    resp_finish(&w);
    return 201;
}

static int h_issue_patch(req_t* rq, u32 id) {
    issue_t* it = issue_by_id(id);
    if (!it) return resp_error(404, "not_found", "issue does not exist", 0, 0);
    k32_jfield f[MAX_FIELDS]; int n;
    if (parse_body(rq, f, &n) < 0) return 400;
    issue_in in;
    const char* err = parse_issue_in(f, n, &in);
    if (err) return resp_error(400, "validation", err, 0, 0);
    int text_changed = in.has_title || in.has_desc;
    if (text_changed) index_remove_issue(id);
    int changed = 0;
    u8 actor = (u8)rq->user;
    if (in.has_title) {
        u16 tl = it->title_len;
        int rc = str_replace(&it->title_off, &tl, in.title, in.tlen, T_TITLE_MAX);
        if (rc < 0) { index_add_issue(id); return resp_error(507, "insufficient_storage", "string arena is full", 0, 0); }
        it->title_len = (u8)tl;
        it->flags = (u8)((it->flags & ~ISSUE_TITLE_ESC) | (k32_jw_needs_escape(in.title, in.tlen) ? ISSUE_TITLE_ESC : 0));
        act_push(rq->now, actor, OP_UPDATE, (u16)id, F_TITLE, 0, 0); changed++;
    }
    if (in.has_desc) {
        if (in.dlen == 0) { it->desc_len = 0; }
        else {
            int rc = str_replace(&it->desc_off, &it->desc_len, in.desc, in.dlen, T_DESC_MAX);
            if (rc < 0) { index_add_issue(id); return resp_error(507, "insufficient_storage", "string arena is full", 0, 0); }
        }
        it->flags = (u8)((it->flags & ~ISSUE_DESC_ESC) | (in.dlen && k32_jw_needs_escape(in.desc, in.dlen) ? ISSUE_DESC_ESC : 0));
        act_push(rq->now, actor, OP_UPDATE, (u16)id, F_DESCRIPTION, 0, 0); changed++;
    }
    if (text_changed) index_add_issue(id);
    if (in.status >= 0 && in.status != it->status) {
        u8 old = it->status;
        issue_set_status(it, (u8)in.status, rq->now);
        act_push(rq->now, actor, OP_UPDATE, (u16)id, F_STATUS, old, in.status); changed++;
    }
    if (in.priority >= 0 && in.priority != it->priority) {
        act_push(rq->now, actor, OP_UPDATE, (u16)id, F_PRIORITY, it->priority, in.priority);
        it->priority = (u8)in.priority; changed++;
    }
    if (in.assignee != -3) {
        u8 na = in.assignee < 0 ? T_NO_USER : (u8)in.assignee;
        if (na != it->assignee) {
            act_push(rq->now, actor, OP_UPDATE, (u16)id, F_ASSIGNEE, it->assignee == T_NO_USER ? -1 : it->assignee, na == T_NO_USER ? -1 : na);
            it->assignee = na; changed++;
        }
    }
    if (in.has_labels && in.label_mask != it->labels) {
        act_push(rq->now, actor, OP_UPDATE, (u16)id, F_LABELS, it->labels, in.label_mask);
        it->labels = in.label_mask; changed++;
    }
    if (in.estimate >= 0 && in.estimate != it->estimate) {
        act_push(rq->now, actor, OP_UPDATE, (u16)id, F_ESTIMATE, it->estimate, in.estimate);
        it->estimate = (u8)in.estimate; changed++;
    }
    if (changed) {
        it->updated = ts_pack(rq->now);
        events_issue("update", id, actor);
    }
    k32_jw w;
    resp_begin_json(&w, 200);
    write_issue(&w, id, it, 1);
    resp_finish(&w);
    return 200;
}

static int h_issue_delete(req_t* rq, u32 id) {
    issue_t* it = issue_by_id(id);
    if (!it) return resp_error(404, "not_found", "issue does not exist", 0, 0);
    index_remove_issue(id);
    events_issue("delete", id, (u8)rq->user);
    act_push(rq->now, (u8)rq->user, OP_DELETE, (u16)id, F_NONE, 0, 0);
    it->flags |= ISSUE_DELETED;
    it->updated = ts_pack(rq->now);
    g.issue_live--;
    g.comment_live -= it->comment_count;
    if (g.last_viewed == id) g.last_viewed = 0;
    return resp_empty(204);
}

static int h_comment_create(req_t* rq, u32 id) {
    if (!issue_by_id(id)) return resp_error(404, "not_found", "issue does not exist", 0, 0);
    k32_jfield f[MAX_FIELDS]; int n;
    if (parse_body(rq, f, &n) < 0) return 400;
    const k32_jfield* b = k32_jf_get(f, n, "body");
    if (!b || b->type != K32_J_STR) return resp_error(400, "validation", "body is required", 0, 0);
    if (b->slen < 1 || b->slen > T_COMMENT_MAX) return resp_error(400, "validation", "body must be 1..280 characters", 0, 0);
    if (g.comment_count >= g.comment_cap) return resp_error(507, "insufficient_storage", "comment table is full", 0, 0);
    int cid = issue_add_comment_indexed(id, (u8)rq->user, b->s, b->slen, rq->now);
    if (cid < 0) return resp_error(507, "insufficient_storage", "could not store comment", 0, 0);
    k32_jw w;
    resp_begin_json(&w, 201);
    write_comment(&w, (u32)cid, comment_by_id((u32)cid));
    resp_finish(&w);
    return 201;
}

static int h_board(req_t* rq) {
    u32 project = 0;
    if (q_u32(rq, "project", &project) && project > T_PROJECTS_N) return resp_error(400, "validation", "unknown project", 0, 0);
    /* one bounded top-BOARD_LIMIT selection per column, filled in a single scan */
    u16* cand = (u16*)scratch_alloc(T_STATUS_N * BOARD_LIMIT * sizeof(u16));
    if (!cand) return resp_error(500, "internal", "out of scratch", 0, 0);
    sort_ctx sc = { SORT_BOARD, 1, 0 };
    k32_topk col[T_STATUS_N]; u32 counts[T_STATUS_N] = { 0 }; skey_t kth[T_STATUS_N];
    memset(kth, 0, sizeof kth);
    for (int s = 0; s < T_STATUS_N; s++) k32_topk_init(&col[s], cand + s * BOARD_LIMIT, BOARD_LIMIT, cmp_issue, &sc);
    const issue_t* issues = g.issues;
    for (u32 id = g.issue_count; id >= 1; id--) {      /* newest first: board order is updated desc within a priority */
        K32_PROF(loop_iters); K32_PROF(scan_issues);
        const issue_t* it = &issues[id - 1];
        if ((it->flags & ISSUE_DELETED) || (project && it->project != project) || it->status >= T_STATUS_N) continue;
        u8 st = it->status; counts[st]++;
        k32_topk* t = &col[st];
        if (t->n == t->k) { skey_t c = sort_key(SORT_BOARD, it, (u16)id); if (!beats_kth(1, &c, &kth[st])) continue; }
        k32_topk_push(t, (u16)id);
        if (t->n == t->k) { u16 last = t->a[t->n - 1]; kth[st] = sort_key(SORT_BOARD, &issues[last - 1], last); }
    }
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_keylit(&w, "columns"); k32_jw_obj_begin(&w);
    for (int s = 0; s < T_STATUS_N; s++) {
        k32_jw_key(&w, t_status_names[s]); k32_jw_arr_begin(&w);
        for (u32 i = 0; i < col[s].n; i++) write_issue(&w, col[s].a[i], &g.issues[col[s].a[i] - 1], 0);
        k32_jw_arr_end(&w);
    }
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "truncated"); k32_jw_obj_begin(&w);
    for (int s = 0; s < T_STATUS_N; s++) if (counts[s] > BOARD_LIMIT) { k32_jw_key(&w, t_status_names[s]); k32_jw_bool(&w, 1); }
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "limit"); k32_jw_int(&w, BOARD_LIMIT);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

static int h_search(req_t* rq) {
    const char* s; u32 sl;
    char* qb = (char*)scratch_alloc(256);
    if (!qb) return resp_error(500, "internal", "out of scratch", 0, 0);
    u32 ql = 0;
    if (q_str(rq, "q", &s, &sl)) ql = k32_url_decode(s, sl, qb, 255);
    u32 limit = SEARCH_LIMIT_DEFAULT;
    if (q_u32(rq, "limit", &limit)) { if (limit == 0) limit = 1; if (limit > LIST_LIMIT_MAX) limit = LIST_LIMIT_MAX; }
    u16* ids; i32* scores; u32 scanned = 0;
    int hits = index_search(qb, ql, &ids, &scores, &scanned, rq->now);
    if (hits < 0) return resp_error(500, "internal", "out of scratch", 0, 0);
    sort_ctx sc = { SORT_SCORE, 1, scores };
    /* only the first `limit` hits by score are returned: bounded selection */
    u16* best = (u16*)scratch_alloc(limit * sizeof(u16));
    if (!best) return resp_error(500, "internal", "out of scratch", 0, 0);
    k32_topk top; k32_topk_init(&top, best, limit, cmp_issue, &sc);
    for (u32 i = 0; i < (u32)hits; i++) k32_topk_push(&top, ids[i]);
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "items"); k32_jw_arr_begin(&w);
    for (u32 i = 0; i < top.n; i++) {
        u16 hid = best[i];
        const issue_t* it = &g.issues[hid - 1];
        k32_jw_obj_begin(&w);
        k32_jw_keylit(&w, "key"); write_key(&w, hid);
        k32_jw_keylit(&w, "title"); write_text(&w, str_at(it->title_off), it->title_len, it->flags & ISSUE_TITLE_ESC);
        k32_jw_keylit(&w, "status"); k32_jw_cstr(&w, t_status_names[it->status < T_STATUS_N ? it->status : 0]);
        k32_jw_keylit(&w, "priority"); k32_jw_int(&w, it->priority);
        k32_jw_keylit(&w, "assignee"); write_assignee(&w, it);
        k32_jw_keylit(&w, "updated"); k32_jw_int(&w, ts_ms(it->updated));
        k32_jw_keylit(&w, "score"); k32_jw_int(&w, scores[hid]);
        k32_jw_obj_end(&w);
    }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "took_us"); k32_jw_int(&w, (k32_time_ms() - rq->t0_ms) * 1000);
    k32_jw_keylit(&w, "scanned"); k32_jw_int(&w, scanned);
    k32_jw_keylit(&w, "total"); k32_jw_int(&w, hits);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

static int h_analytics(req_t* rq) {
    u32 project = 0, days = 14;
    if (q_u32(rq, "project", &project) && project > T_PROJECTS_N) return resp_error(400, "validation", "unknown project", 0, 0);
    if (q_u32(rq, "days", &days)) { if (days < 1) days = 1; if (days > 90) days = 90; }
    u32* created_pd = (u32*)scratch_alloc(days * sizeof(u32));
    u32* resolved_pd = (u32*)scratch_alloc(days * sizeof(u32));
    u32* cycle = (u32*)scratch_alloc(g.issue_cap * sizeof(u32));
    if (!created_pd || !resolved_pd || !cycle) return resp_error(500, "internal", "out of scratch", 0, 0);
    memset(created_pd, 0, days * sizeof(u32)); memset(resolved_pd, 0, days * sizeof(u32));
    u32 by_status[T_STATUS_N] = { 0 }, by_prio[T_PRIO_N] = { 0 };
    u32 open_by_user[T_USERS_N] = { 0 }, done_by_user[T_USERS_N] = { 0 };
    u32 age[4] = { 0 }; u32 ncycle = 0;
    for (u32 id = 1; id <= g.issue_count; id++) {
        const issue_t* it = issue_by_id(id);
        if (!it || (project && it->project != project)) continue;
        if (it->status < T_STATUS_N) by_status[it->status]++;
        if (it->priority < T_PRIO_N) by_prio[it->priority]++;
        int done = it->status == ST_DONE;
        if (it->assignee < T_USERS_N) { if (done) done_by_user[it->assignee]++; else open_by_user[it->assignee]++; }
        i64 d = (rq->now - ts_ms(it->created)) / T_DAY_MS;
        if (d >= 0 && d < (i64)days) created_pd[days - 1 - (u32)d]++;
        if (done) {
            i64 rd = (rq->now - ts_ms(it->resolved)) / T_DAY_MS;
            if (rd >= 0 && rd < (i64)days) resolved_pd[days - 1 - (u32)rd]++;
            i64 h = ((i64)it->resolved - (i64)it->created) / 3600ll;
            if (h < 0) h = 0;
            cycle[ncycle++] = h > 0xFFFFFFFFll ? 0xFFFFFFFFu : (u32)h;
        } else {
            i64 a = rq->now - ts_ms(it->created);
            if (a < T_DAY_MS) age[0]++; else if (a < 7 * T_DAY_MS) age[1]++; else if (a < 30 * T_DAY_MS) age[2]++; else age[3]++;
        }
    }
    k32_sort_u32(cycle, ncycle);
    u32 p50 = ncycle ? cycle[(ncycle - 1) * 50 / 100] : 0;
    u32 p90 = ncycle ? cycle[(ncycle - 1) * 90 / 100] : 0;
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_keylit(&w, "by_status"); k32_jw_obj_begin(&w);
    for (int i = 0; i < T_STATUS_N; i++) { k32_jw_key(&w, t_status_names[i]); k32_jw_int(&w, by_status[i]); }
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "by_priority"); k32_jw_arr_begin(&w);
    for (int i = 0; i < T_PRIO_N; i++) k32_jw_int(&w, by_prio[i]);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "by_assignee"); k32_jw_arr_begin(&w);
    for (int u = 1; u < T_USERS_N; u++) {
        k32_jw_obj_begin(&w);
        k32_jw_keylit(&w, "user"); k32_jw_int(&w, u);
        k32_jw_keylit(&w, "open"); k32_jw_int(&w, open_by_user[u]);
        k32_jw_keylit(&w, "done"); k32_jw_int(&w, done_by_user[u]);
        k32_jw_obj_end(&w);
    }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "created_per_day"); k32_jw_arr_begin(&w);
    for (u32 i = 0; i < days; i++) k32_jw_int(&w, created_pd[i]);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "resolved_per_day"); k32_jw_arr_begin(&w);
    for (u32 i = 0; i < days; i++) k32_jw_int(&w, resolved_pd[i]);
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "cycle_time_hours"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "p50"); k32_jw_int(&w, p50);
    k32_jw_keylit(&w, "p90"); k32_jw_int(&w, p90);
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "open_age_buckets"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "lt1d"); k32_jw_int(&w, age[0]);
    k32_jw_keylit(&w, "lt7d"); k32_jw_int(&w, age[1]);
    k32_jw_keylit(&w, "lt30d"); k32_jw_int(&w, age[2]);
    k32_jw_keylit(&w, "gte30d"); k32_jw_int(&w, age[3]);
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "took_us"); k32_jw_int(&w, (k32_time_ms() - rq->t0_ms) * 1000);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

static int h_activity(req_t* rq) {
    u32 limit = ACTIVITY_LIMIT_DEFAULT;
    if (q_u32(rq, "limit", &limit)) { if (limit == 0) limit = 1; if (limit > ACTIVITY_LIMIT_MAX) limit = ACTIVITY_LIMIT_MAX; }
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "items"); k32_jw_arr_begin(&w);
    u32 avail = act_available();
    for (u32 i = 0; i < avail && i < limit; i++) { const act_t* a = act_at(i); if (a) write_activity(&w, a); }
    k32_jw_arr_end(&w);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

static int h_stats(req_t* rq) {
    u32 used = 0, limit = 0;
    if (k32_mem_stats(&used, &limit) != 0) { used = arena_used(); limit = g.arena.size; }
    u32 n = g.lat_n < T_LAT_RING ? g.lat_n : T_LAT_RING;
    u32 lat[T_LAT_RING];
    memcpy(lat, g.lat_ring, n * sizeof(u32));
    k32_sort_u32(lat, n);
    u32 p99 = n ? lat[(n - 1) * 99 / 100] : 0;
    u32 avg = g.req_total ? (u32)(g.req_sum_us / g.req_total) : 0;
    k32_jw w;
    resp_begin_json(&w, 200);
    k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "issues"); k32_jw_int(&w, g.issue_live);
    k32_jw_keylit(&w, "comments"); k32_jw_int(&w, g.comment_live);
    k32_jw_keylit(&w, "users"); k32_jw_int(&w, T_HUMAN_USERS);
    k32_jw_keylit(&w, "sessions"); k32_jw_int(&w, session_active());
    if (!seed_finished()) {                   /* present only while the data set is incomplete */
        u32 done, total; seed_progress(&done, &total);
        k32_jw_keylit(&w, "seeding"); k32_jw_obj_begin(&w);
        k32_jw_keylit(&w, "done"); k32_jw_int(&w, done);
        k32_jw_keylit(&w, "total"); k32_jw_int(&w, total);
        k32_jw_obj_end(&w);
    }
    k32_jw_keylit(&w, "index"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "terms"); k32_jw_int(&w, g.term_count);
    k32_jw_keylit(&w, "postings"); k32_jw_int(&w, g.posting_count);
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "memory"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "used"); k32_jw_int(&w, used);
    k32_jw_keylit(&w, "limit"); k32_jw_int(&w, limit);
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "arena"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "used"); k32_jw_int(&w, arena_used());
    k32_jw_keylit(&w, "size"); k32_jw_int(&w, g.arena.size);
    k32_jw_keylit(&w, "tables"); k32_jw_int(&w, g.fixed_bytes);
    k32_jw_keylit(&w, "strings"); k32_jw_int(&w, g.string_bytes);
    k32_jw_keylit(&w, "postings"); k32_jw_int(&w, g.posting_bytes);
    k32_jw_keylit(&w, "postings_leaked"); k32_jw_int(&w, g.posting_leaked);
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "requests"); k32_jw_obj_begin(&w);
    k32_jw_keylit(&w, "total"); k32_jw_int(&w, g.req_total);
    k32_jw_keylit(&w, "errors"); k32_jw_int(&w, g.req_errors);
    k32_jw_keylit(&w, "avg_us"); k32_jw_int(&w, avg);
    k32_jw_keylit(&w, "p99_us"); k32_jw_int(&w, p99);
    k32_jw_keylit(&w, "last_us"); k32_jw_int(&w, g.req_last_us);
    k32_jw_obj_end(&w);
    k32_jw_keylit(&w, "uptime_ms"); k32_jw_int(&w, rq->uptime);
    k32_jw_keylit(&w, "now"); k32_jw_int(&w, rq->now);
    k32_jw_obj_end(&w);
    resp_finish(&w);
    return 200;
}

/* ---- dispatch ------------------------------------------------------------ */
static int is(const req_t* rq, const char* m) { return k32_streq(rq->method, rq->mlen, m); }

int router_dispatch(req_t* rq) {
    const char* p = rq->path; u32 pl = rq->plen;
    /* strip one trailing slash */
    if (pl > 1 && p[pl - 1] == '/') pl--;

    /* session resolution */
    const char* tok; u32 tl;
    if (rq->cookie && cookie_get(rq, "tsid", &tok, &tl)) {
        session_t* s = session_find(tok, tl);
        if (s) {
            if (rq->uptime - s->last_seen > T_SESSION_IDLE_MS) session_drop(s);
            else { s->last_seen = rq->uptime; rq->session = s; rq->user = s->user; }
        }
    }

    if (!k32_starts_with(p, pl, "/api/", 5)) return resp_error(404, "not_found", "no such route", 0, 0);
    const char* r = p + 5; u32 rl = pl - 5;

    /* public routes */
    if (k32_streq(r, rl, "auth/login"))  return is(rq, "POST") ? h_login(rq) : resp_error(405, "method_not_allowed", "use POST", 0, 0);
    if (k32_streq(r, rl, "bootstrap"))   return is(rq, "GET") ? h_bootstrap(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);
    if (k32_streq(r, rl, "stats"))       return is(rq, "GET") ? h_stats(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);

    if (rq->user < 0) return resp_error(401, "unauthorized", "login required", 0, 0);

    if (k32_streq(r, rl, "auth/logout")) return is(rq, "POST") ? h_logout(rq) : resp_error(405, "method_not_allowed", "use POST", 0, 0);
    if (k32_streq(r, rl, "me"))          return is(rq, "GET") ? h_me(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);
    if (k32_streq(r, rl, "issues")) {
        if (is(rq, "GET")) return h_issues_list(rq);
        if (is(rq, "POST")) return h_issue_create(rq);
        return resp_error(405, "method_not_allowed", "use GET or POST", 0, 0);
    }
    if (k32_starts_with(r, rl, "issues/", 7)) {
        const char* k = r + 7; u32 kl = rl - 7;
        u32 seg = 0;
        while (seg < kl && k[seg] != '/') seg++;
        u32 id;
        int ok = issue_parse_key(k, seg, &id);
        if (seg == kl) {
            if (!ok) return resp_error(404, "not_found", "issue ", k, seg > 32 ? 32 : seg);
            if (is(rq, "GET")) return h_issue_get(rq, id);
            if (is(rq, "PATCH")) return h_issue_patch(rq, id);
            if (is(rq, "DELETE")) return h_issue_delete(rq, id);
            return resp_error(405, "method_not_allowed", "use GET, PATCH or DELETE", 0, 0);
        }
        if (k32_streq(k + seg, kl - seg, "/comments")) {
            if (!ok) return resp_error(404, "not_found", "issue ", k, seg > 32 ? 32 : seg);
            if (is(rq, "POST")) return h_comment_create(rq, id);
            return resp_error(405, "method_not_allowed", "use POST", 0, 0);
        }
        return resp_error(404, "not_found", "no such route", 0, 0);
    }
    if (k32_streq(r, rl, "board"))     return is(rq, "GET") ? h_board(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);
    if (k32_streq(r, rl, "search"))    return is(rq, "GET") ? h_search(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);
    if (k32_streq(r, rl, "analytics")) return is(rq, "GET") ? h_analytics(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);
    if (k32_streq(r, rl, "activity"))  return is(rq, "GET") ? h_activity(rq) : resp_error(405, "method_not_allowed", "use GET", 0, 0);
    return resp_error(404, "not_found", "no such route", 0, 0);
}
