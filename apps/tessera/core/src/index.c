#include "index.h"
#include "store.h"

typedef struct { u16 term; u16 tf; } bag_entry;

static posting_t* term_postings(const term_t* t) { return (posting_t*)(g.arena.base + t->post_off); }
static const char* term_str(const term_t* t) { return str_at(t->str_off); }

/* ---- dictionary ----------------------------------------------------------
 * Terms keep their index for life (insertion order). Exact lookups go through
 * an open-addressing hash (slot = index + 1, load <= 0.5, linear probing);
 * prefix walks use term_order, the indices sorted by string. */
static int term_find(const char* s, u32 len) {
    u32 h = k32_hash(s, len) & g.term_hash_mask;
    for (;;) {
        u16 slot = g.term_hash[h];
        if (!slot) return -1;
        const term_t* t = &g.terms[slot - 1];
        K32_PROF(term_cmps);
        if (t->len == len && memcmp(term_str(t), s, len) == 0) return (int)(slot - 1);
        h = (h + 1) & g.term_hash_mask;
    }
}
/* first position in term_order whose string is >= s */
static u32 term_lower_bound(const char* s, u32 len) {
    u32 lo = 0, hi = g.term_count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const term_t* t = &g.terms[g.term_order[mid]];
        K32_PROF(term_cmps);
        if (k32_strcmp(term_str(t), t->len, s, len) < 0) lo = mid + 1; else hi = mid;
    }
    return lo;
}
static int term_insert(const char* s, u32 len) {       /* only after term_find failed */
    if (g.term_count >= g.term_cap || len == 0 || len > 255) return -1;
    u32 off = str_store(s, len);
    if (!off) return -1;
    u32 id = g.term_count;
    term_t* t = &g.terms[id];
    memset(t, 0, sizeof *t);
    t->str_off = off; t->len = (u8)len;
    u32 pos = term_lower_bound(s, len);
    if (pos < id) memmove(&g.term_order[pos + 1], &g.term_order[pos], (id - pos) * sizeof(u16));
    g.term_order[pos] = (u16)id;
    u32 h = k32_hash(s, len) & g.term_hash_mask;
    while (g.term_hash[h]) h = (h + 1) & g.term_hash_mask;
    g.term_hash[h] = (u16)(id + 1);
    g.term_count++;
    return (int)id;
}

/* ---- posting blocks ------------------------------------------------------
 * Posting arrays live in the top-down region [arena.limit, arena.size) as
 * contiguous blocks:  [payload: padded(cap) bytes][trailer: u16 term+1, u16 cap]
 * with term+1 == 0 marking a dead block. The trailer sits at the high end so
 * a walk from arena.size downward visits every block without sorting.
 * Growing a term allocates a fresh block and marks the old one dead; when
 * the region cannot grow, compact() slides live blocks upward in place
 * (dst >= src, overlapping memmove) and drops the dead ones. Postings inside
 * a block are kept in ascending issue-id order by every writer. */
#define BLK_HDR 4u
static u32 padded(u32 cap) { return (cap * 3u + 3u) & ~3u; }
static u32 blk_bytes(u32 cap) { return padded(cap) + BLK_HDR; }
static u16* blk_trailer(u32 payload_off, u32 cap) { return (u16*)(g.arena.base + payload_off + padded(cap)); }

static int compact(int slack) {
    K32_PROF(compactions);
    u32 dst = g.arena.size;           /* end (exclusive) of the last packed block */
    u32 off = g.arena.size;           /* walk cursor: end of the next block to visit */
    u32 live = 0;
    while (off > g.arena.limit) {
        if (off - g.arena.limit < BLK_HDR) return -1;
        const u16* tr = (const u16*)(g.arena.base + off - BLK_HDR);
        u32 owner = tr[0], cap = tr[1];
        if (off - BLK_HDR - g.arena.limit < padded(cap)) return -1;
        u32 src = off - BLK_HDR - padded(cap);
        off = src;
        if (!owner) continue;
        term_t* t = &g.terms[owner - 1];
        if (t->post_off != src || owner > g.term_count) return -1;
        if (t->count == 0) { t->post_off = 0; t->cap = 0; continue; }      /* drop empty blocks */
        /* everything above src is packed or dead, so dst - BLK_HDR - src >= padded(cap) >= padded(count) */
        u32 avail = dst - BLK_HDR - src;
        u32 ncap = t->count + (slack ? t->count / 16u + 1u : 0u);
        if (padded(ncap) > avail) ncap = avail / 3u;
        if (ncap > 65535u) ncap = 65535u;
        u32 ndst = dst - BLK_HDR - padded(ncap);
        if (ndst != src) memmove(g.arena.base + ndst, g.arena.base + src, t->count * sizeof(posting_t));
        u16* ntr = blk_trailer(ndst, ncap); ntr[0] = (u16)owner; ntr[1] = (u16)ncap;
        t->post_off = ndst; t->cap = (u16)ncap;
        live += ncap * 3u;
        dst = ndst;
    }
    g.arena.limit = dst;
    g.posting_bytes = live;
    g.posting_leaked = 0;
    return 0;
}
int index_compact(void) { return compact(1); }

