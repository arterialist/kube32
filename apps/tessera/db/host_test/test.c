/*
 * Native tests for tessera-db: record codec, append/read round trip, paging
 * across segments, sequence checks, torn-tail recovery (failed append and
 * crash before restart), damaged logs and the request-block memory cap.
 */
#define _POSIX_C_SOURCE 200809L
#include "../src/tdb.h"
#include "../../common/k32_host.h"
#include "../../common/host/shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

tdb_t* tdb_state(void);
uint8_t* tdb_resolve(uint32_t ptr);

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } } while (0)

static char g_dir[512];

static int req(const char* method, const char* path, const uint8_t* body, uint32_t blen) {
    static uint8_t block[TDB_APPEND_MAX + 1024];
    int n = snprintf((char*)block, sizeof block, "%s %s\nContent-Type: application/octet-stream\n\n", method, path);
    if (blen > sizeof block - (uint32_t)n) return -99;
    memcpy(block + n, body, blen); n += (int)blen;
    uint32_t p = k32_alloc((uint32_t)n);
    if (!p) return -98;
    memcpy(tdb_resolve(p), block, (size_t)n);
    shim_reset_response();
    int st = k32_handle(p, (uint32_t)n);
    CHECK(shim.begin_calls == 1 && shim.end_calls == 1, "%s %s: one response (%d/%d)", method, path, shim.begin_calls, shim.end_calls);
    CHECK(st == shim.status, "%s %s: returned %d, sent %d", method, path, st, shim.status);
    CHECK(shim_fs.open_now == 0, "%s %s: %u files left open", method, path, shim_fs.open_now);
    return st;
}
static long jnum(const char* key) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(shim.body ? shim.body : "", pat);
    return p ? strtol(p + strlen(pat), 0, 10) : -999999;
}
static int init(void) { shim_reset_response(); return k32_init(); }
static char* seg_path(uint32_t i, char* out, size_t cap) {
    char name[12]; tdb_seg_name(i, name);
    snprintf(out, cap, "%s/%s", g_dir, name);
    return out;
}
static long file_size(const char* path) { struct stat st; return stat(path, &st) == 0 ? (long)st.st_size : -1; }

/* ---- a reference copy of everything appended, for round-trip checks ---------------- */
static uint8_t* g_ref; static uint32_t g_ref_len, g_ref_cap;
static uint32_t g_ref_off[20000]; static uint32_t g_ref_n;
static void ref_add(const uint8_t* recs, uint32_t len) {
    if (g_ref_len + len > g_ref_cap) { g_ref_cap = (g_ref_len + len) * 2; g_ref = realloc(g_ref, g_ref_cap); }
    uint32_t pos = 0, rl;
    while (pos < len && tlog_frame(recs + pos, len - pos, &rl) == 1) { g_ref_off[g_ref_n++] = g_ref_len + pos; pos += rl; }
    memcpy(g_ref + g_ref_len, recs, len); g_ref_len += len;
}
/* payload of reference record seq (1-based) */
static const uint8_t* ref_rec(uint32_t seq) { return g_ref + g_ref_off[seq - 1]; }

static uint32_t make_comment(uint8_t* out, uint32_t cap, uint32_t i) {
    char body[300];
    int n = snprintf(body, sizeof body, "comment %u: the upload pipeline stalls under load on staging, retry %u", i, i * 7);
    if (i % 5 == 0) { memset(body + n, 'x', 200); n += 200; }
    tlog_comment c = { i, 1 + i % 40, 1793059200000ll + i, (uint8_t)(1 + i % 8), body, (uint32_t)n };
    return tlog_put_comment(out, cap, &c);
}
/* appends `count` comment records in batches of up to `per` records */
static int append_many(uint32_t first_id, uint32_t count, uint32_t per) {
    static uint8_t batch[TDB_APPEND_MAX];
    uint32_t id = first_id, done = 0;
    while (done < count) {
        uint32_t len = 0, k = 0;
        while (k < per && done + k < count) {
            uint8_t rec[TLOG_REC_MAX];
            uint32_t rl = make_comment(rec, sizeof rec, id + k);
            if (len + rl > sizeof batch) break;
            memcpy(batch + len, rec, rl); len += rl; k++;
        }
        char path[64]; snprintf(path, sizeof path, "/append?expect=%u", tdb_state()->last_seq + 1);
        uint8_t copy[TDB_APPEND_MAX]; memcpy(copy, batch, len);
        int st = req("POST", path, batch, len);
        if (st != 200) return st;
        /* the reference keeps the sealed form the store wrote */
        uint32_t seq = (uint32_t)jnum("first"), pos = 0;
        for (uint32_t j = 0; j < k; j++) { tlog_seal(copy + pos, seq + j); pos += TLOG_HDR + tlog_len(copy + pos); }
        ref_add(copy, len);
        id += k; done += k;
    }
    return 200;
}

