#include "tdb.h"
#include "../../common/k32_host.h"

static uint8_t g_io[TDB_IOBUF];            /* the one file buffer: scans, reads, recovery */
_Static_assert(TDB_IOBUF >= TLOG_REC_MAX && TDB_IOBUF >= TDB_APPEND_MAX, "the buffer holds a whole record and a torn append");

static const uint8_t k_magic[4] = { 'T', 'S', 'D', 'B' };

static void fail(tdb_t* db, const char* msg) {
    uint32_t i = 0;
    while (msg[i] && i + 1 < sizeof db->error) { db->error[i] = msg[i]; i++; }
    db->error[i] = 0;
    db->state = TDB_FAILED;
}

uint32_t tdb_seg_name(uint32_t index, char* out) {
    for (int i = 4; i >= 0; i--) { out[i] = (char)('0' + index % 10); index /= 10; }
    out[5] = '.'; out[6] = 's'; out[7] = 'e'; out[8] = 'g'; out[9] = 0;
    return 9;
}
static int32_t seg_open(uint32_t index, uint32_t flags) {
    char name[12];
    uint32_t n = tdb_seg_name(index, name);
    return k32_fs_open(name, n, flags);
}

static void hdr_build(uint8_t* h, uint32_t log_id, uint32_t index, uint32_t first) {
    for (uint32_t i = 0; i < TDB_SEG_HDR; i++) h[i] = 0;
    for (int i = 0; i < 4; i++) h[i] = k_magic[i];
    tlog_put16(h + 4, TDB_VERSION);
    tlog_put16(h + 6, TDB_SEG_HDR);
    tlog_put32(h + 8, log_id);
    tlog_put32(h + 12, index);
    tlog_put32(h + 16, first);
    tlog_put32(h + 20, TDB_SEG_BYTES);
    tlog_put32(h + 28, tlog_crc32(0, h, 28));
}
static int hdr_parse(const uint8_t* h, uint32_t* log_id, uint32_t* index, uint32_t* first) {
    for (int i = 0; i < 4; i++) if (h[i] != k_magic[i]) return -1;
    if (tlog_get16(h + 4) != TDB_VERSION || tlog_get16(h + 6) != TDB_SEG_HDR) return -1;
    if (tlog_get32(h + 28) != tlog_crc32(0, h, 28)) return -1;
    *log_id = tlog_get32(h + 8); *index = tlog_get32(h + 12); *first = tlog_get32(h + 16);
    return *first ? 0 : -1;
}

/* ---- buffered forward reader over one open file ------------------------------ */
typedef struct {
    int32_t  fd;
    uint32_t fill, pos;      /* valid bytes in g_io, parse position */
    uint32_t base;           /* file offset of g_io[0] */
    int      eof;
    int32_t  err;
} reader;

/* Moves the unread bytes to the front and reads more. Returns bytes read, 0 at EOF, <0 on error. */
static int32_t rd_more(reader* r) {
    if (r->pos) {
        uint32_t keep = r->fill - r->pos;
        for (uint32_t i = 0; i < keep; i++) g_io[i] = g_io[r->pos + i];
        r->base += r->pos; r->fill = keep; r->pos = 0;
    }
    if (r->fill == TDB_IOBUF) return 0;
    int32_t n = k32_fs_read(r->fd, g_io + r->fill, TDB_IOBUF - r->fill);
    if (n < 0) { r->err = n; return n; }
    if (n == 0) r->eof = 1;
    r->fill += (uint32_t)n;
    return n;
}
/* Next record at g_io + r->pos: 1 with its length, 0 at a clean end of file,
 * -1 for bytes that are not a record (or a record cut off by the end). */
static int rd_next(reader* r, uint32_t* len) {
    for (;;) {
        int f = tlog_frame(g_io + r->pos, r->fill - r->pos, len);
        if (f != 0) return f;
        if (r->eof) return r->fill == r->pos ? 0 : -1;
        if (rd_more(r) < 0) return -1;
    }
}
/* Skips the segment header. 0 or -1. */
static int rd_skip_header(reader* r) {
    while (r->fill < TDB_SEG_HDR && !r->eof) if (rd_more(r) < 0) return -1;
    if (r->fill < TDB_SEG_HDR) return -1;
    r->pos = TDB_SEG_HDR;
    return 0;
}

