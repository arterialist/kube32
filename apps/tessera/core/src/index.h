/* Inverted index over issue title (x3), description (x1) and comments (x1). */
#ifndef TESSERA_INDEX_H
#define TESSERA_INDEX_H
#include "k32lib.h"

#define IDX_MAX_QUERY_TOKENS 8
#define IDX_MAX_PREFIX_TERMS 64
#define IDX_BAG_MAX 256

int  index_add_issue(u32 id);           /* tokenizes the issue's current content and adds postings */
void index_remove_issue(u32 id);        /* removes postings for the issue's current content */
/* Adds one comment's tokens to an already indexed issue (tf merged in place);
 * the result equals removing and re-adding the whole issue. */
int  index_add_comment(u32 id, const char* body, u32 len);
/* Packs the posting region (reclaims dead blocks, gives every term
 * count + count/16 + 1 slack when the space allows). Returns 0 or -1. */
int  index_compact(void);
/*
 * Finds issues matching every query token (last token as prefix) and scores
 * them (tf * 10 + recency boost). ids/scores are scratch allocations returned
 * to the caller; scores is indexed by issue id. Returns hit count or -1 when
 * scratch is exhausted. *scanned receives the number of postings visited.
 */
int  index_search(const char* q, u32 qlen, u16** ids_out, i32** scores_out, u32* scanned, i64 now);

#if defined(K32_NATIVE)
/* Test helper: checks the index against a fresh tokenization of every live
 * issue (posting sets and tf per term), the dictionary order, the hash and
 * the block region. Returns 0 or a negative code naming the first problem. */
int  index_verify(void);
#endif
#endif