/* reads everything after `after` page by page; checks every page against the reference */
static uint32_t read_all(uint32_t after, uint32_t max, int* ok, uint32_t* pages) {
    uint32_t last = tdb_state()->last_seq;
    *pages = 0;
    while (after < last) {
        char path[96]; snprintf(path, sizeof path, "/records?after=%u&max=%u", after, max);
        int st = req("GET", path, 0, 0);
        if (st != 200 || shim.body_len == 0 || strcmp(shim.content_type, "application/octet-stream")) { *ok = 0; break; }
        if (shim.body_len > (max < TLOG_REC_MAX ? TLOG_REC_MAX : max)) *ok = 0;
        (*pages)++;
        const uint8_t* b = (const uint8_t*)shim.body;
        uint32_t pos = 0, rl;
        while (pos < shim.body_len) {
            if (tlog_frame(b + pos, shim.body_len - pos, &rl) != 1 || !tlog_crc_ok(b + pos)) { *ok = 0; return after; }
            uint32_t seq = tlog_seq(b + pos);
            if (seq != after + 1 || tlog_type(b + pos) == TLOG_PAD) { *ok = 0; return after; }
            if (memcmp(b + pos, ref_rec(seq), rl) != 0) { *ok = 0; return after; }
            after = seq; pos += rl;
        }
    }
    return after;
}

