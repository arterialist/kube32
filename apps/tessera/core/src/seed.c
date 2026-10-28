/*
 * Deterministic seed data. Titles, descriptions and comments are assembled
 * from word lists so that full-text search over them is meaningful.
 */
#include "seed.h"
#include "store.h"
#include "index.h"

static const char* const k_components[] = {
    "wifi driver", "esp-now link", "scheduler", "kubelet", "radix store", "flash journal",
    "ingress proxy", "sse stream", "display driver", "keyboard matrix", "i2c bus", "spi dma path",
    "psram allocator", "wasm3 runtime", "gas meter", "lease controller", "mdns responder",
    "chunked encoder", "cookie parser", "audio amplifier", "adc sampler", "uart console",
    "ota updater", "littlefs mount", "watchdog", "metrics api", "capability firewall",
    "pod sandbox", "event bus", "token bucket", "search index", "board view", "session cache",
    "json writer", "request router", "heartbeat timer", "node agent", "tft backlight",
};
static const char* const k_symptoms[] = {
    "crashes", "hangs", "leaks memory", "drops frames", "times out", "returns stale data",
    "corrupts state", "stalls", "reboots the node", "logs garbage", "double-frees a buffer",
    "ignores backpressure", "misreports uptime", "truncates output", "starves the api server",
    "spins at 100% cpu", "loses the lease", "reorders events", "panics on empty input",
    "overflows its ring buffer", "sends duplicate frames", "fails crc validation",
};
static const char* const k_conditions[] = {
    "after reconnect", "under load", "on cold boot", "when psram is full", "during ota",
    "with three pods scheduled", "after 24h uptime", "on node-02", "when the link is lossy",
    "at 240 mhz", "with chunked bodies", "on brownout", "after failover", "when kubectl times out",
    "with large manifests", "while the keyboard is held", "when sse clients disconnect",
    "with search queries over 8 tokens", "after a journal replay", "when the heap is fragmented",
};
static const char* const k_actions[] = {
    "Refactor", "Harden", "Add", "Remove", "Document", "Benchmark", "Rewrite", "Expose",
    "Tune", "Cache", "Throttle", "Validate", "Instrument", "Migrate", "Split", "Simplify",
};
static const char* const k_goals[] = {
    "to cut heap usage", "for the 256 kb nodes", "before the demo", "to unblock the hud pod",
    "behind a feature flag", "with a ring buffer", "using dma", "without blocking the api server",
    "for faster boot", "to match kubectl output", "to survive brownout", "with crc32 checks",
    "to keep p99 under 20 ms", "so the board view stays responsive", "for the keyboard input path",
    "to stay under the 1536 ki pod limit",
};
static const char* const k_nouns[] = {
    "espnow frame", "crc", "dma descriptor", "gpio irq", "i2c ack", "spi clock", "udp socket",
    "http header", "json payload", "tcp window", "flash sector", "heap block", "ring buffer",
    "mutex", "semaphore", "queue", "timer", "lease", "manifest", "pod spec", "cookie",
    "posting list", "term dictionary", "activity ring", "scratch arena", "session token",
    "retry counter", "frame reassembly table", "backlight pwm", "tone generator",
};
static const char* const k_nodes[] = {
    "esp32-node-01", "esp32-node-02", "the host simulator", "a fresh board", "the ci rig",
};
static const char* const k_details[] = {
    "a steady climb", "zero activity", "bursts every 2 s", "a crc mismatch", "retries piling up",
    "a 300 ms gap", "two owners for one lease", "an off-by-one in the page count",
    "frames arriving out of order", "a heap low-water mark of 11 kb",
};
static const char* const k_adjectives[] = {
    "suspicious", "fine", "unrelated", "saturated", "starved", "racy", "stuck", "healthy",
};
static const char* const k_steps[] = {
    "deploy two pods and pull the antenna", "run kubectl get pods in a loop",
    "hold enter on the keyboard for 10 s", "power cycle node-02 mid-request",
    "send 50 searches in a row", "patch the same issue from two tabs",
    "open the board with 40 cards per column", "let the session idle for a day",
    "post a 1000 char description", "replay the journal twice",
};
static const char* const k_expected[] = {
    "no frame loss", "the lease renews within 5 s", "the hud redraws once", "a clean 404",
    "heap returns to baseline", "the comment shows up in every browser", "p99 stays flat",
    "the card moves without a refetch", "a 400 with a validation message", "boot in under 2 s",
};
static const char* const k_comment_tpl[] = {
    "Reproduced on {node}, {detail}.",
    "Caused by the {noun}, I think.",
    "Bumping priority, this blocks {goal}.",
    "Fixed on the branch, please verify.",
    "Cannot reproduce {condition}.",
    "Logs show {detail} {condition}.",
    "Moving to review.",
    "Split out the {noun} handling?",
    "Same with the {component} {condition}.",
    "After the fix: {detail}, looks {adjective}.",
    "Needs a test: {step}.",
    "Expected {expected}, got {detail}.",
};

