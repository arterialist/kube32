#ifndef TESSERA_ROUTER_H
#define TESSERA_ROUTER_H
#include "k32lib.h"
#include "store.h"

typedef struct {
    const char* method; u32 mlen;
    const char* path;   u32 plen;
    const char* query;  u32 qlen;
    const char* cookie; u32 clen;
    char* body;         u32 blen;
    session_t* session; int user;      /* user = -1 when unauthenticated */
    i64 now, uptime, t0_ms;
} req_t;

/* Parses the host request encoding in place. Returns 0 or -1 (malformed). */
int  router_parse(char* buf, u32 len, req_t* rq);
/* Dispatches the request; always produces exactly one response; returns the status. */
int  router_dispatch(req_t* rq);

/* Events on channel tessera.events (payloads < K32_MAX_EVENT_BYTES). */
void events_issue(const char* op, u32 id, u8 actor);
void events_comment(u32 issue_id, u32 comment_id, u8 author);
void events_hud(const char* text, u32 len);

/* Writes a comment through to tessera-db, applies it and emits its event.
 * Returns the comment id, or minus the HTTP status (404, 503, 507). */
int  comment_commit(u32 issue_id, u8 author, const char* body, u32 len, i64 now);
u32  first_live_issue(void);
#endif
