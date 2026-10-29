#include "storage.h"
#include "store.h"
#include "journal.h"
#include "index.h"
#include "router.h"
#include "../../common/k32_host.h"
#include "../../common/tlog.h"

#define WRITE_TIMEOUT_MS 2000u
#define READ_TIMEOUT_MS  5000u
#define RETRY_MS         2000
#define FAILED_CHECK_MS  10000
#define LOAD_CHUNK_DEFAULT 32u

static void set_error(const char* a, i64 v) {
    u32 n = 0;
    while (a[n] && n < sizeof g.stg.error - 24) { g.stg.error[n] = a[n]; n++; }
    if (v != 0) n += k32_fmt_i64(g.stg.error + n, sizeof g.stg.error - 1 - n, v);
    g.stg.error[n] = 0;
}
static void log_msg(const char* a, i64 v) {
    char buf[128]; u32 n = 0;
    const char* p = "tessera: ";
    while (*p) buf[n++] = *p++;
    while (*a && n < 100) buf[n++] = *a++;
    n += k32_fmt_i64(buf + n, sizeof buf - n, v);
    k32_log(buf, n);
}

void storage_init(void) {
    char url[STG_URL_MAX];
    int32_t n = k32_env_get("TESSERA_DB_URL", 14, url, sizeof url - 1);
    const char* dflt = "http://tessera-db";
    u32 l = 0;
    if (n > 0 && n < (int32_t)sizeof url - 1) { for (; l < (u32)n; l++) g.stg.url[l] = url[l]; }
    else { while (dflt[l]) { g.stg.url[l] = dflt[l]; l++; } }
    while (l > 0 && g.stg.url[l - 1] == '/') l--;
    g.stg.url[l] = 0;
    char v[16]; i64 chunk = LOAD_CHUNK_DEFAULT;
    n = k32_env_get("LOAD_CHUNK", 10, v, sizeof v);
    if (n > 0 && n <= (int32_t)sizeof v && k32_parse_i64(v, (u32)n, &chunk) && chunk >= 1 && chunk <= 100000) g.stg.load_chunk = (u32)chunk;
    else g.stg.load_chunk = LOAD_CHUNK_DEFAULT;
    g.stg.state = STG_INIT;
    g.stg.reachable = 0;
    g.stg.retry_at = 0;
}

/* ---- calls to tessera-db --------------------------------------------------------- */
static u32 build_url(char* out, u32 cap, const char* path, u32 a, const char* mid, u32 b) {
    u32 n = 0;
    for (const char* s = g.stg.url; *s && n < cap; s++) out[n++] = *s;
    for (const char* s = path; *s && n < cap; s++) out[n++] = *s;
    if (a != 0xFFFFFFFFu && n + 11 < cap) n += k32_fmt_u32(out + n, cap - n, a);
    if (mid) { for (const char* s = mid; *s && n < cap; s++) out[n++] = *s; if (n + 11 < cap) n += k32_fmt_u32(out + n, cap - n, b); }
    return n;
}
static int32_t call(const char* method, const char* url, u32 url_len, const u8* body, u32 blen, u8* resp, u32 cap, u32* rlen, u32 timeout) {
    static const char k_hdr[] = "Content-Type: application/octet-stream\r\n";
    int32_t rc = k32_http_call(method, (u32)strlen(method), url, url_len, body ? k_hdr : "", body ? (u32)sizeof k_hdr - 1 : 0,
                               body, blen, resp, cap, rlen, timeout);
    g.stg.reachable = rc > 0;
    if (rc < 0 || rc >= 500) g.stg.last_error = rc;
    return rc;
}
/* Reads unsigned integers (or 8-digit hex strings) from a flat JSON object. */
typedef struct { k32_jfield f[16]; int n; } json_doc;
static int json_parse(json_doc* d, char* body, u32 len) { d->n = k32_json_parse_flat(body, len, d->f, 16); return d->n >= 0; }
static int json_u32(const json_doc* d, const char* key, u32* out) {
    const k32_jfield* x = k32_jf_get(d->f, d->n, key);
    if (!x) return 0;
    if (x->type == K32_J_INT && x->i >= 0 && x->i <= 0xFFFFFFFFll) { *out = (u32)x->i; return 1; }
    if (x->type == K32_J_STR && x->slen == 8) {
        u32 v = 0;
        for (u32 i = 0; i < 8; i++) {
            char c = x->s[i];
            u32 dg = (c >= '0' && c <= '9') ? (u32)(c - '0') : (c >= 'a' && c <= 'f') ? (u32)(c - 'a' + 10) : 99;
            if (dg > 15) return 0;
            v = v * 16 + dg;
        }
        *out = v;
        return 1;
    }
    return 0;
}

