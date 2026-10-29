#include "dataset.h"
#include "seed.h"
#include "../../db/src/tdb.h"
#include <stdlib.h>
#include <string.h>

enum { EV_CREATE, EV_STATUS, EV_COMMENT };
typedef struct { int64_t ts; uint32_t order; uint8_t kind; uint32_t idx; } event;

static int ev_cmp(const void* a, const void* b) {
    const event* x = (const event*)a; const event* y = (const event*)b;
    if (x->ts != y->ts) return x->ts < y->ts ? -1 : 1;
    return x->order < y->order ? -1 : (x->order > y->order);
}

typedef struct { tdb_t db; uint8_t batch[TDB_APPEND_MAX]; uint32_t len; int rc; } writer;
static void flush(writer* w) {
    uint32_t first, last;
    if (w->len == 0 || w->rc) return;
    int rc = tdb_append(&w->db, w->batch, w->len, 0, &first, &last);
    if (rc) w->rc = rc;
    w->len = 0;
}
static void put(writer* w, const uint8_t* rec, uint32_t n) {
    if (n == 0) { w->rc = -1; return; }
    if (w->len + n > sizeof w->batch) flush(w);
    memcpy(w->batch + w->len, rec, n);
    w->len += n;
}

int dataset_build(uint32_t n_issues, uint32_t n_comments, int64_t epoch_ms, dataset_stats* st) {
    memset(st, 0, sizeof *st);
    seed_issue* issues = calloc(n_issues ? n_issues : 1, sizeof *issues);
    seed_comment* comments = calloc(n_comments ? n_comments : 1, sizeof *comments);
    event* ev = calloc(2u * n_issues + n_comments + 1, sizeof *ev);
    writer* w = calloc(1, sizeof *w);
    int rc = -12;
    if (!issues || !comments || !ev || !w) goto out;
    uint32_t nc = 0;
    seed_generate(n_issues, n_comments, epoch_ms, issues, comments, &nc);

    /* time order; at equal times a create comes first, then status changes, then comments */
    uint32_t n = 0, order = 0;
    for (uint32_t i = 0; i < n_issues; i++) {
        ev[n++] = (event){ issues[i].created, order++, EV_CREATE, i };
        if (issues[i].status != SEED_BACKLOG) ev[n++] = (event){ issues[i].status_ts, order++, EV_STATUS, i };
    }
    for (uint32_t c = 0; c < nc; c++) ev[n++] = (event){ comments[c].ts, order++, EV_COMMENT, c };
    qsort(ev, n, sizeof *ev, ev_cmp);

    rc = tdb_open(&w->db);
    if (rc < 0) goto out;
    if (w->db.state != TDB_OK || w->db.last_seq != 0) { rc = -17; goto out; }
    uint8_t rec[TLOG_REC_MAX];
    uint32_t next_issue = 1, comment_id = 0;
    for (uint32_t k = 0; k < n && !w->rc; k++) {
        const event* e = &ev[k];
        if (e->kind == EV_CREATE) {
            const seed_issue* it = &issues[e->idx];
            if (it->id != next_issue++) { rc = -22; goto out; }   /* creation order must be id order */
            tlog_issue r = { it->id, it->created, it->project, SEED_BACKLOG, it->priority, it->assignee, it->reporter, it->estimate,
                             it->labels, it->title, it->title_len, it->desc, it->desc_len };
            put(w, rec, tlog_put_issue(rec, sizeof rec, &r));
            st->issues++;
        } else if (e->kind == EV_STATUS) {
            const seed_issue* it = &issues[e->idx];
            tlog_update u;
            memset(&u, 0, sizeof u);
            u.id = it->id; u.ts = it->status_ts; u.count = 1;
            u.actor = it->assignee != SEED_NO_USER ? it->assignee : it->reporter;
            u.ch[0].field = TLOG_F_STATUS; u.ch[0].from = SEED_BACKLOG; u.ch[0].to = it->status; u.ch[0].text = "";
            put(w, rec, tlog_put_update(rec, sizeof rec, &u));
            st->updates++;
        } else {
            const seed_comment* c = &comments[e->idx];
            tlog_comment r = { ++comment_id, c->issue, c->ts, c->author, c->body, c->len };
            put(w, rec, tlog_put_comment(rec, sizeof rec, &r));
            st->comments++;
        }
    }
    flush(w);
    rc = w->rc;
    st->records = w->db.last_seq;
    st->last_seq = w->db.last_seq;
    st->bytes = w->db.total_bytes;
    st->segments = w->db.nsegs;
out:
    free(issues); free(comments); free(ev); free(w);
    return rc;
}
