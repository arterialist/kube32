/*
 * Tessera data model. Fixed-size records in slab tables carved from one
 * static arena; variable-length text lives in the same arena as bump
 * allocations. All ids are 1-based for issues/comments (0 = none).
 */
#ifndef TESSERA_STORE_H
#define TESSERA_STORE_H

#include "k32lib.h"

#define T_STATUS_N     5
#define T_PRIO_N       5
#define T_USERS_N      9      /* id 0 is the "hud" system user */
#define T_HUMAN_USERS  8
#define T_PROJECTS_N   3
#define T_LABELS_N     12
#define T_MAX_LABELS   16
#define T_ACT_RING     512
#define T_SESSIONS_N   32
#define T_TITLE_MAX    120
#define T_DESC_MAX     1000
#define T_COMMENT_MAX  280
#define T_NOTE_MAX     200
#define T_NONE_U16     0xFFFFu
#define T_NO_USER      0xFFu
#define T_LAT_RING     256
#define T_SESSION_IDLE_MS (24ll * 3600ll * 1000ll)
#define T_DAY_MS       86400000ll

enum { ST_BACKLOG = 0, ST_TODO, ST_IN_PROGRESS, ST_REVIEW, ST_DONE };
enum { OP_CREATE = 0, OP_UPDATE, OP_COMMENT, OP_DELETE };
enum { F_NONE = 0, F_TITLE, F_DESCRIPTION, F_STATUS, F_PRIORITY, F_ASSIGNEE, F_LABELS, F_ESTIMATE };
enum { ISSUE_DELETED = 1 };

typedef struct { const char* username; const char* name; const char* initials; const char* color; } user_t;
typedef struct { const char* key; const char* name; const char* color; } project_t;
typedef struct { const char* name; const char* color; } label_t;

extern const user_t      t_users[T_USERS_N];
extern const project_t   t_projects[T_PROJECTS_N];
extern const label_t     t_labels[T_LABELS_N];
extern const char* const t_status_names[T_STATUS_N];
extern const char* const t_prio_names[T_PRIO_N];
extern const char* const t_op_names[4];
extern const char* const t_field_names[8];

typedef struct issue {
    u32 created, updated, resolved;   /* packed seconds (ts_pack); resolved = 0 unless done */
    u32 title_off, desc_off;
    u16 labels;                       /* bitmask */
    u16 comment_count;
    u16 comment_head, comment_tail;   /* 1-based comment ids, 0 = none */
    u16 desc_len;
    u8  title_len;
    u8  project;                      /* 1..3 */
    u8  status, priority;
    u8  assignee, reporter;           /* T_NO_USER = unassigned */
    u8  estimate;
    u8  flags;
} issue_t;                            /* 40 bytes */

typedef struct comment {
    u32 created;                      /* packed seconds */
    u32 body_off;
    u16 body_len;
    u16 issue;                        /* issue id */
    u16 next;                         /* next comment id on the same issue, 0 = end */
    u8  author;
    u8  flags;
} comment_t;                          /* 16 bytes */

typedef struct act {
    i64 ts;
    u32 id;
    i32 from, to;
    u16 issue;
    u8  actor, op, field;
    u8  pad[3];
} act_t;                              /* 32 bytes */

typedef struct session {
    char token[16];
    i64  last_seen;                   /* uptime ms */
    u8   user;
    u8   used;
    u8   pad[6];
} session_t;                          /* 32 bytes */

typedef struct term {
    u32 str_off;
    u32 post_off;                     /* arena offset of the posting array (top region, see index.c); 0 = none */
    u16 count, cap;
    u8  len;
    u8  pad[3];
} term_t;                             /* 16 bytes; a term keeps its index for life (insertion order) */

typedef struct __attribute__((packed)) posting { u16 id; u8 tf; } posting_t;   /* 3 bytes */

