/*
 * tessera-db: the record store of Tessera. Keeps the records tessera-api
 * writes (common/tlog.h) in segment files on the pod's microSD volume
 * (tdb.h) and serves them over HTTP:
 *
 *   POST /append?expect=<seq>&log=<id>   body: records with seq 0, at most 2048 bytes.
 *        200 {"first":f,"last":l,"log":"<id>"} once the records are on the card;
 *        409 when expect is not the next sequence number or log names another
 *        log, 400 for a malformed batch, 507 when the card or the log is full,
 *        503 when read-only.
 *   GET  /records?after=<seq>&max=<bytes>
 *        200 application/octet-stream: the records with seq > after, whole,
 *        at most max (default and limit 16384) bytes, from one segment. An
 *        empty body means there is nothing after `after`.
 *   GET  /stats                   state, log id, sequence range, sizes.
 *
 * Memory: linear memory stays below TDB_MEM_CAP. The stack, the data and
 * the 2 KiB file buffer are static; the request block the host hands to
 * k32_handle goes in [__heap_base, TDB_MEM_CAP).
 */
#include "tdb.h"
#include "../../common/k32_host.h"

#ifndef TDB_MEM_CAP
#define TDB_MEM_CAP (24u * 1024u)
#endif

static tdb_t g_db;
static int   g_open_rc;

/* ---- request block memory ------------------------------------------------------ */
#if defined(__wasm32__)
extern unsigned char __heap_base;
static uint8_t* heap_ptr(void) { return &__heap_base; }
static uint32_t heap_cap(void) {
    uint32_t base = (uint32_t)(uintptr_t)&__heap_base;
    return base < TDB_MEM_CAP ? TDB_MEM_CAP - base : 0;
}
static uint8_t* resolve(uint32_t ptr) { return (uint8_t*)(uintptr_t)ptr; }
static uint32_t to_abi(const uint8_t* p) { return (uint32_t)(uintptr_t)p; }
#else
/* Native builds model linear memory with an array; ABI pointers are offsets
 * into it (offset 0 stays unused: k32_alloc returns 0 for failure). */
#define NATIVE_HEAP (TDB_APPEND_MAX + 1024u)
static uint8_t g_native_mem[16 + NATIVE_HEAP];
static uint8_t* heap_ptr(void) { return g_native_mem + 16; }
static uint32_t heap_cap(void) { return NATIVE_HEAP; }
static uint8_t* resolve(uint32_t ptr) { return g_native_mem + ptr; }
static uint32_t to_abi(const uint8_t* p) { return (uint32_t)(p - g_native_mem); }
#endif

/* ---- small text helpers -------------------------------------------------------- */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static int span_eq(const char* a, uint32_t n, const char* lit) {
    uint32_t i = 0;
    for (; i < n; i++) if (lit[i] != a[i]) return 0;
    return lit[i] == 0;
}
static int parse_u32(const char* s, uint32_t n, uint32_t* out) {
    if (n == 0 || n > 10) return 0;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    if (v > 0xFFFFFFFFu) return 0;
    *out = (uint32_t)v;
    return 1;
}
/* Value of key in "a=1&b=2": 1 when present (value and length set), 0 when absent. */
static int query_get(const char* q, uint32_t qlen, const char* key, const char** v, uint32_t* vlen) {
    uint32_t kl = slen(key), i = 0;
    while (i < qlen) {
        uint32_t s = i;
        while (i < qlen && q[i] != '&') i++;
        if (i - s > kl && q[s + kl] == '=' && span_eq(q + s, kl, key)) { *v = q + s + kl + 1; *vlen = i - s - kl - 1; return 1; }
        i++;
    }
    return 0;
}
/* 1 parsed, 0 absent, -1 present but not a number */
static int query_u32(const char* q, uint32_t qlen, const char* key, uint32_t* out) {
    const char* v; uint32_t vl;
    if (!query_get(q, qlen, key, &v, &vl)) return 0;
    return parse_u32(v, vl, out) ? 1 : -1;
}
static int query_hex(const char* q, uint32_t qlen, const char* key, uint32_t* out) {
    const char* v; uint32_t vl, x = 0;
    if (!query_get(q, qlen, key, &v, &vl)) return 0;
    if (vl != 8) return -1;
    for (uint32_t i = 0; i < 8; i++) {
        char c = v[i];
        uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10) : 99;
        if (d > 15) return -1;
        x = x * 16 + d;
    }
    *out = x;
    return 1;
}