static void test_codec(void) {
    CHECK(tlog_crc32(0, "123456789", 9) == 0xCBF43926u, "crc32 check value %08x", tlog_crc32(0, "123456789", 9));
    uint8_t rec[TLOG_REC_MAX]; uint32_t rl;
    char desc[1000]; memset(desc, 'd', sizeof desc);
    tlog_issue in = { 42, 1793059200123ll, 2, 1, 3, TLOG_NO_USER, 5, 8, 0x0a05, "Upload \"stalls\"", 15, desc, 1000 };
    uint32_t n = tlog_put_issue(rec, sizeof rec, &in);
    CHECK(n == TLOG_HDR + 4 + 8 + 6 + 2 + 3 + 15 + 1000, "issue record length %u", n);
    CHECK(tlog_frame(rec, n, &rl) == 1 && rl == n && tlog_type(rec) == TLOG_ISSUE_CREATE && tlog_crc_ok(rec), "issue framing");
    tlog_issue out;
    CHECK(tlog_get_issue(tlog_payload(rec), tlog_len(rec), &out) == 0 && out.id == 42 && out.created == in.created && out.project == 2 &&
          out.status == 1 && out.priority == 3 && out.assignee == TLOG_NO_USER && out.reporter == 5 && out.estimate == 8 && out.labels == 0x0a05 &&
          out.title_len == 15 && !memcmp(out.title, in.title, 15) && out.desc_len == 1000 && !memcmp(out.desc, desc, 1000), "issue round trip");
    CHECK(tlog_get_issue(tlog_payload(rec), tlog_len(rec) - 1, &out) < 0, "truncated issue rejected");
    CHECK(tlog_put_issue(rec, 100, &in) == 0, "short buffer rejected");

    tlog_update u = { 7, -5, 3, 3, { { TLOG_F_TITLE, 0, 0, "new title", 9 }, { TLOG_F_ASSIGNEE, -1, 4, "", 0 }, { TLOG_F_LABELS, 3, 0x801, "", 0 } } };
    n = tlog_put_update(rec, sizeof rec, &u);
    tlog_update uo;
    CHECK(n && tlog_get_update(tlog_payload(rec), tlog_len(rec), &uo) == 0 && uo.id == 7 && uo.ts == -5 && uo.actor == 3 && uo.count == 3 &&
          uo.ch[0].field == TLOG_F_TITLE && uo.ch[0].len == 9 && !memcmp(uo.ch[0].text, "new title", 9) &&
          uo.ch[1].field == TLOG_F_ASSIGNEE && uo.ch[1].from == -1 && uo.ch[1].to == 4 && uo.ch[2].to == 0x801, "update round trip");
    u.count = 0;
    CHECK(tlog_put_update(rec, sizeof rec, &u) == 0, "empty update rejected");

    tlog_comment c = { 9, 42, 1793059200999ll, 0, "hi \xc3\xa9", 5 }, co;
    n = tlog_put_comment(rec, sizeof rec, &c);
    CHECK(n && tlog_get_comment(tlog_payload(rec), tlog_len(rec), &co) == 0 && co.id == 9 && co.issue == 42 && co.ts == c.ts && co.author == 0 &&
          co.len == 5 && !memcmp(co.body, c.body, 5), "comment round trip");
    tlog_delete d = { 3, 1793059200000ll, 6 }, dd;
    n = tlog_put_delete(rec, sizeof rec, &d);
    CHECK(n == TLOG_HDR + 13 && tlog_get_delete(tlog_payload(rec), tlog_len(rec), &dd) == 0 && dd.id == 3 && dd.ts == d.ts && dd.actor == 6, "delete round trip");

    rec[TLOG_HDR] ^= 1;
    CHECK(!tlog_crc_ok(rec), "crc catches a flipped bit");
    CHECK(tlog_put_pad(rec, 11) == 0 && tlog_put_pad(rec, 12) == 12 && tlog_type(rec) == TLOG_PAD && tlog_crc_ok(rec) && tlog_seq(rec) == 0, "pad record");
    uint8_t bad[TLOG_HDR] = { 0xff, 0xff, 3, 0 };
    CHECK(tlog_frame(bad, sizeof bad, &rl) == -1, "oversized length is not a record");
    CHECK(tlog_frame(rec, 5, &rl) == 0, "partial header asks for more");
}

static void test_fresh_log(void) {
    shim_fs_reset_dir(g_dir);
    CHECK(init() == 0, "init on an empty volume");
    char p[600];
    CHECK(file_size(seg_path(0, p, sizeof p)) == TDB_SEG_HDR, "segment 0 created with its header");
    CHECK(req("GET", "/stats", 0, 0) == 200, "stats");
    CHECK(strstr(shim.body, "\"state\":\"ok\"") && jnum("last") == 0 && jnum("records") == 0 && jnum("segments") == 1 && jnum("bytes") == TDB_SEG_HDR, "empty stats: %s", shim.body);
    CHECK(req("GET", "/records?after=0", 0, 0) == 200 && shim.body_len == 0, "nothing to read");
    CHECK(req("GET", "/nope", 0, 0) == 404, "unknown route");
    CHECK(req("DELETE", "/append", 0, 0) == 405, "append needs POST");
    CHECK(req("POST", "/append", (const uint8_t*)"junk", 4) == 400, "malformed batch");
    uint8_t big[TDB_APPEND_MAX + 8]; memset(big, 0, sizeof big);
    CHECK(req("POST", "/append", big, sizeof big) == 413, "batch over 4096 bytes");
    uint8_t rec[TLOG_REC_MAX];
    uint32_t n = make_comment(rec, sizeof rec, 1);
    rec[2] = TLOG_PAD; tlog_seal(rec, 0);
    CHECK(req("POST", "/append", rec, n) == 400, "store-reserved record type rejected");
    CHECK(req("POST", "/append?expect=5", rec, make_comment(rec, sizeof rec, 1)) == 409 && jnum("next") == 1, "expect checked: %s", shim.body);
    CHECK(tdb_state()->last_seq == 0, "rejected appends leave the log alone");
}

