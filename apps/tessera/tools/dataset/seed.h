#ifndef TESSERA_SEED_H
#define TESSERA_SEED_H
/*
 * Deterministic sample data for Tessera: issues and comments of a fictional
 * photo-sharing product, generated natively by the dataset tool. Nothing on
 * the boards runs this code.
 */
#include "../../api/src/k32lib.h"

#define SEED_TITLE_MAX   120
#define SEED_DESC_MAX    1000
#define SEED_COMMENT_MAX 280
#define SEED_USERS       8          /* user ids 1..8 */
#define SEED_LABELS      12
#define SEED_NO_USER     0xFFu
enum { SEED_BACKLOG = 0, SEED_TODO, SEED_IN_PROGRESS, SEED_REVIEW, SEED_DONE };

typedef struct {
    u32 id;
    i64 created;                     /* ms, whole seconds */
    i64 status_ts;                   /* when the issue reached `status` (resolution time when done) */
    i64 updated;                     /* last change, comments included */
    u8  project, status, priority, assignee, reporter, estimate;
    u16 labels;
    char title[SEED_TITLE_MAX + 8]; u32 title_len;
    char desc[SEED_DESC_MAX + 8];   u32 desc_len;
} seed_issue;

typedef struct {
    u32 issue;
    i64 ts;
    u8  author;
    char body[SEED_COMMENT_MAX + 8]; u32 len;
} seed_comment;

/* Fills issues[0..n_issues) and up to n_comments comments (each issue's own
 * comments in order, then the leftovers). Returns 0. */
int seed_generate(u32 n_issues, u32 n_comments, i64 now, seed_issue* issues, seed_comment* comments, u32* comment_count);
#endif