/* ---- load ------------------------------------------------------------------------------ */
static void fail(const char* why, i64 v) {
    set_error(why, v);
    log_msg(why, v);
    g.stg.state = STG_FAILED;
}
static void finish_load(void) {
    index_compact();                        /* pack the posting region once everything is in */
    g.stg.state = STG_READY;
    g.stg.load_ms = k32_time_ms() - g.stg.load_started;
    g.stg.loads++;
    g.stg.error[0] = 0;
    g.stg.verify = 1;                       /* the log may have changed while it loaded */
    log_msg("loaded records=", g.stg.seq);
    log_msg("load ms=", g.stg.load_ms);
}
/* GET /stats: 1 with the last sequence number and the log id, 0 when unreachable. */
static int db_stats(u32* last, u32* log) {
    char url[96], resp[512]; u32 rl = 0;
    u32 n = build_url(url, sizeof url, "/stats", 0xFFFFFFFFu, 0, 0);
    int32_t rc = call("GET", url, n, 0, 0, (u8*)resp, sizeof resp - 1, &rl, READ_TIMEOUT_MS);
    json_doc d;
    if (rc == 200 && json_parse(&d, resp, rl) && json_u32(&d, "last", last) && json_u32(&d, "log", log)) return 1;
    set_error("tessera-db unreachable, status ", rc);
    g.stg.retry_at = k32_time_ms() + RETRY_MS;
    return 0;
}
static int load_stats(void) {
    u32 last, log;
    if (!db_stats(&last, &log)) return 0;
    g.stg.log_id = log;
    g.stg.target = last;
    g.stg.seq = 0;
    g.stg.page_len = g.stg.page_pos = 0;
    g.stg.state = STG_LOADING;
    g.stg.load_started = k32_time_ms();
    g.stg.error[0] = 0;
    log_msg("loading records=", last);
    if (last == 0) finish_load();
    return 1;
}
/* Fetches the next page into g.stg.page and checks every record in it. */
static int load_page(void) {
    char url[128]; u32 rl = 0;
    u32 n = build_url(url, sizeof url, "/records?after=", g.stg.seq, "&max=", STG_PAGE_BYTES);
    int32_t rc = call("GET", url, n, 0, 0, g.stg.page, STG_PAGE_BYTES, &rl, READ_TIMEOUT_MS);
    if (rc != 200) {
        set_error("tessera-db unreachable while loading, status ", rc);
        g.stg.retry_at = k32_time_ms() + RETRY_MS;
        return 0;
    }
    if (rl == 0) { fail("tessera-db returned no records after ", g.stg.seq); return 0; }
    /* the records before a damaged one still load; the load stops there */
    u32 pos = 0, len, expect = g.stg.seq + 1;
    g.stg.damaged = 0;
    while (pos < rl) {
        const u8* rec = g.stg.page + pos;
        if (tlog_frame(rec, rl - pos, &len) != 1 || !tlog_crc_ok(rec) ||
            (tlog_type(rec) != TLOG_PAD && tlog_seq(rec) != expect)) { g.stg.damaged = 1; break; }
        if (tlog_type(rec) != TLOG_PAD) expect++;
        pos += len;
    }
    if (pos == 0) { fail("damaged record after seq ", g.stg.seq); return 0; }
    g.stg.page_len = pos; g.stg.page_pos = 0;
    return 1;
}
/* Applies up to load_chunk records of the current page. */
static void load_apply(void) {
    u32 done = 0, before = g.issue_count;
    while (done < g.stg.load_chunk && g.stg.page_pos < g.stg.page_len) {
        const u8* rec = g.stg.page + g.stg.page_pos;
        u32 len = TLOG_HDR + tlog_len(rec);
        int rc = journal_apply(rec);
        if (rc != J_OK) { fail("cannot apply record ", tlog_seq(rec)); return; }
        if (tlog_type(rec) != TLOG_PAD) g.stg.seq = tlog_seq(rec);
        g.stg.page_pos += len;
        done++;
    }
    if (g.issue_count > before) {
        /* one create event per chunk: the HUD bumps its counter and web views refetch */
        const issue_t* last = issue_slot(g.issue_count);
        events_issue("create", g.issue_count, last ? last->reporter : 0);
    }
}

