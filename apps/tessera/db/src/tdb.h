/*
 * tessera-db storage engine: an append-only log of tlog records (see
 * common/tlog.h) kept in segment files on the pod's volume.
 *
 * Files: "00000.seg", "00001.seg", ... at the volume root. Each segment holds
 * at most TDB_SEG_BYTES bytes: a TDB_SEG_HDR-byte header, then whole records.
 * Records never span segments; a full segment is never written again.
 *
 * Segment header (little-endian):
 *   0  u32 magic      "TSDB"
 *   4  u16 version    TDB_VERSION
 *   6  u16 hdr_size   TDB_SEG_HDR
 *   8  u32 log_id     random id of the log, the same in every segment
 *  12  u32 index      segment number (matches the file name)
 *  16  u32 first_seq  sequence number of the first record in the segment
 *  20  u32 seg_bytes  TDB_SEG_BYTES at creation
 *  24  u32 reserved   0
 *  28  u32 crc        CRC-32 of bytes 0..27
 *
 * Opening the log reads every segment header and scans the last segment
 * record by record (framing, CRC, consecutive sequence numbers). Bytes after
 * the last good record (a torn append: at most TDB_APPEND_MAX bytes, and no
 * whole record after the first bad byte) are overwritten in place with
 * TLOG_PAD records, so the file needs neither truncation nor rename. A longer
 * bad tail, a good record after a bad one or a damaged header elsewhere
 * leaves the log read-only.
 *
 * The ABI has no seek: a read opens the segment that holds the requested
 * sequence number and reads forward from its start, so one page costs at
 * most one segment of reading.
 */
#ifndef TESSERA_TDB_H
#define TESSERA_TDB_H

#include <stdint.h>
#include "../../common/tlog.h"

#define TDB_VERSION     1u
#define TDB_SEG_HDR     32u
#define TDB_SEG_BYTES   (32u * 1024u)
#define TDB_MAX_SEGS    256u            /* 8 MiB of records */
#define TDB_APPEND_MAX  2048u           /* bytes per append batch: one k32_fs_write */
#define TDB_IOBUF       2048u           /* holds the largest record (TLOG_REC_MAX) and a torn tail */
#define TDB_PAGE_MAX    16384u          /* bytes of records per read page */

enum { TDB_OK = 0, TDB_FAILED = 1 };

/* tdb_append / tdb_read results */
#define TDB_E_STATE     (-100)   /* log is read-only after a failed open or recovery */
#define TDB_E_INVAL     (-101)   /* malformed batch */
#define TDB_E_CONFLICT  (-102)   /* expect does not match the next sequence number */
#define TDB_E_FULL      (-103)   /* TDB_MAX_SEGS segments in use */
#define TDB_E_CORRUPT   (-104)   /* damaged record found while reading */

typedef struct {
    int      state;
    uint32_t log_id;
    uint32_t nsegs;
    uint32_t last_seq;          /* 0 for an empty log */
    uint32_t tail_end;          /* bytes in use in the last segment */
    uint32_t total_bytes;       /* bytes in use in all segments */
    uint32_t recovered_bytes;   /* torn bytes covered with TLOG_PAD at open */
    uint32_t appends, append_errors;
    int      needs_recovery;    /* an append failed: rescan the tail first */
    int      roll_pending;      /* a new segment could not be created cleanly: create it before the next append */
    int32_t  last_error;        /* last K32_ERR_* from the file system */
    char     error[64];         /* why the log is read-only */
    uint32_t seg_first[TDB_MAX_SEGS];
} tdb_t;

/* Opens (or creates) the log in the volume. Returns 0, or a negative
 * K32_ERR_* when the volume cannot be used at all (no card, no grant).
 * A damaged log opens read-only: state TDB_FAILED and error say why. */
int tdb_open(tdb_t* db);

/* Appends a batch of records (encoded with seq 0; the store assigns the
 * sequence numbers and seals the CRCs in place). expect = 0 skips the check,
 * otherwise it must equal last_seq + 1. Returns 0 and the assigned range,
 * a TDB_E_* code, or a negative K32_ERR_* from the file system. Durable once
 * it returns 0: the segment file is closed after the write. */
int tdb_append(tdb_t* db, uint8_t* recs, uint32_t len, uint32_t expect, uint32_t* first, uint32_t* last);

/* Streams whole records with seq > after, up to max_bytes, from one segment
 * to sink (TLOG_PAD records are skipped). A page ends at the end of a
 * segment; the next call continues in the next one. Returns 0, TDB_E_CORRUPT
 * or a negative K32_ERR_*; *bytes and *records count what reached the sink. */
typedef int (*tdb_sink)(void* ctx, const uint8_t* data, uint32_t len);
int tdb_read(tdb_t* db, uint32_t after, uint32_t max_bytes, tdb_sink sink, void* ctx, uint32_t* bytes, uint32_t* records);

/* "00042.seg" for index 42; returns the length. */
uint32_t tdb_seg_name(uint32_t index, char* out);

#endif