static void test_append_read(void) {
    g_ref_len = 0; g_ref_n = 0;
    CHECK(append_many(1, 3, 3) == 200, "first batch");
    CHECK(jnum("first") == 1 && jnum("last") == 3 && strstr(shim.body, "\"log\":\""), "assigned sequence numbers: %s", shim.body);
    int ok = 1; uint32_t pages;
    CHECK(read_all(0, TDB_PAGE_MAX, &ok, &pages) == 3 && ok && pages == 1, "read back 3 records");
    /* fill several segments, batches of 1..9 records */
    uint32_t id = 4;
    for (uint32_t b = 0; b < 400; b++) {
        uint32_t k = 1 + b % 9;
        if (append_many(id, k, k) != 200) { CHECK(0, "append batch %u", b); break; }
        id += k;
    }
    tdb_t* db = tdb_state();
    CHECK(db->last_seq == id - 1 && db->nsegs > 5, "%u records in %u segments", db->last_seq, db->nsegs);
    uint32_t sizes = 0;
    for (uint32_t i = 0; i < db->nsegs; i++) {
        char p[600]; long sz = file_size(seg_path(i, p, sizeof p));
        CHECK(sz > 0 && sz <= (long)TDB_SEG_BYTES, "segment %u size %ld within the limit", i, sz);
        sizes += (uint32_t)sz;
    }
    CHECK(sizes == db->total_bytes, "byte count matches the files (%u vs %u)", sizes, db->total_bytes);
    static const uint32_t maxes[] = { 1, TLOG_REC_MAX, 5000, TDB_PAGE_MAX, 1000000 };
    for (int m = 0; m < 5; m++) {
        ok = 1;
        uint32_t got = read_all(0, maxes[m], &ok, &pages);
        CHECK(ok && got == db->last_seq, "paged read with max=%u: %u records in %u pages", maxes[m], got, pages);
    }
    /* reading from the middle, and the page never crosses a segment */
    ok = 1;
    CHECK(read_all(db->seg_first[2] - 2, TDB_PAGE_MAX, &ok, &pages) == db->last_seq && ok, "read from the middle");
    char path[64]; snprintf(path, sizeof path, "/records?after=%u", db->seg_first[1] - 2);
    req("GET", path, 0, 0);
    uint32_t pos = 0, rl, cnt = 0;
    while (pos < shim.body_len && tlog_frame((uint8_t*)shim.body + pos, shim.body_len - pos, &rl) == 1) { pos += rl; cnt++; }
    CHECK(cnt == 1, "a page ends at its segment end (%u records)", cnt);
    snprintf(path, sizeof path, "/records?after=%u", db->last_seq);
    CHECK(req("GET", path, 0, 0) == 200 && shim.body_len == 0, "after the last record: empty page");
    snprintf(path, sizeof path, "/records?after=%u", db->last_seq + 50);
    CHECK(req("GET", path, 0, 0) == 200 && shim.body_len == 0, "past the end: empty page");
    /* reopening keeps everything */
    uint32_t last = db->last_seq, log_id = db->log_id, bytes = db->total_bytes;
    CHECK(init() == 0 && db->last_seq == last && db->log_id == log_id && db->total_bytes == bytes && db->recovered_bytes == 0 && db->state == TDB_OK,
          "reopen: last %u log %08x bytes %u", db->last_seq, db->log_id, db->total_bytes);
    ok = 1;
    CHECK(read_all(0, TDB_PAGE_MAX, &ok, &pages) == last && ok, "reopened log reads back the same records");
    CHECK(req("GET", "/stats", 0, 0) == 200 && jnum("last") == (long)last && jnum("records") == (long)last && jnum("segments") == (long)db->nsegs, "stats after reopen");
}