static int term_grow(term_t* t) {
    u32 ncap = t->cap ? t->cap * 2u : 4u;
    if (ncap > 65535u) ncap = 65535u;
    if (ncap <= t->cap) return -1;
    u32 bytes = blk_bytes(ncap);
    u8* p = (u8*)k32_arena_alloc_top(&g.arena, bytes, 4);
    if (!p) {
        if (compact(1) < 0) return -1;
        if (t->count < t->cap) return 0;          /* compaction already gave it slack */
        p = (u8*)k32_arena_alloc_top(&g.arena, bytes, 4);
        if (!p) return -1;
    }
    u32 noff = (u32)(p - g.arena.base);
    if (t->count) memcpy(p, term_postings(t), t->count * sizeof(posting_t));
    if (t->post_off) {
        blk_trailer(t->post_off, t->cap)[0] = 0;
        g.posting_leaked += blk_bytes(t->cap);
        g.posting_bytes -= t->cap * 3u;
    }
    u16* tr = blk_trailer(noff, ncap); tr[0] = (u16)((t - g.terms) + 1); tr[1] = (u16)ncap;
    t->post_off = noff; t->cap = (u16)ncap;
    g.posting_bytes += ncap * 3u;
    return 0;
}
/* binary search by issue id; index or -1 */
static int posting_find(const term_t* t, u16 id) {
    const posting_t* p = term_postings(t);
    u32 lo = 0, hi = t->count;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        K32_PROF(posting_steps);
        if (p[mid].id < id) lo = mid + 1; else hi = mid;
    }
    return (lo < t->count && p[lo].id == id) ? (int)lo : -1;
}
static int term_add_posting(term_t* t, u16 id, u16 tf) {
    if (t->count >= t->cap && term_grow(t) < 0) return -1;
    posting_t* p = term_postings(t);
    u32 pos = t->count;
    if (pos && p[pos - 1].id >= id) {            /* out-of-order add: keep the array sorted */
        u32 lo = 0, hi = pos;
        while (lo < hi) { u32 mid = (lo + hi) / 2; K32_PROF(posting_steps); if (p[mid].id < id) lo = mid + 1; else hi = mid; }
        pos = lo;
        memmove(&p[pos + 1], &p[pos], (t->count - pos) * sizeof(posting_t));
    }
    p[pos].id = id; p[pos].tf = tf > 255 ? 255 : (u8)tf;
    t->count++;
    g.posting_count++;
    return 0;
}
static void term_remove_posting(term_t* t, u16 id) {
    int i = posting_find(t, id);
    if (i < 0) return;
    posting_t* p = term_postings(t);
    if ((u32)i + 1 < t->count) memmove(&p[i], &p[i + 1], (t->count - (u32)i - 1) * sizeof(posting_t));
    t->count--;
    g.posting_count--;
}

/*
 * Build the term bag of a text: distinct term indices with weighted tf.
 * When insert_missing is set, unknown terms are added to the dictionary.
 */
static void bag_add(bag_entry* bag, u32* n, const char* tok, u32 len, u16 w, int insert_missing) {
    if (k32_is_stopword(tok, len)) return;
    int idx = term_find(tok, len);
    if (idx < 0) {
        if (!insert_missing) return;
        idx = term_insert(tok, len);
        if (idx < 0) return;
    }
    for (u32 i = 0; i < *n; i++) {
        if (bag[i].term == (u16)idx) {
            u32 tf = (u32)bag[i].tf + w;
            bag[i].tf = tf > 65535u ? 65535u : (u16)tf;
            return;
        }
    }
    if (*n < IDX_BAG_MAX) { bag[*n].term = (u16)idx; bag[*n].tf = w; (*n)++; }
}
static void bag_text(bag_entry* bag, u32* n, const char* s, u32 len, u16 w, int insert_missing) {
    k32_tok t; char tok[K32_TOKEN_MAX];
    k32_tok_init(&t, s, len);
    u32 tl;
    while ((tl = k32_tok_next(&t, tok, sizeof tok)) > 0) bag_add(bag, n, tok, tl, w, insert_missing);
}
static u32 bag_issue(const issue_t* it, u32 id, bag_entry* bag, int insert_missing) {
    u32 n = 0;
    bag_text(bag, &n, str_at(it->title_off), it->title_len, 3, insert_missing);
    if (it->desc_len) bag_text(bag, &n, str_at(it->desc_off), it->desc_len, 1, insert_missing);
    u32 cid = it->comment_head; u32 guard = 0;
    while (cid && guard++ < 65535) {
        const comment_t* c = comment_by_id(cid);
        if (!c) break;
        if ((u32)c->issue == id) bag_text(bag, &n, str_at(c->body_off), c->body_len, 1, insert_missing);
        cid = c->next;
    }
    return n;
}

