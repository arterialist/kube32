#include "tlog.h"

/* ---- CRC-32 ---------------------------------------------------------------
 * Byte-at-a-time with a 256-entry table built on first use (1 KiB) when
 * TLOG_CRC_TABLE256 is set, otherwise two steps per byte through a 16-entry
 * table (64 bytes of data). Both give the zlib checksum. */
#if defined(TLOG_CRC_TABLE256)
static uint32_t k_table[256];
static int k_table_ready;
static void table_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        k_table[i] = c;
    }
    k_table_ready = 1;
}
uint32_t tlog_crc32(uint32_t crc, const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    if (!k_table_ready) table_init();
    crc = ~crc;
    while (n--) crc = (crc >> 8) ^ k_table[(crc ^ *b++) & 0xFFu];
    return ~crc;
}
#else
static const uint32_t k_nibble[16] = {
    0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
    0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
};
uint32_t tlog_crc32(uint32_t crc, const void* p, uint32_t n) {
    const uint8_t* b = (const uint8_t*)p;
    crc = ~crc;
    while (n--) {
        crc ^= *b++;
        crc = (crc >> 4) ^ k_nibble[crc & 15u];
        crc = (crc >> 4) ^ k_nibble[crc & 15u];
    }
    return ~crc;
}
#endif

/* ---- framing --------------------------------------------------------------- */
int tlog_frame(const uint8_t* p, uint32_t avail, uint32_t* rec_len) {
    if (avail < TLOG_HDR) return 0;
    uint32_t len = tlog_get16(p);
    if (len > TLOG_PAYLOAD_MAX || p[2] == 0 || p[3] != 0) return -1;
    if (avail < TLOG_HDR + len) return 0;
    *rec_len = TLOG_HDR + len;
    return 1;
}
uint32_t tlog_rec_crc(const uint8_t* rec) {
    uint32_t crc = tlog_crc32(0, rec, 8);
    return tlog_crc32(crc, rec + TLOG_HDR, tlog_get16(rec));
}
int tlog_crc_ok(const uint8_t* rec) { return tlog_get32(rec + 8) == tlog_rec_crc(rec); }
void tlog_seal(uint8_t* rec, uint32_t seq) {
    tlog_put32(rec + 4, seq);
    tlog_put32(rec + 8, tlog_rec_crc(rec));
}

static void zero(uint8_t* p, uint32_t n) { while (n--) *p++ = 0; }
static void copy(uint8_t* d, const void* s, uint32_t n) { const uint8_t* b = (const uint8_t*)s; while (n--) *d++ = *b++; }

uint32_t tlog_put_pad(uint8_t* out, uint32_t total) {
    if (total < TLOG_HDR || total > TLOG_REC_MAX) return 0;
    zero(out, total);
    tlog_put16(out, (uint16_t)(total - TLOG_HDR));
    out[2] = TLOG_PAD;
    tlog_seal(out, 0);
    return total;
}

/* ---- payload writer / reader ------------------------------------------------ */
typedef struct { uint8_t* p; uint32_t n, cap; int bad; } wr;
static void w_u8(wr* w, uint8_t v) { if (w->n + 1 > w->cap) { w->bad = 1; return; } w->p[w->n++] = v; }
static void w_u16(wr* w, uint16_t v) { if (w->n + 2 > w->cap) { w->bad = 1; return; } tlog_put16(w->p + w->n, v); w->n += 2; }
static void w_u32(wr* w, uint32_t v) { if (w->n + 4 > w->cap) { w->bad = 1; return; } tlog_put32(w->p + w->n, v); w->n += 4; }
static void w_i64(wr* w, int64_t v) { w_u32(w, (uint32_t)(uint64_t)v); w_u32(w, (uint32_t)((uint64_t)v >> 32)); }
static void w_bytes(wr* w, const char* s, uint32_t n) { if (w->n + n > w->cap) { w->bad = 1; return; } copy(w->p + w->n, s, n); w->n += n; }

typedef struct { const uint8_t* p; uint32_t n, len; int bad; } rd;
static uint8_t  r_u8(rd* r) { if (r->n + 1 > r->len) { r->bad = 1; return 0; } return r->p[r->n++]; }
static uint16_t r_u16(rd* r) { if (r->n + 2 > r->len) { r->bad = 1; return 0; } uint16_t v = tlog_get16(r->p + r->n); r->n += 2; return v; }
static uint32_t r_u32(rd* r) { if (r->n + 4 > r->len) { r->bad = 1; return 0; } uint32_t v = tlog_get32(r->p + r->n); r->n += 4; return v; }
static int64_t  r_i64(rd* r) { uint32_t lo = r_u32(r), hi = r_u32(r); return (int64_t)(((uint64_t)hi << 32) | lo); }
static const char* r_bytes(rd* r, uint32_t n) { if (r->n + n > r->len) { r->bad = 1; return ""; } const char* s = (const char*)r->p + r->n; r->n += n; return s; }

/* Starts a record in out (header written by finish). */
static wr begin(uint8_t* out, uint32_t cap) {
    wr w = { out + TLOG_HDR, 0, 0, 0 };
    if (cap < TLOG_HDR) { w.bad = 1; return w; }
    w.cap = cap - TLOG_HDR < TLOG_PAYLOAD_MAX ? cap - TLOG_HDR : TLOG_PAYLOAD_MAX;
    return w;
}
static uint32_t finish(uint8_t* out, const wr* w, uint8_t type) {
    if (w->bad) return 0;
    tlog_put16(out, (uint16_t)w->n);
    out[2] = type; out[3] = 0;
    tlog_seal(out, 0);
    return TLOG_HDR + w->n;
}