/* ---- creation and recovery ---------------------------------------------------- */
/* Creates segment `index`. On failure the file is removed again: a complete
 * header left behind would make the next open treat it as the last segment. */
static int32_t seg_create(tdb_t* db, uint32_t index, uint32_t first) {
    uint8_t h[TDB_SEG_HDR];
    hdr_build(h, db->log_id, index, first);
    int32_t fd = seg_open(index, K32_FS_O_WRONLY | K32_FS_O_CREAT | K32_FS_O_TRUNC);
    if (fd < 0) return fd;
    int32_t w = k32_fs_write(fd, h, TDB_SEG_HDR);
    int32_t c = k32_fs_close(fd);
    int32_t rc = w != (int32_t)TDB_SEG_HDR ? (w < 0 ? w : K32_ERR_IO) : c;
    if (rc < 0) {
        char name[12];
        if (k32_fs_remove(name, tdb_seg_name(index, name)) < 0) db->roll_pending = 1;
    }
    return rc;
}

/* Overwrites `garbage` bytes at offset `at` of segment `index` with TLOG_PAD
 * records (the last one may extend the file to reach TLOG_HDR bytes).
 * Returns the bytes written or a negative K32_ERR_*. */
static int32_t pad_over(uint32_t index, uint32_t at, uint32_t garbage) {
    int32_t fd = seg_open(index, K32_FS_O_RDWR);
    if (fd < 0) return fd;
    uint32_t skip = at;
    int32_t rc = 0;
    while (skip > 0) {            /* the ABI has no seek: reading moves the file position */
        int32_t n = k32_fs_read(fd, g_io, skip < TDB_IOBUF ? skip : TDB_IOBUF);
        if (n <= 0) { rc = n < 0 ? n : K32_ERR_IO; break; }
        skip -= (uint32_t)n;
    }
    uint32_t rem = garbage, written = 0;
    while (rc == 0 && rem > 0) {
        uint32_t take = rem > TLOG_REC_MAX ? TLOG_REC_MAX : rem;
        if (rem > take && rem - take < TLOG_HDR) take = rem - TLOG_HDR;
        if (take < TLOG_HDR) take = TLOG_HDR;
        tlog_put_pad(g_io, take);
        int32_t w = k32_fs_write(fd, g_io, take);
        if (w != (int32_t)take) { rc = w < 0 ? w : K32_ERR_IO; break; }
        written += take;
        rem = rem > take ? rem - take : 0;
    }
    int32_t c = k32_fs_close(fd);
    if (rc == 0 && c != 0) rc = c;
    return rc < 0 ? rc : (int32_t)written;
}

/* Scans the last segment: sets last_seq and tail_end, pads a torn tail. */
static int32_t scan_tail(tdb_t* db) {
    uint32_t k = db->nsegs - 1;
    int32_t fd = seg_open(k, K32_FS_O_RDONLY);
    if (fd < 0) return fd;
    int32_t size = k32_fs_size(fd);
    reader r = { fd, 0, 0, 0, 0, 0 };
    uint32_t expected = db->seg_first[k], valid_end = TDB_SEG_HDR, len;
    if (size < 0 || rd_skip_header(&r) < 0) { k32_fs_close(fd); return size < 0 ? size : K32_ERR_IO; }
    while (rd_next(&r, &len) == 1) {
        const uint8_t* rec = g_io + r.pos;
        if (!tlog_crc_ok(rec)) break;
        if (tlog_type(rec) != TLOG_PAD) {
            if (tlog_seq(rec) != expected) break;
            expected++;
        }
        valid_end += len;
        r.pos += len;
    }
    uint32_t garbage = (uint32_t)size > valid_end ? (uint32_t)size - valid_end : 0;
    int damaged = garbage > TDB_APPEND_MAX;
    if (garbage && !damaged) {
        /* An interrupted append leaves a prefix of its batch: no whole record
         * follows the first bad byte. A good record after it means damage in
         * committed records, which must not be padded over. */
        while (!r.eof && !r.err && r.fill < TDB_IOBUF) if (rd_more(&r) < 0) break;
        for (uint32_t i = r.pos; i + TLOG_HDR <= r.fill && !damaged; i++) {
            uint32_t l;
            const uint8_t* p = g_io + i;
            if (tlog_frame(p, r.fill - i, &l) == 1 && tlog_type(p) != TLOG_PAD && tlog_seq(p) != 0 && tlog_crc_ok(p)) damaged = 1;
        }
    }
    k32_fs_close(fd);
    if (r.err) return r.err;
    db->last_seq = expected - 1;
    db->tail_end = valid_end;
    if (garbage == 0) return 0;
    if (damaged) {
        fail(db, "damaged records in the last segment");
        return 0;
    }
    int32_t w = pad_over(k, valid_end, garbage);
    if (w < 0) { fail(db, "cannot repair the torn tail"); return w; }
    db->recovered_bytes += garbage;
    db->tail_end = valid_end + (uint32_t)w;
    return 0;
}