#define COUNT(a) ((u32)(sizeof(a) / sizeof((a)[0])))

typedef struct { char* buf; u32 len, cap; } sb;
static void sb_put(sb* b, const char* s) {
    u32 n = (u32)strlen(s);
    if (n > b->cap - b->len) n = b->cap - b->len;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
}
static void sb_cap_first(sb* b) {
    if (b->len && b->buf[0] >= 'a' && b->buf[0] <= 'z') b->buf[0] = (char)(b->buf[0] - 32);
}
static const char* pick(k32_rng* r, const char* const* list, u32 n) { return list[k32_rng_range(r, n)]; }

static void expand(sb* b, k32_rng* r, const char* tpl) {
    for (const char* p = tpl; *p; ) {
        if (*p == '{') {
            const char* e = p + 1;
            while (*e && *e != '}') e++;
            u32 n = (u32)(e - p - 1);
            const char* s = "";
            if (k32_streq(p + 1, n, "node")) s = pick(r, k_nodes, COUNT(k_nodes));
            else if (k32_streq(p + 1, n, "detail")) s = pick(r, k_details, COUNT(k_details));
            else if (k32_streq(p + 1, n, "noun")) s = pick(r, k_nouns, COUNT(k_nouns));
            else if (k32_streq(p + 1, n, "component")) s = pick(r, k_components, COUNT(k_components));
            else if (k32_streq(p + 1, n, "goal")) s = pick(r, k_goals, COUNT(k_goals));
            else if (k32_streq(p + 1, n, "condition")) s = pick(r, k_conditions, COUNT(k_conditions));
            else if (k32_streq(p + 1, n, "symptom")) s = pick(r, k_symptoms, COUNT(k_symptoms));
            else if (k32_streq(p + 1, n, "adjective")) s = pick(r, k_adjectives, COUNT(k_adjectives));
            else if (k32_streq(p + 1, n, "step")) s = pick(r, k_steps, COUNT(k_steps));
            else if (k32_streq(p + 1, n, "expected")) s = pick(r, k_expected, COUNT(k_expected));
            sb_put(b, s);
            p = *e ? e + 1 : e;
        } else {
            if (b->len < b->cap) b->buf[b->len++] = *p;
            p++;
        }
    }
}

static void make_title(sb* b, k32_rng* r) {
    u32 kind = k32_rng_range(r, 10);
    if (kind < 5) {
        sb_put(b, pick(r, k_components, COUNT(k_components))); sb_put(b, " ");
        sb_put(b, pick(r, k_symptoms, COUNT(k_symptoms))); sb_put(b, " ");
        sb_put(b, pick(r, k_conditions, COUNT(k_conditions)));
    } else if (kind < 8) {
        sb_put(b, pick(r, k_actions, COUNT(k_actions))); sb_put(b, " ");
        sb_put(b, pick(r, k_components, COUNT(k_components))); sb_put(b, " ");
        sb_put(b, pick(r, k_goals, COUNT(k_goals)));
    } else {
        sb_put(b, pick(r, k_nouns, COUNT(k_nouns))); sb_put(b, " ");
        sb_put(b, pick(r, k_symptoms, COUNT(k_symptoms))); sb_put(b, " in ");
        sb_put(b, pick(r, k_components, COUNT(k_components)));
    }
    sb_cap_first(b);
}
static void make_desc(sb* b, k32_rng* r) {
    static const char* const s1[] = {
        "Observed on {node}: the {component} {symptom} {condition}.",
        "Seen {condition} on {node}; the {component} {symptom} within a minute.",
        "While testing the {component} on {node} it {symptom} {condition}.",
    };
    static const char* const s2[] = {
        " Logs show {detail}.",
        " The {noun} looks {adjective}.",
        " Repro: {step}.",
        " Expected {expected}.",
        "", "",
    };
    expand(b, r, pick(r, s1, COUNT(s1)));
    expand(b, r, pick(r, s2, COUNT(s2)));
}
static void make_comment(sb* b, k32_rng* r) {
    expand(b, r, pick(r, k_comment_tpl, COUNT(k_comment_tpl)));
    sb_cap_first(b);
}