static void reset_for_reload(void);

int storage_tick(void) {
    if (g.stg.reload) { reset_for_reload(); return 1; }
    switch (g.stg.state) {
        case STG_INIT:
            if (k32_time_ms() < g.stg.retry_at) return 0;
            return load_stats();
        case STG_LOADING:
            if (g.stg.damaged && g.stg.page_pos >= g.stg.page_len) { fail("damaged record after seq ", g.stg.seq); return 0; }
            if (g.stg.page_pos < g.stg.page_len) {
                load_apply();
                if (g.stg.state == STG_LOADING && !g.stg.damaged && g.stg.page_pos >= g.stg.page_len && g.stg.seq >= g.stg.target) finish_load();
                return g.stg.state == STG_LOADING;
            }
            if (g.stg.seq >= g.stg.target) { finish_load(); return 0; }
            if (k32_time_ms() < g.stg.retry_at) return 0;
            if (!load_page()) return 0;
            load_apply();
            return 1;
        case STG_READY:
            /* after an append of unknown outcome: reload if the log is not what the store holds */
            if (g.stg.verify && k32_time_ms() >= g.stg.retry_at) {
                u32 last, log;
                if (db_stats(&last, &log)) {
                    g.stg.verify = 0;
                    if (log != g.stg.log_id || last != g.stg.seq) { g.stg.reload = 1; return 1; }
                }
            }
            return 0;
        case STG_FAILED:
            /* a damaged log stays failed until it is replaced (another log id) */
            if (k32_time_ms() >= g.stg.retry_at) {
                u32 last, log;
                char keep[sizeof g.stg.error];
                memcpy(keep, g.stg.error, sizeof keep);
                if (db_stats(&last, &log) && log != g.stg.log_id) { g.stg.reload = 1; return 1; }
                memcpy(g.stg.error, keep, sizeof keep);
                g.stg.retry_at = k32_time_ms() + FAILED_CHECK_MS;
            }
            return 0;
        default:
            return 0;
    }
}

/* The log changed under the store (another writer, a replaced card): drop
 * the data and load again. Sessions survive. */
void app_reset_store(void);
static void reset_for_reload(void) {
    session_t keep[T_SESSIONS_N];
    memcpy(keep, g.sessions, sizeof keep);
    app_reset_store();
    memcpy(g.sessions, keep, sizeof keep);
    g.stg.reload = 0;
    log_msg("reloading from tessera-db, conflicts=", g.stg.conflicts);
}

/* ---- writes ------------------------------------------------------------------------------ */
int storage_writable(void) { return g.stg.state == STG_READY && !g.stg.reload; }

const char* storage_unavailable_reason(void) {
    if (g.stg.reload) return "the log changed; reloading from tessera-db";
    switch (g.stg.state) {
        case STG_INIT: return g.stg.error[0] ? g.stg.error : "connecting to tessera-db";
        case STG_LOADING: return !g.stg.reachable && g.stg.error[0] ? g.stg.error : "loading data from tessera-db";
        case STG_FAILED: return g.stg.error[0] ? g.stg.error : "storage failed";
        default: return g.stg.error[0] ? g.stg.error : "tessera-db unavailable";
    }
}