int tdb_open(tdb_t* db) {
    uint8_t* zero = (uint8_t*)db;
    for (uint32_t i = 0; i < sizeof *db; i++) zero[i] = 0;
    uint8_t h[TDB_SEG_HDR];
    uint32_t sizes = 0, last_size = 0;
    for (uint32_t i = 0; i < TDB_MAX_SEGS; i++) {
        int32_t fd = seg_open(i, K32_FS_O_RDONLY);
        if (fd == K32_ERR_NOENT) break;
        if (fd < 0) { db->last_error = fd; return fd; }
        int32_t n = k32_fs_read(fd, h, TDB_SEG_HDR);
        int32_t size = k32_fs_size(fd);
        k32_fs_close(fd);
        uint32_t log_id, index, first;
        if (n != (int32_t)TDB_SEG_HDR || hdr_parse(h, &log_id, &index, &first) < 0 || index != i) {
            /* a segment cut off while its header was written holds no records */
            int32_t next = seg_open(i + 1, K32_FS_O_RDONLY);
            if (next >= 0) k32_fs_close(next);
            if (size >= 0 && size < (int32_t)TDB_SEG_HDR && next == K32_ERR_NOENT) {
                char name[12];
                k32_fs_remove(name, tdb_seg_name(i, name));
                db->recovered_bytes += (uint32_t)size;
                break;
            }
            fail(db, "damaged segment header");
            return 0;
        }
        if (i == 0) db->log_id = log_id;
        else if (log_id != db->log_id || first <= db->seg_first[i - 1]) { fail(db, "segments of different logs"); return 0; }
        db->seg_first[i] = first;
        db->nsegs = i + 1;
        last_size = size > 0 ? (uint32_t)size : 0;
        sizes += last_size;
    }
    if (db->nsegs == 0) {
        uint32_t id = k32_random();
        db->log_id = id ? id : 1;
        int32_t rc = seg_create(db, 0, 1);
        if (rc < 0) { db->last_error = rc; return rc; }
        db->nsegs = 1; db->seg_first[0] = 1;
        db->last_seq = 0; db->tail_end = TDB_SEG_HDR; db->total_bytes = TDB_SEG_HDR;
        return 0;
    }
    if (db->nsegs == TDB_MAX_SEGS) {
        int32_t more = seg_open(TDB_MAX_SEGS, K32_FS_O_RDONLY);
        if (more >= 0) { k32_fs_close(more); fail(db, "more segments than supported"); return 0; }
    }
    int32_t rc = scan_tail(db);
    if (rc < 0) {
        db->last_error = rc;
        if (db->state == TDB_OK) fail(db, "cannot read the last segment");
    }
    db->total_bytes = sizes - last_size + db->tail_end;
    return 0;
}