typedef struct seed_state {
    k32_rng rng;
    u32 n_issues, n_comments;         /* targets */
    u32 next;                         /* issues seeded so far */
    u32 comments_left;
    i64 now;                          /* synthetic "now" the data set is relative to */
    u8  phase;                        /* SEED_ISSUES -> SEED_DONE */
} seed_state;
enum { SEED_IDLE = 0, SEED_ISSUES, SEED_DONE, SEED_FAILED };

typedef struct store {
    k32_arena arena;
    u8* membase;                      /* base for u32 "pointers" crossing the ABI */

    u8* scratch; u32 scratch_size, scratch_top; u8 scratch_fresh;

    issue_t*   issues;   u32 issue_cap,   issue_count, issue_live;
    comment_t* comments; u32 comment_cap, comment_count, comment_live;
    act_t*     acts;     u32 act_count,   act_next_id;
    session_t* sessions;
    term_t*    terms;    u32 term_cap,    term_count;
    u16*       term_order;            /* term indices sorted by string (prefix walks) */
    u16*       term_hash; u32 term_hash_mask;   /* open addressing, slot = term index + 1, 0 = empty */
    u32 posting_count, posting_bytes, posting_leaked;
    u32 string_bytes;
    u32 fixed_bytes;                  /* bytes used by the slab tables + scratch */

    i64 epoch_base;
    i64 ts_base;                      /* packed timestamp origin (epoch_base - 120 days) */
    u32 seed_issues, seed_comments, seed_chunk;
    seed_state seed;

    u16  last_viewed;
    char note[T_NOTE_MAX];
    u32  note_len;

    u32 req_total, req_errors, req_last_us;
    uint64_t req_sum_us;
    u32 lat_ring[T_LAT_RING]; u32 lat_n;
} store_t;

extern store_t g;

/* lifecycle */
int   store_init(u32 max_issues, u32 max_comments, u32 max_terms, u32 scratch_size);
i64   store_now(void);
u32   arena_used(void);                                /* bottom + top region bytes */

/* packed timestamps: whole seconds since g.ts_base, monotonic with the ms value */
u32   ts_pack(i64 ms);
i64   ts_ms(u32 packed);

/* scratch (per-request bump region) */
void* scratch_alloc(u32 size);

/* strings */
u32         str_store(const char* s, u32 len);        /* arena offset, 0 on failure */
const char* str_at(u32 off);
int         str_replace(u32* off, u16* len, const char* s, u32 n, u32 cap_len);

/* issues */
issue_t* issue_by_id(u32 id);                          /* live issues only, NULL otherwise */
issue_t* issue_slot(u32 id);                           /* including tombstones */
int      issue_parse_key(const char* key, u32 len, u32* out_id);   /* 1 if "KEY-123" form and project matches */
u32      issue_key(u32 id, char* out, u32 cap);        /* "TES-42" */
int      issue_create(u8 project, const char* title, u32 tlen, const char* desc, u32 dlen,
                      u8 status, u8 priority, u8 assignee, u8 reporter, u16 labels, u8 estimate,
                      i64 created);                    /* id or negative */
void     issue_set_status(issue_t* it, u8 status, i64 ts);

/* comments */
int      comment_add(u32 issue_id, u8 author, const char* body, u32 len, i64 ts);  /* comment id or negative */
comment_t* comment_by_id(u32 id);

/* activity */
void     act_push(i64 ts, u8 actor, u8 op, u16 issue, u8 field, i32 from, i32 to);
const act_t* act_at(u32 i);                            /* i-th most recent, NULL past the end */
u32      act_available(void);

/* sessions */
session_t* session_create(u8 user, i64 uptime_ms);
session_t* session_find(const char* tok, u32 len);
void       session_drop(session_t* s);
u32        session_expire(i64 uptime_ms);
u32        session_active(void);

/* lookups */
int status_from_name(const char* s, u32 len);         /* -1 if unknown */
int user_from_name(const char* s, u32 len);

/* request latency */
void stats_record_request(u32 dur_us, int is_error);

#endif