static void test_torn_tail(void) {
    tdb_t* db = tdb_state();
    uint32_t last = db->last_seq;
    /* an append that fails part way leaves bytes behind; the next append repairs them */
    shim_fs.fail_write_after = 50;
    uint8_t rec[TLOG_REC_MAX];
    uint32_t n = make_comment(rec, sizeof rec, 90001);
    CHECK(req("POST", "/append", rec, n) == 500 && db->needs_recovery && db->last_seq == last, "failed append reports an I/O error");
    char p[600]; long torn_size = file_size(seg_path(db->nsegs - 1, p, sizeof p));
    CHECK(torn_size == (long)db->tail_end + 50, "50 torn bytes on the card");
    CHECK(append_many(90001, 1, 1) == 200 && jnum("first") == (long)last + 1, "next append repairs and continues at %u", last + 1);
    CHECK(db->recovered_bytes == 50, "50 bytes covered with padding (%u)", db->recovered_bytes);
    int ok = 1; uint32_t pages;
    CHECK(read_all(0, TDB_PAGE_MAX, &ok, &pages) == last + 1 && ok, "padding never reaches readers");

    /* a crash in the middle of an append: garbage at the end of the file before the restart */
    last = db->last_seq;
    for (int garbage = 1; garbage <= 2000; garbage += 666) {
        seg_path(db->nsegs - 1, p, sizeof p);
        FILE* f = fopen(p, "ab");
        uint8_t half[TLOG_REC_MAX]; uint32_t hl = make_comment(half, sizeof half, 7);
        for (int i = 0; i < garbage; i++) fputc(i < (int)hl ? half[i] : 0xA5, f);
        fclose(f);
        CHECK(init() == 0 && db->state == TDB_OK && db->last_seq == last && db->recovered_bytes == (uint32_t)garbage,
              "restart after %d torn bytes: last %u, repaired %u", garbage, db->last_seq, db->recovered_bytes);
        CHECK(file_size(p) == (long)db->tail_end, "the file ends where the log does");
        CHECK(append_many(91000 + (uint32_t)garbage, 2, 2) == 200 && jnum("first") == (long)last + 1, "appends continue after the repair");
        last = db->last_seq;
        ok = 1;
        CHECK(read_all(0, TDB_PAGE_MAX, &ok, &pages) == last && ok, "every record reads back after the repair");
        CHECK(init() == 0 && db->recovered_bytes == 0 && db->last_seq == last, "the repair is permanent");
    }
    /* a segment cut off while its header was written */
    seg_path(db->nsegs, p, sizeof p);
    FILE* f = fopen(p, "wb"); fwrite("TSDB\x01", 1, 5, f); fclose(f);
    uint32_t nsegs = db->nsegs;
    CHECK(init() == 0 && db->state == TDB_OK && db->nsegs == nsegs && file_size(p) == -1, "half-written segment header removed");
}