static u8 pick_status(k32_rng* r) {
    u32 v = k32_rng_range(r, 100);
    if (v < 40) return ST_DONE;
    if (v < 65) return ST_BACKLOG;
    if (v < 80) return ST_TODO;
    if (v < 92) return ST_IN_PROGRESS;
    return ST_REVIEW;
}
static u8 pick_priority(k32_rng* r) {
    u32 v = k32_rng_range(r, 100);
    if (v < 10) return 0;
    if (v < 35) return 1;
    if (v < 70) return 2;
    if (v < 90) return 3;
    return 4;
}
static u8 pick_project(k32_rng* r) {
    u32 v = k32_rng_range(r, 100);
    return v < 60 ? 1 : (v < 85 ? 2 : 3);
}
static u16 pick_labels(k32_rng* r) {
    u32 v = k32_rng_range(r, 10);
    u32 n = v < 2 ? 0 : (v < 7 ? 1 : (v < 9 ? 2 : 3));
    u16 m = 0;
    for (u32 i = 0; i < n; i++) m |= (u16)(1u << k32_rng_range(r, T_LABELS_N));
    return m;
}
static u8 pick_estimate(k32_rng* r) {
    static const u8 fib[] = { 0, 1, 2, 3, 5, 8, 13 };
    return fib[k32_rng_range(r, 7)];
}
static u32 pick_comment_count(k32_rng* r) {
    u32 v = k32_rng_range(r, 100);
    if (v < 20) return 0;
    if (v < 50) return 1;
    if (v < 75) return 2;
    if (v < 90) return 3;
    if (v < 97) return 4;
    return 6;
}

/*
 * The seeder is resumable: seed_begin fixes the targets and the RNG, and
 * every seed_step call generates up to max_issues more issues (with their
 * comments, indexed right away) from the carried RNG state, so the data set
 * is byte-for-byte the one a single pass would produce. The call that seeds
 * the last issue also distributes the leftover comments, seeds the activity
 * ring and packs the posting region.
 */
void seed_begin(u32 n_issues, u32 n_comments, i64 now) {
    seed_state* s = &g.seed;
    memset(s, 0, sizeof *s);
    k32_rng_seed(&s->rng, 0x51ED5EEDu);
    s->n_issues = n_issues; s->n_comments = n_comments; s->now = now;
    s->comments_left = n_comments;
    s->phase = SEED_ISSUES;
}
int seed_active(void) { return g.seed.phase == SEED_ISSUES; }
int seed_finished(void) { return g.seed.phase == SEED_DONE; }
void seed_progress(u32* done, u32* total) { *done = g.seed.next; *total = g.seed.n_issues; }

static int seed_finish(seed_state* s, char* cbuf) {
    k32_rng* r = &s->rng;
    const i64 now = s->now;
    /* distribute any leftover comments over random issues */
    u32 guard = 0;
    while (s->comments_left > 0 && guard++ < s->n_comments + 16) {
        u32 id = 1 + k32_rng_range(r, s->n_issues);
        issue_t* it = issue_by_id(id);
        if (!it) continue;
        sb cb = { cbuf, 0, T_COMMENT_MAX };
        make_comment(&cb, r);
        i64 cts = ts_ms(it->updated) + 1 + (i64)k32_rng_range(r, 3600000u);
        if (cts > now - 1000) cts = now - 1000;
        if (comment_add(id, (u8)(1 + k32_rng_range(r, T_HUMAN_USERS)), cbuf, cb.len, cts) < 0) break;
        index_add_comment(id, cbuf, cb.len);
        s->comments_left--;
    }
    /* seed the activity ring with the most recent creations and resolutions */
    u32 first = s->n_issues > 400 ? s->n_issues - 400 : 0;
    for (u32 id = first + 1; id <= s->n_issues; id++) {
        const issue_t* it = issue_by_id(id);
        if (!it) continue;
        act_push(ts_ms(it->created), it->reporter, OP_CREATE, (u16)id, F_NONE, 0, 0);
        if (it->status == ST_DONE)
            act_push(ts_ms(it->resolved), it->assignee == T_NO_USER ? it->reporter : it->assignee, OP_UPDATE, (u16)id,
                     F_STATUS, ST_IN_PROGRESS, ST_DONE);
    }
    s->phase = SEED_DONE;
    return index_compact();
}