int index_add_issue(u32 id) {
    const issue_t* it = issue_by_id(id);
    if (!it) return -1;
    u32 save = g.scratch_top;
    bag_entry* bag = (bag_entry*)scratch_alloc(IDX_BAG_MAX * sizeof(bag_entry));
    if (!bag) return -1;
    u32 n = bag_issue(it, id, bag, 1);
    int rc = 0;
    for (u32 i = 0; i < n; i++) if (term_add_posting(&g.terms[bag[i].term], (u16)id, bag[i].tf) < 0) rc = -1;
    g.scratch_top = save;
    return rc;
}
void index_remove_issue(u32 id) {
    const issue_t* it = issue_by_id(id);
    if (!it) return;
    u32 save = g.scratch_top;
    bag_entry* bag = (bag_entry*)scratch_alloc(IDX_BAG_MAX * sizeof(bag_entry));
    if (!bag) return;
    u32 n = bag_issue(it, id, bag, 0);
    for (u32 i = 0; i < n; i++) term_remove_posting(&g.terms[bag[i].term], (u16)id);
    g.scratch_top = save;
}
int index_add_comment(u32 id, const char* body, u32 len) {
    if (!issue_by_id(id)) return -1;
    u32 save = g.scratch_top;
    bag_entry* bag = (bag_entry*)scratch_alloc(IDX_BAG_MAX * sizeof(bag_entry));
    if (!bag) return -1;
    u32 n = 0;
    bag_text(bag, &n, body, len, 1, 1);
    int rc = 0;
    for (u32 i = 0; i < n; i++) {
        term_t* t = &g.terms[bag[i].term];
        int pos = t->post_off ? posting_find(t, (u16)id) : -1;
        if (pos >= 0) {
            posting_t* p = term_postings(t);
            u32 tf = (u32)p[pos].tf + bag[i].tf;
            p[pos].tf = tf > 255u ? 255u : (u8)tf;
        } else if (term_add_posting(t, (u16)id, bag[i].tf) < 0) rc = -1;
    }
    g.scratch_top = save;
    return rc;
}

static void score_postings(const term_t* t, u32 k, i32* score, u8* mcount, u8* lastk, u32* scanned) {
    const posting_t* p = term_postings(t);
    for (u32 i = 0; i < t->count; i++) {
        u16 id = p[i].id;
        if (id == 0 || id > g.issue_cap) continue;
        if (lastk[id] != (u8)(k + 1)) { lastk[id] = (u8)(k + 1); mcount[id]++; }
        score[id] += (i32)p[i].tf * 10;
        (*scanned)++;
    }
}