static void test_damage(void) {
    tdb_t* db = tdb_state();
    char p[600];
    /* flip a byte early in a full segment that is not the last: reads report it, the tail stays writable */
    seg_path(1, p, sizeof p);
    FILE* f = fopen(p, "r+b"); fseek(f, TDB_SEG_HDR + 20, SEEK_SET); int c = fgetc(f); fseek(f, TDB_SEG_HDR + 20, SEEK_SET); fputc(c ^ 0x40, f); fclose(f);
    CHECK(init() == 0 && db->state == TDB_OK, "damage outside the last segment is not seen at open");
    char path[64]; snprintf(path, sizeof path, "/records?after=%u", db->seg_first[1] - 1);
    int st = req("GET", path, 0, 0);
    /* the damaged byte sits in a payload: framing holds, the reader's CRC check catches it */
    const uint8_t* b = (const uint8_t*)shim.body;
    CHECK(st == 200 && shim.body_len > 0 && !tlog_crc_ok(b), "a damaged payload reaches the client, whose CRC check fails");
    f = fopen(p, "r+b"); fseek(f, TDB_SEG_HDR + 20, SEEK_SET); fputc(c, f); fclose(f);

    /* damage deep inside the last segment: more than one append of bad bytes behind it */
    uint32_t id = 95000;
    while (db->tail_end < TDB_SEG_HDR + 3 * TDB_APPEND_MAX) { if (append_many(id, 5, 5) != 200) break; id += 5; }
    seg_path(db->nsegs - 1, p, sizeof p);
    f = fopen(p, "r+b"); fseek(f, TDB_SEG_HDR + 4, SEEK_SET); int seq0 = fgetc(f); fseek(f, TDB_SEG_HDR + 4, SEEK_SET); fputc(seq0 ^ 0x77, f); fclose(f);
    uint32_t last = db->last_seq;
    CHECK(init() == 0 && db->state == TDB_FAILED && strstr(db->error, "damaged"), "damaged committed records make the log read-only (%s)", db->error);
    uint8_t rec[TLOG_REC_MAX];
    CHECK(req("POST", "/append", rec, make_comment(rec, sizeof rec, 1)) == 503, "read-only log refuses appends");
    CHECK(req("GET", "/stats", 0, 0) == 200 && strstr(shim.body, "\"state\":\"read-only\"") && strstr(shim.body, "\"error\":\""), "stats say why: %s", shim.body);
    CHECK(db->last_seq < last, "records after the damage are not served as committed");
    f = fopen(p, "r+b"); fseek(f, TDB_SEG_HDR + 4, SEEK_SET); fputc(seq0, f); fclose(f);
    CHECK(init() == 0 && db->state == TDB_OK && db->last_seq == last, "the repaired byte brings the log back");
}

/* Padding left in a segment that later fills up: readers skip it across the
 * segment boundary, and paging still returns every record exactly once. */
static void test_pad_then_rollover(void) {
    shim_fs_reset_dir(g_dir);
    g_ref_len = 0; g_ref_n = 0;
    CHECK(init() == 0, "fresh log");
    tdb_t* db = tdb_state();
    uint32_t id = 1;
    CHECK(append_many(id, 20, 5) == 200, "first records"); id += 20;
    shim_fs.fail_write_after = 30;
    uint8_t rec[TLOG_REC_MAX];
    CHECK(req("POST", "/append", rec, make_comment(rec, sizeof rec, 777)) == 500, "torn append");
    /* fill past the end of the segment so the padding ends up inside a full segment */
    while (db->nsegs < 3) { if (append_many(id, 7, 7) != 200) { CHECK(0, "append"); break; } id += 7; }
    CHECK(db->recovered_bytes == 30, "torn bytes padded (%u)", db->recovered_bytes);
    int ok = 1; uint32_t pages;
    CHECK(read_all(0, TDB_PAGE_MAX, &ok, &pages) == db->last_seq && ok, "all records once, padding skipped");
    for (uint32_t after = 0; after < db->last_seq; after += 13) {
        char path[64]; snprintf(path, sizeof path, "/records?after=%u&max=%u", after, TLOG_REC_MAX);
        req("GET", path, 0, 0);
        uint32_t rl;
        CHECK(shim.body_len > 0 && tlog_frame((uint8_t*)shim.body, shim.body_len, &rl) == 1 && tlog_seq((uint8_t*)shim.body) == after + 1,
              "a page after %u starts at %u", after, after + 1);
    }
    CHECK(init() == 0 && db->state == TDB_OK && db->recovered_bytes == 0, "reopen after rollover finds nothing to repair");
}

/* A failed rollover leaves no segment behind; damage just before the end is
 * not mistaken for a torn append; malformed parameters are refused. */
