/*
 * Tessera record log: the on-card format of tessera-db and the wire format
 * between tessera-api and tessera-db. All integers are little-endian.
 *
 * Record (TLOG_HDR bytes of header, then the payload):
 *   0  u16 len     payload bytes, at most TLOG_PAYLOAD_MAX
 *   2  u8  type    TLOG_ISSUE_CREATE .. TLOG_ISSUE_DELETE, or TLOG_PAD
 *   3  u8  flags   0
 *   4  u32 seq     1, 2, 3, ... in log order (0 in TLOG_PAD records)
 *   8  u32 crc     CRC-32 (zlib polynomial) of bytes 0..7 followed by the payload
 *
 * Payloads (text is UTF-8 as the API received it, length first):
 *   ISSUE_CREATE  u32 id, i64 created_ms, u8 project, u8 status, u8 priority,
 *                 u8 assignee (TLOG_NO_USER = none), u8 reporter, u8 estimate,
 *                 u16 labels, u8 title_len, u16 desc_len, title, description
 *   ISSUE_UPDATE  u32 id, i64 ts_ms, u8 actor, u8 count, then count changes:
 *                 u8 field; TITLE and DESCRIPTION carry u16 len + the new text,
 *                 the other fields i32 from, i32 to (assignee -1 = none)
 *   COMMENT       u32 id, u32 issue, i64 ts_ms, u8 author, u16 len, body
 *   ISSUE_DELETE  u32 id, i64 ts_ms, u8 actor
 *
 * Issue and comment ids are assigned in log order starting at 1, so a replay
 * of the log rebuilds the same ids. TLOG_PAD records cover bytes the store
 * gave up on (a torn tail) and carry no data.
 */
#ifndef TESSERA_TLOG_H
#define TESSERA_TLOG_H

#include <stdint.h>

#define TLOG_HDR          12u
#define TLOG_PAYLOAD_MAX  1280u           /* the largest API record is 1,185 bytes */
#define TLOG_REC_MAX      (TLOG_HDR + TLOG_PAYLOAD_MAX)
#define TLOG_NO_USER      0xFFu
#define TLOG_MAX_CHANGES  8u

enum {
    TLOG_ISSUE_CREATE = 1,
    TLOG_ISSUE_UPDATE = 2,
    TLOG_COMMENT      = 3,
    TLOG_ISSUE_DELETE = 4,
    TLOG_TYPE_MAX     = 127,   /* types above this are reserved for the store */
    TLOG_PAD          = 0xFF
};

/* Change fields (same numbering as the API's activity fields). */
enum { TLOG_F_TITLE = 1, TLOG_F_DESCRIPTION, TLOG_F_STATUS, TLOG_F_PRIORITY, TLOG_F_ASSIGNEE, TLOG_F_LABELS, TLOG_F_ESTIMATE };

typedef struct {
    uint32_t id;
    int64_t  created;
    uint8_t  project, status, priority, assignee, reporter, estimate;
    uint16_t labels;
    const char* title; uint32_t title_len;
    const char* desc;  uint32_t desc_len;
} tlog_issue;

typedef struct {
    uint8_t  field;
    int32_t  from, to;              /* scalar fields */
    const char* text; uint32_t len; /* TITLE / DESCRIPTION */
} tlog_change;

typedef struct {
    uint32_t id;
    int64_t  ts;
    uint8_t  actor;
    uint8_t  count;
    tlog_change ch[TLOG_MAX_CHANGES];
} tlog_update;

typedef struct {
    uint32_t id, issue;
    int64_t  ts;
    uint8_t  author;
    const char* body; uint32_t len;
} tlog_comment;

typedef struct {
    uint32_t id;
    int64_t  ts;
    uint8_t  actor;
} tlog_delete;

/* little-endian access */
static inline void     tlog_put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void     tlog_put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint16_t tlog_get16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t tlog_get32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* CRC-32 with the zlib polynomial; tlog_crc32(0, p, n) is the checksum of p. */
uint32_t tlog_crc32(uint32_t crc, const void* p, uint32_t n);

/* Record framing. tlog_frame returns 1 and the record length when p holds a
 * complete record header and payload, 0 when more bytes are needed and -1
 * when the header is impossible (payload over TLOG_PAYLOAD_MAX, flags set,
 * type 0). It does not check the CRC. */
int      tlog_frame(const uint8_t* p, uint32_t avail, uint32_t* rec_len);
uint32_t tlog_rec_crc(const uint8_t* rec);          /* CRC the record should carry */
int      tlog_crc_ok(const uint8_t* rec);
void     tlog_seal(uint8_t* rec, uint32_t seq);     /* sets seq and crc */
static inline uint32_t tlog_len(const uint8_t* rec)  { return tlog_get16(rec); }
static inline uint8_t  tlog_type(const uint8_t* rec) { return rec[2]; }
static inline uint32_t tlog_seq(const uint8_t* rec)  { return tlog_get32(rec + 4); }
static inline const uint8_t* tlog_payload(const uint8_t* rec) { return rec + TLOG_HDR; }

/* Writes a TLOG_PAD record of exactly total bytes (>= TLOG_HDR) into out;
 * returns total, or 0 when total is out of range. */
uint32_t tlog_put_pad(uint8_t* out, uint32_t total);

/* Encoders write one record (seq 0, sealed with crc for seq 0) into out and
 * return its length, or 0 when the input is out of range or cap is short.
 * Decoders parse a payload; text pointers point into it. 0 or -1. */
uint32_t tlog_put_issue(uint8_t* out, uint32_t cap, const tlog_issue* r);
uint32_t tlog_put_update(uint8_t* out, uint32_t cap, const tlog_update* r);
uint32_t tlog_put_comment(uint8_t* out, uint32_t cap, const tlog_comment* r);
uint32_t tlog_put_delete(uint8_t* out, uint32_t cap, const tlog_delete* r);
int      tlog_get_issue(const uint8_t* p, uint32_t len, tlog_issue* r);
int      tlog_get_update(const uint8_t* p, uint32_t len, tlog_update* r);
int      tlog_get_comment(const uint8_t* p, uint32_t len, tlog_comment* r);
int      tlog_get_delete(const uint8_t* p, uint32_t len, tlog_delete* r);

#endif