uint32_t tlog_put_issue(uint8_t* out, uint32_t cap, const tlog_issue* r) {
    if (r->title_len > 255 || r->desc_len > 0xFFFF) return 0;
    wr w = begin(out, cap);
    w_u32(&w, r->id); w_i64(&w, r->created);
    w_u8(&w, r->project); w_u8(&w, r->status); w_u8(&w, r->priority);
    w_u8(&w, r->assignee); w_u8(&w, r->reporter); w_u8(&w, r->estimate);
    w_u16(&w, r->labels);
    w_u8(&w, (uint8_t)r->title_len); w_u16(&w, (uint16_t)r->desc_len);
    w_bytes(&w, r->title, r->title_len); w_bytes(&w, r->desc, r->desc_len);
    return finish(out, &w, TLOG_ISSUE_CREATE);
}
int tlog_get_issue(const uint8_t* p, uint32_t len, tlog_issue* r) {
    rd d = { p, 0, len, 0 };
    r->id = r_u32(&d); r->created = r_i64(&d);
    r->project = r_u8(&d); r->status = r_u8(&d); r->priority = r_u8(&d);
    r->assignee = r_u8(&d); r->reporter = r_u8(&d); r->estimate = r_u8(&d);
    r->labels = r_u16(&d);
    r->title_len = r_u8(&d); r->desc_len = r_u16(&d);
    r->title = r_bytes(&d, r->title_len); r->desc = r_bytes(&d, r->desc_len);
    return (d.bad || d.n != len) ? -1 : 0;
}

static int is_text_field(uint8_t f) { return f == TLOG_F_TITLE || f == TLOG_F_DESCRIPTION; }

uint32_t tlog_put_update(uint8_t* out, uint32_t cap, const tlog_update* r) {
    if (r->count == 0 || r->count > TLOG_MAX_CHANGES) return 0;
    wr w = begin(out, cap);
    w_u32(&w, r->id); w_i64(&w, r->ts); w_u8(&w, r->actor); w_u8(&w, r->count);
    for (uint32_t i = 0; i < r->count; i++) {
        const tlog_change* c = &r->ch[i];
        if (c->field < TLOG_F_TITLE || c->field > TLOG_F_ESTIMATE) return 0;
        w_u8(&w, c->field);
        if (is_text_field(c->field)) {
            if (c->len > 0xFFFF) return 0;
            w_u16(&w, (uint16_t)c->len); w_bytes(&w, c->text, c->len);
        } else {
            w_u32(&w, (uint32_t)c->from); w_u32(&w, (uint32_t)c->to);
        }
    }
    return finish(out, &w, TLOG_ISSUE_UPDATE);
}
int tlog_get_update(const uint8_t* p, uint32_t len, tlog_update* r) {
    rd d = { p, 0, len, 0 };
    r->id = r_u32(&d); r->ts = r_i64(&d); r->actor = r_u8(&d); r->count = r_u8(&d);
    if (r->count == 0 || r->count > TLOG_MAX_CHANGES) return -1;
    for (uint32_t i = 0; i < r->count && !d.bad; i++) {
        tlog_change* c = &r->ch[i];
        c->field = r_u8(&d);
        if (c->field < TLOG_F_TITLE || c->field > TLOG_F_ESTIMATE) return -1;
        if (is_text_field(c->field)) {
            c->len = r_u16(&d); c->text = r_bytes(&d, c->len); c->from = c->to = 0;
        } else {
            c->from = (int32_t)r_u32(&d); c->to = (int32_t)r_u32(&d); c->text = ""; c->len = 0;
        }
    }
    return (d.bad || d.n != len) ? -1 : 0;
}

uint32_t tlog_put_comment(uint8_t* out, uint32_t cap, const tlog_comment* r) {
    if (r->len > 0xFFFF) return 0;
    wr w = begin(out, cap);
    w_u32(&w, r->id); w_u32(&w, r->issue); w_i64(&w, r->ts); w_u8(&w, r->author);
    w_u16(&w, (uint16_t)r->len); w_bytes(&w, r->body, r->len);
    return finish(out, &w, TLOG_COMMENT);
}
int tlog_get_comment(const uint8_t* p, uint32_t len, tlog_comment* r) {
    rd d = { p, 0, len, 0 };
    r->id = r_u32(&d); r->issue = r_u32(&d); r->ts = r_i64(&d); r->author = r_u8(&d);
    r->len = r_u16(&d); r->body = r_bytes(&d, r->len);
    return (d.bad || d.n != len) ? -1 : 0;
}

uint32_t tlog_put_delete(uint8_t* out, uint32_t cap, const tlog_delete* r) {
    wr w = begin(out, cap);
    w_u32(&w, r->id); w_i64(&w, r->ts); w_u8(&w, r->actor);
    return finish(out, &w, TLOG_ISSUE_DELETE);
}
int tlog_get_delete(const uint8_t* p, uint32_t len, tlog_delete* r) {
    rd d = { p, 0, len, 0 };
    r->id = r_u32(&d); r->ts = r_i64(&d); r->actor = r_u8(&d);
    return (d.bad || d.n != len) ? -1 : 0;
}