static void test_rollover_damage_params(void) {
    shim_fs_reset_dir(g_dir);
    g_ref_len = 0; g_ref_n = 0;
    CHECK(init() == 0, "fresh log");
    tdb_t* db = tdb_state();
    uint32_t id = 1;
    while (db->tail_end < TDB_SEG_BYTES - 400) { if (append_many(id, 1, 1) != 200) { CHECK(0, "append"); break; } id++; }
    /* a record too big for what is left forces a new segment; its header write fails */
    uint8_t big[TLOG_REC_MAX]; char text[1000]; memset(text, 'q', sizeof text);
    tlog_issue is = { 1, 1793059200000ll, 1, 0, 0, TLOG_NO_USER, 1, 0, 0, "big", 3, text, 1000 };
    uint32_t bn = tlog_put_issue(big, sizeof big, &is);
    shim_fs.fail_write_after = 10;
    CHECK(req("POST", "/append", big, bn) == 500 && db->nsegs == 1, "rollover fails");
    char p[600];
    CHECK(file_size(seg_path(1, p, sizeof p)) == -1 && !db->roll_pending, "the half-made segment is removed");
    CHECK(append_many(id, 1, 1) == 200, "a small record still fits the current segment"); id++;
    uint32_t last = db->last_seq;
    CHECK(init() == 0 && db->state == TDB_OK && db->last_seq == last && db->nsegs == 1, "reopen sees every record (%u of %u)", db->last_seq, last);
    CHECK(req("POST", "/append", big, bn) == 200 && db->nsegs == 2, "the next rollover works");
    /* damage in a record near the end, good records after it: read-only, nothing padded over */
    for (int k = 0; k < 6; k++) { if (append_many(id, 1, 1) != 200) CHECK(0, "append"); id++; }
    last = db->last_seq;
    seg_path(db->nsegs - 1, p, sizeof p);
    long sz = file_size(p);
    FILE* f = fopen(p, "r+b"); fseek(f, sz - 300, SEEK_SET); int c = fgetc(f); fseek(f, sz - 300, SEEK_SET); fputc(c ^ 0x01, f); fclose(f);
    CHECK(init() == 0 && db->state == TDB_FAILED && db->recovered_bytes == 0 && file_size(p) == sz, "damage before good records is not repaired as a torn tail (%s)", db->error);
    f = fopen(p, "r+b"); fseek(f, sz - 300, SEEK_SET); fputc(c, f); fclose(f);
    CHECK(init() == 0 && db->state == TDB_OK && db->last_seq == last, "undamaged again");
    /* parameters */
    uint8_t rec[TLOG_REC_MAX]; uint32_t n = make_comment(rec, sizeof rec, 5);
    CHECK(req("POST", "/append?expect=abc", rec, n) == 400, "malformed expect");
    CHECK(req("POST", "/append?log=xyz", rec, n) == 400, "malformed log id");
    char path[96]; snprintf(path, sizeof path, "/append?log=%08x", db->log_id ^ 1u);
    CHECK(req("POST", path, rec, n) == 409, "another log's id is a conflict");
    snprintf(path, sizeof path, "/append?expect=%u&log=%08x", db->last_seq + 1, db->log_id);
    CHECK(req("POST", path, rec, n) == 200, "matching expect and log");
    CHECK(req("GET", "/records?after=x", 0, 0) == 400, "malformed after");
}

static void test_volume_errors(void) {
    shim_fs.nodev = K32_ERR_NODEV;
    CHECK(init() == K32_ERR_NODEV, "no card: k32_init fails");
    shim_fs.nodev = 0;
    CHECK(k32_alloc(0) == 0 && k32_alloc(1u << 20) == 0, "k32_alloc refuses sizes past the cap");
    CHECK(k32_alloc(TDB_APPEND_MAX + 512) != 0, "room for the largest append request");
}

int main(void) {
    shim.quiet = 1;
    srand(9);
    snprintf(g_dir, sizeof g_dir, "%s/tessera-db-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    snprintf(shim_fs.root, sizeof shim_fs.root, "%s", g_dir);
    test_codec();
    test_fresh_log();
    test_append_read();
    test_torn_tail();
    test_damage();
    test_pad_then_rollover();
    test_rollover_damage_params();
    test_volume_errors();
    shim_fs_reset_dir(g_dir); rmdir(g_dir);
    printf("%s: %d checks, %d failures\n", failures ? "FAILED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
