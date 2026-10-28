#ifndef TESSERA_APP_H
#define TESSERA_APP_H
#include "k32lib.h"
#ifndef TESSERA_ARENA_BYTES
#define TESSERA_ARENA_BYTES (1392u * 1024u)
#endif
#ifndef TESSERA_SCRATCH_BYTES
#define TESSERA_SCRATCH_BYTES (64u * 1024u)
#endif

typedef struct {
    u32 arena_size, arena_used, tables, scratch, strings, postings, postings_leaked;
    u32 issue_cap, issue_count, comment_cap, comment_count, term_cap, term_count, posting_count;
    u32 issue_rec, comment_rec, act_rec, session_rec, term_rec;
} tessera_layout;

/* Native-only helpers used by host_test; harmless in the wasm build. */
void  tessera_layout_get(tessera_layout* out);
u8*   tessera_resolve(u32 ptr);     /* ABI u32 "pointer" -> host pointer */
#endif
