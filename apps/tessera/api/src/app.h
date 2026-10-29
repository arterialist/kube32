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

/* Exports besides k32_init / k32_alloc / k32_handle (common/k32_host.h). */
#include "../../common/k32_host.h"
K32_EXPORT("k32_tick")  int32_t k32_tick(uint32_t uptime_ms);
K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic_ptr, uint32_t topic_len, uint32_t ptr, uint32_t len);

/* Native-only helpers used by host_test; harmless in the wasm build. */
void  tessera_layout_get(tessera_layout* out);
u8*   tessera_resolve(u32 ptr);     /* ABI u32 "pointer" -> host pointer */
#endif