/* ---- append -------------------------------------------------------------------- */
int tdb_append(tdb_t* db, uint8_t* recs, uint32_t len, uint32_t expect, uint32_t* first, uint32_t* last) {
    if (db->state != TDB_OK) return TDB_E_STATE;
    if (len == 0 || len > TDB_APPEND_MAX) return TDB_E_INVAL;
    uint32_t n = 0, pos = 0, rl;
    while (pos < len) {
        if (tlog_frame(recs + pos, len - pos, &rl) != 1) return TDB_E_INVAL;
        uint8_t t = tlog_type(recs + pos);
        if (t == 0 || t > TLOG_TYPE_MAX) return TDB_E_INVAL;
        pos += rl; n++;
    }
    if (db->needs_recovery) {
        /* a failed append may have left part of a batch behind */
        uint32_t before = db->tail_end;
        int32_t rc = scan_tail(db);
        if (rc < 0) { db->last_error = rc; return rc; }
        db->total_bytes = db->total_bytes - before + db->tail_end;
        if (db->state != TDB_OK) return TDB_E_STATE;
        db->needs_recovery = 0;
    }
    if (expect && expect != db->last_seq + 1) return TDB_E_CONFLICT;
    uint32_t seq = db->last_seq + 1;
    for (pos = 0; pos < len; pos += TLOG_HDR + tlog_len(recs + pos)) tlog_seal(recs + pos, seq++);

    if (db->roll_pending || db->tail_end + len > TDB_SEG_BYTES) {
        if (db->nsegs >= TDB_MAX_SEGS) return TDB_E_FULL;
        int32_t rc = seg_create(db, db->nsegs, db->last_seq + 1);
        if (rc < 0) { db->last_error = rc; db->append_errors++; return rc; }
        db->seg_first[db->nsegs] = db->last_seq + 1;
        db->nsegs++;
        db->roll_pending = 0;
        db->tail_end = TDB_SEG_HDR;
        db->total_bytes += TDB_SEG_HDR;
    }
    int32_t fd = seg_open(db->nsegs - 1, K32_FS_O_WRONLY | K32_FS_O_APPEND);
    if (fd < 0) { db->last_error = fd; db->append_errors++; return fd; }
    int32_t w = k32_fs_write(fd, recs, len);
    int32_t c = k32_fs_close(fd);
    if (w != (int32_t)len || c != 0) {
        db->needs_recovery = 1;
        db->append_errors++;
        db->last_error = w < 0 ? w : (c < 0 ? c : K32_ERR_IO);
        return db->last_error;
    }
    *first = db->last_seq + 1;
    *last = db->last_seq + n;
    db->last_seq += n;
    db->tail_end += len;
    db->total_bytes += len;
    db->appends++;
    return 0;
}

/* ---- read ---------------------------------------------------------------------- */
int tdb_read(tdb_t* db, uint32_t after, uint32_t max_bytes, tdb_sink sink, void* ctx, uint32_t* bytes, uint32_t* records) {
    *bytes = 0; *records = 0;
    if (after >= db->last_seq || db->nsegs == 0) return 0;
    /* the last segment whose first record is <= after + 1 */
    uint32_t lo = 0, hi = db->nsegs;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (db->seg_first[mid] <= after + 1) lo = mid; else hi = mid;
    }
    const uint32_t k = lo, is_last = (k == db->nsegs - 1);
    int32_t fd = seg_open(k, K32_FS_O_RDONLY);
    if (fd < 0) return fd;
    reader r = { fd, 0, 0, 0, 0, 0 };
    int rc = 0;
    uint32_t expected = after + 1, len, out = 0, count = 0;
    uint32_t run = 0, run_len = 0;         /* records waiting for the sink: g_io[run, run + run_len) */
    if (rd_skip_header(&r) < 0) rc = r.err ? r.err : TDB_E_CORRUPT;
    while (rc == 0) {
        if (is_last && r.base + r.pos >= db->tail_end) break;   /* bytes past the log's end are not records */
        int f = tlog_frame(g_io + r.pos, r.fill - r.pos, &len);
        if (f == 0 && !r.eof) {
            /* refill: hand the pending run over first, the buffer moves */
            if (run_len) { if (sink(ctx, g_io + run, run_len) < 0) { rc = K32_ERR_IO; break; } run_len = 0; }
            if (rd_more(&r) < 0) { rc = r.err; break; }
            continue;
        }
        if (f == 0) { if (r.fill != r.pos) rc = TDB_E_CORRUPT; break; }
        if (f < 0) { rc = TDB_E_CORRUPT; break; }
        const uint8_t* rec = g_io + r.pos;
        if (tlog_type(rec) != TLOG_PAD && tlog_seq(rec) > after) {
            if (tlog_seq(rec) != expected) { rc = TDB_E_CORRUPT; break; }
            if (out + len > max_bytes) break;
            if (run_len == 0) run = r.pos;
            run_len += len;
            out += len; count++; expected++;
        } else if (run_len) {
            if (sink(ctx, g_io + run, run_len) < 0) { rc = K32_ERR_IO; break; }
            run_len = 0;
        }
        r.pos += len;
    }
    /* records accepted before an error still go out */
    if (run_len && sink(ctx, g_io + run, run_len) < 0 && rc == 0) rc = K32_ERR_IO;
    k32_fs_close(fd);
    *bytes = out; *records = count;
    return rc;
}