typedef struct { char b[320]; uint32_t n; } out_t;
static void o_str(out_t* o, const char* s) { while (*s && o->n < sizeof o->b) o->b[o->n++] = *s++; }
static void o_u32(out_t* o, uint32_t v) {
    char t[10]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && o->n < sizeof o->b) o->b[o->n++] = t[--k];
}
static void o_i32(out_t* o, int32_t v) { if (v < 0) { o_str(o, "-"); o_u32(o, (uint32_t)(-(int64_t)v)); } else o_u32(o, (uint32_t)v); }
static void o_hex(out_t* o, uint32_t v) {
    for (int i = 7; i >= 0; i--) { uint32_t d = (v >> (i * 4)) & 15u; if (o->n < sizeof o->b) o->b[o->n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); }
}
static void o_key(out_t* o, const char* k) { o_str(o, o->n > 1 ? ",\"" : "\""); o_str(o, k); o_str(o, "\":"); }

static void send_json(int32_t status, const out_t* o) {
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(o->b, o->n);
    k32_resp_end();
}
static int32_t send_error(int32_t status, const char* code, const char* msg, int32_t rc) {
    out_t o; o.n = 0;
    o_str(&o, "{\"error\":\""); o_str(&o, code); o_str(&o, "\",\"message\":\""); o_str(&o, msg);
    if (rc) { o_str(&o, " (rc "); o_i32(&o, rc); o_str(&o, ")"); }
    o_str(&o, "\"");
    if (status == 409) { o_str(&o, ",\"next\":"); o_u32(&o, g_db.last_seq + 1); }
    o_str(&o, "}");
    send_json(status, &o);
    return status;
}

static void log_num(const char* what, int32_t v) {
    out_t o; o.n = 0;
    o_str(&o, "tessera-db: "); o_str(&o, what); o_i32(&o, v);
    k32_log(o.b, o.n);
}

/* ---- handlers -------------------------------------------------------------------- */
static int32_t h_stats(void) {
    out_t o; o.n = 0;
    uint32_t first = g_db.nsegs ? g_db.seg_first[0] : 1;
    o_str(&o, "{");
    o_key(&o, "state"); o_str(&o, g_db.state == TDB_OK ? "\"ok\"" : "\"read-only\"");
    o_key(&o, "format"); o_u32(&o, TDB_VERSION);
    o_key(&o, "log"); o_str(&o, "\""); o_hex(&o, g_db.log_id); o_str(&o, "\"");
    o_key(&o, "first"); o_u32(&o, first);
    o_key(&o, "last"); o_u32(&o, g_db.last_seq);
    o_key(&o, "records"); o_u32(&o, g_db.last_seq >= first ? g_db.last_seq - first + 1 : 0);
    o_key(&o, "bytes"); o_u32(&o, g_db.total_bytes);
    o_key(&o, "segments"); o_u32(&o, g_db.nsegs);
    o_key(&o, "segment_bytes"); o_u32(&o, TDB_SEG_BYTES);
    o_key(&o, "recovered_bytes"); o_u32(&o, g_db.recovered_bytes);
    o_key(&o, "appends"); o_u32(&o, g_db.appends);
    o_key(&o, "append_errors"); o_u32(&o, g_db.append_errors);
    if (g_db.state != TDB_OK) { o_key(&o, "error"); o_str(&o, "\""); o_str(&o, g_db.error); o_str(&o, "\""); }
    o_str(&o, "}");
    send_json(200, &o);
    return 200;
}

static int32_t h_append(const char* q, uint32_t qlen, uint8_t* body, uint32_t blen) {
    uint32_t expect = 0, first = 0, last = 0, log = 0;
    /* expect absent: no sequence check; log absent: no log check */
    if (query_u32(q, qlen, "expect", &expect) < 0 || query_hex(q, qlen, "log", &log) < 0)
        return send_error(400, "bad_request", "expect is a sequence number, log 8 hex digits", 0);
    if (log && log != g_db.log_id) return send_error(409, "conflict", "the log on this card is a different one", 0);
    if (blen > TDB_APPEND_MAX) return send_error(413, "too_large", "append batches are at most 2048 bytes", 0);
    int rc = tdb_append(&g_db, body, blen, expect, &first, &last);
    switch (rc) {
        case 0: break;
        case TDB_E_INVAL:    return send_error(400, "bad_request", "malformed record batch", 0);
        case TDB_E_CONFLICT: return send_error(409, "conflict", "expect is not the next sequence number", 0);
        case TDB_E_FULL:     return send_error(507, "insufficient_storage", "the log has no free segment", 0);
        case TDB_E_STATE:    return send_error(503, "read_only", g_db.error, 0);
        case K32_ERR_NOSPC:  return send_error(507, "insufficient_storage", "the card is full", rc);
        default:             return send_error(500, "io_error", "write failed", rc);
    }
    out_t o; o.n = 0;
    o_str(&o, "{");
    o_key(&o, "first"); o_u32(&o, first);
    o_key(&o, "last"); o_u32(&o, last);
    o_key(&o, "log"); o_str(&o, "\""); o_hex(&o, g_db.log_id); o_str(&o, "\"");
    o_str(&o, "}");
    send_json(200, &o);
    return 200;
}