int storage_commit(u8* rec, u32 len) {
    if (!storage_writable()) return 503;
    char url[128], resp[512]; u32 rl = 0;
    u32 n = build_url(url, sizeof url, "/append?expect=", g.stg.seq + 1, 0, 0);
    static const char k_log[] = "&log=";
    memcpy(url + n, k_log, sizeof k_log - 1); n += sizeof k_log - 1;
    k32_fmt_hex32(url + n, 8, g.stg.log_id); n += 8;
    int32_t rc = call("POST", url, n, rec, len, (u8*)resp, sizeof resp - 1, &rl, WRITE_TIMEOUT_MS);
    if (rc != 200) {
        g.stg.write_errors++;
        if (rc == 409) { g.stg.conflicts++; g.stg.reload = 1; return 503; }
        if (rc == 507) { set_error("tessera-db is full", 0); return 507; }
        /* No answer, or a failure after the bytes may have reached the card: check the
         * log on the next tick. Refusals (no ready pod, a call loop, no grant, 4xx,
         * read-only) wrote nothing. */
        if (rc != K32_ERR_NOENT && rc != K32_ERR_LOOP && rc != K32_ERR_PERM && (rc < 400 || rc >= 500) && rc != 503) { g.stg.verify = 1; g.stg.retry_at = 0; }
        set_error(rc < 0 ? "tessera-db unreachable, rc " : "tessera-db refused the write, status ", rc);
        return 503;
    }
    u32 first, log;
    json_doc d;
    if (!json_parse(&d, resp, rl) || !json_u32(&d, "first", &first) || !json_u32(&d, "log", &log) || first != g.stg.seq + 1 || log != g.stg.log_id) {
        g.stg.write_errors++; g.stg.conflicts++; g.stg.reload = 1;
        return 503;
    }
    tlog_seal(rec, first);
    int ar = journal_apply(rec);
    if (ar != J_OK) {
        /* on the card but not in memory: serve reads, refuse writes until a reload */
        fail("stored record does not apply, seq ", first);
        return 507;
    }
    g.stg.seq = first;
    g.stg.writes++;
    g.stg.error[0] = 0;
    return 0;
}

/* ---- stats ------------------------------------------------------------------------------- */
void storage_write_stats(k32_jw* w) {
    static const char* const names[] = { "connecting", "loading", "ready", "failed" };
    if (g.stg.state != STG_READY) {
        k32_jw_keylit(w, "loading"); k32_jw_obj_begin(w);
        k32_jw_keylit(w, "done"); k32_jw_int(w, g.stg.seq);
        k32_jw_keylit(w, "total"); k32_jw_int(w, g.stg.target);
        k32_jw_obj_end(w);
    }
    char hex[8];
    k32_fmt_hex32(hex, sizeof hex, g.stg.log_id);
    k32_jw_keylit(w, "storage"); k32_jw_obj_begin(w);
    k32_jw_keylit(w, "state"); k32_jw_cstr(w, names[g.stg.state < 4 ? g.stg.state : 3]);
    k32_jw_keylit(w, "reachable"); k32_jw_bool(w, g.stg.reachable);
    k32_jw_keylit(w, "log"); k32_jw_str(w, hex, 8);
    k32_jw_keylit(w, "seq"); k32_jw_int(w, g.stg.seq);
    k32_jw_keylit(w, "writes"); k32_jw_int(w, g.stg.writes);
    k32_jw_keylit(w, "write_errors"); k32_jw_int(w, g.stg.write_errors);
    k32_jw_keylit(w, "conflicts"); k32_jw_int(w, g.stg.conflicts);
    k32_jw_keylit(w, "load_ms"); k32_jw_int(w, g.stg.load_ms);
    if (g.stg.error[0]) { k32_jw_keylit(w, "error"); k32_jw_cstr(w, g.stg.error); }
    k32_jw_obj_end(w);
}