int index_search(const char* q, u32 qlen, u16** ids_out, i32** scores_out, u32* scanned, i64 now) {
    char toks[IDX_MAX_QUERY_TOKENS][K32_TOKEN_MAX];
    u32 tlen[IDX_MAX_QUERY_TOKENS];
    u32 ntok = 0;
    *scanned = 0;
    {
        k32_tok t; char tok[K32_TOKEN_MAX]; u32 l;
        k32_tok_init(&t, q, qlen);
        while ((l = k32_tok_next(&t, tok, sizeof tok)) > 0) {
            /* keep the final token even when short: it is the prefix token */
            if (ntok < IDX_MAX_QUERY_TOKENS) { memcpy(toks[ntok], tok, l); tlen[ntok] = l; ntok++; }
        }
        /* drop stopwords except for the final (prefix) token */
        u32 w = 0;
        for (u32 i = 0; i < ntok; i++) {
            if (i + 1 < ntok && k32_is_stopword(toks[i], tlen[i])) continue;
            if (w != i) { memcpy(toks[w], toks[i], tlen[i]); tlen[w] = tlen[i]; }
            w++;
        }
        ntok = w;
    }
    u32 n = g.issue_cap + 1;
    i32* score = (i32*)scratch_alloc(n * sizeof(i32));
    u8* mcount = (u8*)scratch_alloc(n);
    u8* lastk  = (u8*)scratch_alloc(n);
    u16* ids   = (u16*)scratch_alloc(g.issue_cap * sizeof(u16));
    if (!score || !mcount || !lastk || !ids) return -1;
    *ids_out = ids; *scores_out = score;
    if (ntok == 0) return 0;
    memset(score, 0, n * sizeof(i32));
    memset(mcount, 0, n);
    memset(lastk, 0, n);

    for (u32 k = 0; k < ntok; k++) {
        if (k + 1 == ntok) {
            /* prefix range starting at the lower bound in sorted order */
            u32 start = term_lower_bound(toks[k], tlen[k]);
            u32 expanded = 0;
            for (u32 o = start; o < g.term_count && expanded < IDX_MAX_PREFIX_TERMS; o++) {
                const term_t* tm = &g.terms[g.term_order[o]];
                if (!k32_starts_with(term_str(tm), tm->len, toks[k], tlen[k])) break;
                if (tm->post_off) score_postings(tm, k, score, mcount, lastk, scanned);
                expanded++;
            }
        } else {
            int idx = term_find(toks[k], tlen[k]);
            if (idx >= 0 && g.terms[idx].post_off) score_postings(&g.terms[idx], k, score, mcount, lastk, scanned);
        }
    }
    u32 hits = 0;
    for (u32 id = 1; id <= g.issue_count; id++) {
        if (mcount[id] != (u8)ntok) continue;
        const issue_t* it = issue_by_id(id);
        if (!it) continue;
        i64 age = now - ts_ms(it->updated);
        if (age < T_DAY_MS) score[id] += 5;
        else if (age < 7 * T_DAY_MS) score[id] += 3;
        else if (age < 30 * T_DAY_MS) score[id] += 1;
        ids[hits++] = (u16)id;
    }
    return (int)hits;
}

#if defined(K32_NATIVE)
int index_verify(void) {
    /* dictionary: order sorted strictly, hash resolves every term */
    for (u32 o = 1; o < g.term_count; o++) {
        const term_t* a = &g.terms[g.term_order[o - 1]]; const term_t* b = &g.terms[g.term_order[o]];
        if (k32_strcmp(term_str(a), a->len, term_str(b), b->len) >= 0) return -1;
    }
    for (u32 t = 0; t < g.term_count; t++)
        if (term_find(term_str(&g.terms[t]), g.terms[t].len) != (int)t) return -2;
    /* block region: every block accounted for, trailers consistent */
    {
        u32 off = g.arena.size, live_bytes = 0, dead_bytes = 0;
        while (off > g.arena.limit) {
            if (off - g.arena.limit < BLK_HDR) return -3;
            const u16* tr = (const u16*)(g.arena.base + off - BLK_HDR);
            u32 owner = tr[0], cap = tr[1];
            if (off - BLK_HDR - g.arena.limit < padded(cap)) return -3;
            u32 src = off - BLK_HDR - padded(cap);
            if (owner) {
                if (owner > g.term_count) return -4;
                const term_t* t = &g.terms[owner - 1];
                if (t->post_off != src || t->cap != cap || t->count > t->cap) return -5;
                live_bytes += cap * 3u;
            } else dead_bytes += blk_bytes(cap);
            off = src;
        }
        if (live_bytes != g.posting_bytes || dead_bytes != g.posting_leaked) return -6;
    }
    /* postings sorted by id, no duplicates, ids live */
    u32 total = 0;
    for (u32 t = 0; t < g.term_count; t++) {
        const term_t* tm = &g.terms[t];
        if (tm->count && !tm->post_off) return -7;
        const posting_t* p = term_postings(tm);
        for (u32 i = 0; i < tm->count; i++) {
            if (i && p[i - 1].id >= p[i].id) return -8;
            if (!issue_by_id(p[i].id)) return -9;
        }
        total += tm->count;
    }
    if (total != g.posting_count) return -10;
    /* content: the bag of every live issue is exactly its postings */
    u32 save = g.scratch_top;
    bag_entry* bag = (bag_entry*)scratch_alloc(IDX_BAG_MAX * sizeof(bag_entry));
    if (!bag) return -11;
    u32 matched = 0;
    int rc = 0;
    for (u32 id = 1; id <= g.issue_count && rc == 0; id++) {
        const issue_t* it = issue_by_id(id);
        if (!it) continue;
        u32 n = bag_issue(it, id, bag, 0);
        for (u32 i = 0; i < n; i++) {
            const term_t* tm = &g.terms[bag[i].term];
            int pos = tm->post_off ? posting_find(tm, (u16)id) : -1;
            u32 tf = bag[i].tf > 255u ? 255u : bag[i].tf;
            if (pos < 0 || term_postings(tm)[pos].tf != tf) { rc = -12; break; }
            matched++;
        }
    }
    g.scratch_top = save;
    if (rc) return rc;
    return matched == total ? 0 : -13;
}
#endif