static int g_started;
static int sink(void* ctx, const uint8_t* data, uint32_t len) {
    (void)ctx;
    if (!g_started) { k32_resp_begin(200, "application/octet-stream", 24); g_started = 1; }
    return k32_resp_write(data, len) < 0 ? -1 : 0;
}
static int32_t h_records(const char* q, uint32_t qlen) {
    uint32_t after = 0, max = TDB_PAGE_MAX, bytes, records;
    int ha = query_u32(q, qlen, "after", &after), hm = query_u32(q, qlen, "max", &max);
    if (ha < 0 || hm < 0) return send_error(400, "bad_request", "after and max are numbers", 0);
    if (hm) {
        if (max < TLOG_REC_MAX) max = TLOG_REC_MAX;
        if (max > TDB_PAGE_MAX) max = TDB_PAGE_MAX;
    }
    g_started = 0;
    int rc = tdb_read(&g_db, after, max, sink, 0, &bytes, &records);
    if (g_started) { k32_resp_end(); return 200; }
    if (rc == TDB_E_CORRUPT) return send_error(500, "corrupt", "damaged record in the log", 0);
    if (rc < 0) return send_error(500, "io_error", "read failed", rc);
    k32_resp_begin(200, "application/octet-stream", 24);
    k32_resp_end();
    return 200;
}

/* ---- exports ------------------------------------------------------------------------ */
K32_EXPORT("k32_init") int32_t k32_init(void) {
    g_open_rc = tdb_open(&g_db);
    if (g_open_rc < 0) {
        log_num("cannot open the volume, rc ", g_open_rc);
        return g_open_rc;        /* no card or no volume grant: nothing to serve */
    }
    log_num("segments ", (int32_t)g_db.nsegs);
    log_num("last seq ", (int32_t)g_db.last_seq);
    if (g_db.recovered_bytes) log_num("torn bytes repaired ", (int32_t)g_db.recovered_bytes);
    if (g_db.state != TDB_OK) k32_log(g_db.error, slen(g_db.error));
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    if (size == 0 || size > heap_cap()) return 0;
    return to_abi(heap_ptr());
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    char* req = (char*)resolve(ptr);
    uint32_t i = 0;
    while (i < len && req[i] != ' ' && req[i] != '\n') i++;
    if (i >= len || req[i] != ' ') return send_error(400, "bad_request", "malformed request", 0);
    const char* method = req; uint32_t mlen = i++;
    const char* path = req + i; uint32_t ps = i;
    while (i < len && req[i] != '?' && req[i] != ' ' && req[i] != '\n') i++;
    uint32_t plen = i - ps;
    const char* q = ""; uint32_t qlen = 0;
    if (i < len && req[i] == '?') {
        q = req + ++i; uint32_t qs = i;
        while (i < len && req[i] != ' ' && req[i] != '\n') i++;
        qlen = i - qs;
    }
    /* the body starts after the empty line that ends the headers */
    while (i < len && req[i] != '\n') i++;
    uint32_t body = len;
    for (uint32_t k = i; k + 1 < len; k++) if (req[k] == '\n' && req[k + 1] == '\n') { body = k + 2; break; }
    if (i + 1 == len) body = len;

    if (span_eq(path, plen, "/append")) {
        if (!span_eq(method, mlen, "POST")) return send_error(405, "method_not_allowed", "use POST", 0);
        return h_append(q, qlen, (uint8_t*)req + body, len - body);
    }
    if (!span_eq(method, mlen, "GET")) return send_error(405, "method_not_allowed", "use GET", 0);
    if (span_eq(path, plen, "/records")) return h_records(q, qlen);
    if (span_eq(path, plen, "/stats") || span_eq(path, plen, "/")) return h_stats();
    return send_error(404, "not_found", "routes: POST /append, GET /records, GET /stats", 0);
}

#if !defined(__wasm32__)
/* Native test access to the engine state. */
tdb_t* tdb_state(void) { return &g_db; }
uint8_t* tdb_resolve(uint32_t ptr) { return resolve(ptr); }
#endif
