#ifndef TESSERA_SEED_H
#define TESSERA_SEED_H
#include "k32lib.h"
/* Resumable seeding (state lives in g.seed). seed_step returns 1 while
 * issues remain, 0 once everything (issues, leftover comments, activity
 * ring, posting compaction) is done, negative on failure. */
void seed_begin(u32 n_issues, u32 n_comments, i64 now);
int  seed_step(u32 max_issues);
int  seed_active(void);          /* issues still to seed (ticks continue) */
int  seed_finished(void);        /* the full set is in place */
void seed_progress(u32* done, u32* total);
/* seed_begin + seed_step until done, in one call */
int  seed_run(u32 n_issues, u32 n_comments, i64 now);
#endif