int seed_step(u32 max_issues) {
    seed_state* s = &g.seed;
    if (s->phase != SEED_ISSUES) return 0;
    k32_rng* r = &s->rng;
    char tbuf[T_TITLE_MAX + 8], dbuf[T_DESC_MAX + 8], cbuf[T_COMMENT_MAX + 8];
    const i64 now = s->now;
    const i64 span = 90 * T_DAY_MS;
    const i64 start = now - span;
    const u32 n_issues = s->n_issues;

    for (u32 done = 0; done < max_issues && s->next < n_issues; done++) {
        u32 i = s->next;
        sb t = { tbuf, 0, T_TITLE_MAX }; sb d = { dbuf, 0, T_DESC_MAX };
        make_title(&t, r);
        make_desc(&d, r);
        i64 slot = span / (i64)(n_issues ? n_issues : 1);
        i64 created = start + slot * (i64)i + (i64)k32_rng_range(r, (u32)(slot > 1 ? (slot < 0x7fffffff ? slot : 0x7fffffff) : 1));
        if (created > now - 60000) created = now - 60000;
        if (created < start) created = start;
        u8 status = pick_status(r);
        u8 assignee = k32_rng_range(r, 10) == 0 ? T_NO_USER : (u8)(1 + k32_rng_range(r, T_HUMAN_USERS));
        u8 reporter = (u8)(1 + k32_rng_range(r, T_HUMAN_USERS));
        int id = issue_create(pick_project(r), tbuf, t.len, dbuf, d.len, status, pick_priority(r),
                              assignee, reporter, pick_labels(r), pick_estimate(r), created);
        if (id < 0) return id;
        issue_t* it = issue_slot((u32)id);
        i64 remaining = now - created;
        if (status == ST_DONE) {
            i64 cycle = (i64)(2 + k32_rng_range(r, 20 * 24)) * 3600000ll;   /* 2h .. 20d */
            if (cycle > remaining) cycle = remaining;
            it->resolved = ts_pack(created + cycle);
            it->updated = it->resolved;
        } else {
            i64 lim = remaining < 14 * T_DAY_MS ? remaining : 14 * T_DAY_MS;
            it->updated = ts_pack(created + (i64)k32_rng_range(r, (u32)(lim > 0 ? lim : 1)));
        }
        /* comments */
        u32 nc = pick_comment_count(r);
        if (nc > s->comments_left) nc = s->comments_left;
        i64 cts = created;
        for (u32 c = 0; c < nc; c++) {
            sb cb = { cbuf, 0, T_COMMENT_MAX };
            make_comment(&cb, r);
            i64 gap = remaining > 0 ? (i64)k32_rng_range(r, (u32)((remaining / (i64)(nc + 1)) > 0x7fffffff ? 0x7fffffff : (remaining / (i64)(nc + 1)))) : 0;
            cts += gap + 1;
            if (cts > now - 1000) cts = now - 1000;
            int cid = comment_add((u32)id, (u8)(1 + k32_rng_range(r, T_HUMAN_USERS)), cbuf, cb.len, cts);
            if (cid < 0) return cid;
            s->comments_left--;
        }
        if (it->updated > ts_pack(now)) it->updated = ts_pack(now);
        s->next = i + 1;
        if (index_add_issue((u32)id) < 0) return -5;
    }
    if (s->next < n_issues) return 1;
    return seed_finish(s, cbuf);
}

int seed_run(u32 n_issues, u32 n_comments, i64 now) {
    seed_begin(n_issues, n_comments, now);
    int rc;
    while ((rc = seed_step(n_issues)) > 0) {}
    return rc;
}
