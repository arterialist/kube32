#include "journal.h"
#include "store.h"
#include "index.h"

_Static_assert(TLOG_F_TITLE == F_TITLE && TLOG_F_DESCRIPTION == F_DESCRIPTION && TLOG_F_STATUS == F_STATUS &&
               TLOG_F_PRIORITY == F_PRIORITY && TLOG_F_ASSIGNEE == F_ASSIGNEE && TLOG_F_LABELS == F_LABELS &&
               TLOG_F_ESTIMATE == F_ESTIMATE, "record fields use the activity field numbers");
_Static_assert(TLOG_NO_USER == T_NO_USER, "one encoding for 'no user'");

static int user_ok(u8 u) { return u < T_USERS_N; }

static int apply_create(const tlog_issue* r) {
    if (r->id != g.issue_count + 1) return J_ID;
    if (r->project < 1 || r->project > T_PROJECTS_N || r->status >= T_STATUS_N || r->priority >= T_PRIO_N ||
        !(user_ok(r->assignee) || r->assignee == T_NO_USER) || !user_ok(r->reporter) ||
        r->title_len < 1 || r->title_len > T_TITLE_MAX || r->desc_len > T_DESC_MAX) return J_BAD;
    int id = issue_create(r->project, r->title, r->title_len, r->desc, r->desc_len, r->status, r->priority,
                          r->assignee, r->reporter, r->labels, r->estimate, r->created);
    if (id < 0) return id == -2 ? J_BAD : J_FULL;
    index_add_issue((u32)id);          /* a full term dictionary leaves new words unindexed */
    act_push(r->created, r->reporter, OP_CREATE, (u16)id, F_NONE, 0, 0);
    return J_OK;
}

static int apply_update(const tlog_update* r) {
    issue_t* it = issue_by_id(r->id);
    if (!it) return J_ID;
    int text = 0;
    for (u32 i = 0; i < r->count; i++) {
        const tlog_change* c = &r->ch[i];
        if (c->field == F_TITLE && (c->len < 1 || c->len > T_TITLE_MAX)) return J_BAD;
        if (c->field == F_DESCRIPTION && c->len > T_DESC_MAX) return J_BAD;
        if (c->field == F_STATUS && (c->to < 0 || c->to >= T_STATUS_N)) return J_BAD;
        if (c->field == F_PRIORITY && (c->to < 0 || c->to >= T_PRIO_N)) return J_BAD;
        if (c->field == F_ASSIGNEE && (c->to < -1 || c->to >= T_USERS_N)) return J_BAD;
        if (c->field == F_ESTIMATE && (c->to < 0 || c->to > 255)) return J_BAD;
        if (c->field == F_TITLE || c->field == F_DESCRIPTION) text = 1;
    }
    if (text) index_remove_issue(r->id);
    int rc = J_OK;
    for (u32 i = 0; i < r->count && rc == J_OK; i++) {
        const tlog_change* c = &r->ch[i];
        switch (c->field) {
            case F_TITLE: {
                u16 tl = it->title_len;
                if (str_replace(&it->title_off, &tl, c->text, c->len, T_TITLE_MAX) < 0) { rc = J_FULL; break; }
                it->title_len = (u8)tl;
                it->flags = (u8)((it->flags & ~ISSUE_TITLE_ESC) | (k32_jw_needs_escape(c->text, c->len) ? ISSUE_TITLE_ESC : 0));
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_TITLE, 0, 0);
                break;
            }
            case F_DESCRIPTION:
                if (c->len == 0) it->desc_len = 0;
                else if (str_replace(&it->desc_off, &it->desc_len, c->text, c->len, T_DESC_MAX) < 0) { rc = J_FULL; break; }
                it->flags = (u8)((it->flags & ~ISSUE_DESC_ESC) | (c->len && k32_jw_needs_escape(c->text, c->len) ? ISSUE_DESC_ESC : 0));
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_DESCRIPTION, 0, 0);
                break;
            case F_STATUS:
                issue_set_status(it, (u8)c->to, r->ts);
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_STATUS, c->from, c->to);
                break;
            case F_PRIORITY:
                it->priority = (u8)c->to;
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_PRIORITY, c->from, c->to);
                break;
            case F_ASSIGNEE:
                it->assignee = c->to < 0 ? T_NO_USER : (u8)c->to;
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_ASSIGNEE, c->from, c->to);
                break;
            case F_LABELS:
                it->labels = (u16)(c->to & ((1 << T_LABELS_N) - 1));
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_LABELS, c->from, c->to);
                break;
            case F_ESTIMATE:
                it->estimate = (u8)c->to;
                act_push(r->ts, r->actor, OP_UPDATE, (u16)r->id, F_ESTIMATE, c->from, c->to);
                break;
            default: rc = J_BAD; break;
        }
    }
    if (text) index_add_issue(r->id);
    it->updated = ts_pack(r->ts);
    return rc;
}

static int apply_comment(const tlog_comment* r) {
    if (r->id != g.comment_count + 1 || !issue_by_id(r->issue)) return J_ID;
    if (r->len < 1 || r->len > T_COMMENT_MAX || !user_ok(r->author)) return J_BAD;
    int cid = comment_add(r->issue, r->author, r->body, r->len, r->ts);
    if (cid < 0) return cid == -3 ? J_BAD : J_FULL;
    index_add_comment(r->issue, r->body, r->len);   /* merges the new tokens; same result as remove + re-add */
    act_push(r->ts, r->author, OP_COMMENT, (u16)r->issue, F_NONE, 0, cid);
    return J_OK;
}

static int apply_delete(const tlog_delete* r) {
    issue_t* it = issue_by_id(r->id);
    if (!it) return J_ID;
    index_remove_issue(r->id);
    act_push(r->ts, r->actor, OP_DELETE, (u16)r->id, F_NONE, 0, 0);
    it->flags |= ISSUE_DELETED;
    it->updated = ts_pack(r->ts);
    g.issue_live--;
    g.comment_live -= it->comment_count;
    if (g.last_viewed == r->id) g.last_viewed = 0;
    return J_OK;
}

int journal_apply(const u8* rec) {
    const u8* p = tlog_payload(rec);
    u32 len = tlog_len(rec);
    int rc;
    switch (tlog_type(rec)) {
        case TLOG_ISSUE_CREATE: {
            tlog_issue r;
            if (tlog_get_issue(p, len, &r) < 0) return J_BAD;
            store_observe_ts(r.created);
            rc = apply_create(&r);
            break;
        }
        case TLOG_ISSUE_UPDATE: {
            tlog_update r;
            if (tlog_get_update(p, len, &r) < 0) return J_BAD;
            store_observe_ts(r.ts);
            rc = apply_update(&r);
            break;
        }
        case TLOG_COMMENT: {
            tlog_comment r;
            if (tlog_get_comment(p, len, &r) < 0) return J_BAD;
            store_observe_ts(r.ts);
            rc = apply_comment(&r);
            break;
        }
        case TLOG_ISSUE_DELETE: {
            tlog_delete r;
            if (tlog_get_delete(p, len, &r) < 0) return J_BAD;
            store_observe_ts(r.ts);
            rc = apply_delete(&r);
            break;
        }
        default: rc = J_OK; break;
    }
    return rc;
}
