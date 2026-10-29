/*
 * Deterministic sample data for a fictional photo-sharing product. Titles,
 * descriptions and comments are assembled from word lists so that full-text
 * search over them is meaningful.
 */
#include "seed.h"

static const char* const k_components[] = {
    "feed ranking", "upload pipeline", "search autocomplete", "billing service", "notification worker", "iOS app",
    "android app", "web client", "cdn cache", "auth provider", "export job", "image resizer",
    "comment threads", "rate limiter", "analytics etl", "album sync", "share links", "onboarding flow",
    "push gateway", "email digest", "avatar service", "video transcoder", "stories feed", "tag suggestions",
    "payment webhook", "session store", "admin console", "dark mode", "offline cache", "deep links",
    "map view", "face grouping", "backup importer", "activity feed", "spam filter", "api gateway",
    "trash recovery", "profile page",
};
static const char* const k_symptoms[] = {
    "crashes", "hangs", "leaks memory", "drops the session", "times out", "returns stale results",
    "corrupts captions", "stalls", "logs out everyone", "shows a blank screen", "double-charges the card",
    "ignores the retry", "misreports the count", "truncates captions", "starves the queue",
    "spins at 100% cpu", "loses uploads", "reorders the feed", "fails on empty albums",
    "overflows the queue", "sends duplicate emails", "fails checksum validation",
};
static const char* const k_conditions[] = {
    "after the tuesday deploy", "under load", "on first launch", "when storage is full", "during a migration",
    "with three tabs open", "after 24h idle", "on the canary", "on slow 3g",
    "on safari", "with large uploads", "with 2fa enabled", "after failover", "when the token expires",
    "for accounts over 10k photos", "while a video is uploading", "when the app is backgrounded",
    "with search queries over 8 words", "after a cache purge", "on low-end android phones",
};
static const char* const k_actions[] = {
    "Refactor", "Harden", "Add", "Remove", "Document", "Benchmark", "Rewrite", "Expose",
    "Tune", "Cache", "Throttle", "Validate", "Instrument", "Migrate", "Split", "Simplify",
};
static const char* const k_goals[] = {
    "to cut memory usage", "for the free tier", "before the launch", "to unblock the ios release",
    "behind a feature flag", "with a job queue", "using a cdn", "without blocking the api",
    "for faster cold start", "to match the design spec", "to survive a region outage", "with idempotency keys",
    "to keep p99 under 200 ms", "so the feed stays responsive", "for the onboarding flow",
    "to stay under the storage quota",
};
static const char* const k_nouns[] = {
    "upload token", "checksum", "thumbnail", "push token", "oauth callback", "retry policy", "websocket",
    "http header", "json payload", "cursor", "storage bucket", "cache key", "job queue",
    "mutex", "semaphore", "queue", "timer", "feature flag", "manifest", "release build", "cookie",
    "search index", "term dictionary", "activity feed", "scratch buffer", "session token",
    "retry counter", "dedupe table", "signed url", "rate limit",
};
static const char* const k_envs[] = {
    "staging", "the canary", "production", "a fresh install", "the ci pipeline",
};
static const char* const k_details[] = {
    "a steady climb", "zero activity", "bursts every 2 s", "a checksum mismatch", "retries piling up",
    "a 300 ms gap", "two owners for one album", "an off-by-one in the page count",
    "photos arriving out of order", "a memory low-water mark of 11 mb",
};
static const char* const k_adjectives[] = {
    "suspicious", "fine", "unrelated", "saturated", "starved", "racy", "stuck", "healthy",
};
static const char* const k_steps[] = {
    "open two tabs and sign out of one", "run the export job in a loop",
    "hold refresh on the feed for 10 s", "kill the app mid-upload",
    "send 50 searches in a row", "patch the same issue from two tabs",
    "open the board with 40 cards per column", "let the session idle for a day",
    "post a 1000 char description", "replay the webhook twice",
};
static const char* const k_expected[] = {
    "no lost uploads", "the token refreshes within 5 s", "the feed redraws once", "a clean 404",
    "memory returns to baseline", "the comment shows up in every browser", "p99 stays flat",
    "the card moves without a refetch", "a 400 with a validation message", "cold start under 2 s",
};
static const char* const k_comment_tpl[] = {
    "Reproduced on {env}, {detail}.",
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
    /* Leave mixed-case brand words alone ("iOS app" stays "iOS app") */
    if (b->len > 1 && b->buf[1] >= 'A' && b->buf[1] <= 'Z') return;
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
            if (k32_streq(p + 1, n, "env")) s = pick(r, k_envs, COUNT(k_envs));
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
        "Observed on {env}: the {component} {symptom} {condition}.",
        "Seen {condition} on {env}; the {component} {symptom} within a minute.",
        "While testing the {component} on {env} it {symptom} {condition}.",
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
    if (v < 40) return SEED_DONE;
    if (v < 65) return SEED_BACKLOG;
    if (v < 80) return SEED_TODO;
    if (v < 92) return SEED_IN_PROGRESS;
    return SEED_REVIEW;
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
    for (u32 i = 0; i < n; i++) m |= (u16)(1u << k32_rng_range(r, SEED_LABELS));
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
 * Generation follows one RNG stream (seed 0x51ED5EED) in a fixed order:
 * every issue with its own comments, then the comments left over spread
 * over random issues. Timestamps are whole seconds relative to
 * now - 120 days (the API's packed time base), returned in milliseconds.
 */
#define DAY_MS 86400000ll
typedef struct { i64 base; } clock_t_;
static u32 pack(const clock_t_* c, i64 ms) {
    i64 s = (ms - c->base) / 1000;
    if (s < 0) return 0;
    if (s > 0xFFFFFFFFll) return 0xFFFFFFFFu;
    return (u32)s;
}
static i64 unpack(const clock_t_* c, u32 p) { return c->base + (i64)p * 1000; }

int seed_generate(u32 n_issues, u32 n_comments, i64 now, seed_issue* issues, seed_comment* comments, u32* comment_count) {
    k32_rng rng; k32_rng* r = &rng;
    k32_rng_seed(r, 0x51ED5EEDu);
    clock_t_ ck = { now - 120 * DAY_MS };
    const i64 span = 90 * DAY_MS, start = now - span;
    u32 left = n_comments, nc_total = 0;
    for (u32 i = 0; i < n_issues; i++) {
        seed_issue* it = &issues[i];
        sb t = { it->title, 0, SEED_TITLE_MAX }; sb d = { it->desc, 0, SEED_DESC_MAX };
        make_title(&t, r);
        make_desc(&d, r);
        it->title_len = t.len; it->desc_len = d.len;
        i64 slot = span / (i64)(n_issues ? n_issues : 1);
        i64 created = start + slot * (i64)i + (i64)k32_rng_range(r, (u32)(slot > 1 ? (slot < 0x7fffffff ? slot : 0x7fffffff) : 1));
        if (created > now - 60000) created = now - 60000;
        if (created < start) created = start;
        it->status = pick_status(r);
        it->assignee = k32_rng_range(r, 10) == 0 ? SEED_NO_USER : (u8)(1 + k32_rng_range(r, SEED_USERS));
        it->reporter = (u8)(1 + k32_rng_range(r, SEED_USERS));
        it->project = pick_project(r);
        it->priority = pick_priority(r);
        it->labels = pick_labels(r);
        it->estimate = pick_estimate(r);
        it->id = i + 1;
        u32 c_packed = pack(&ck, created);
        it->created = unpack(&ck, c_packed);
        i64 remaining = now - created;
        u32 upd;
        if (it->status == SEED_DONE) {
            i64 cycle = (i64)(2 + k32_rng_range(r, 20 * 24)) * 3600000ll;   /* 2h .. 20d */
            if (cycle > remaining) cycle = remaining;
            upd = pack(&ck, created + cycle);
        } else {
            i64 lim = remaining < 14 * DAY_MS ? remaining : 14 * DAY_MS;
            upd = pack(&ck, created + (i64)k32_rng_range(r, (u32)(lim > 0 ? lim : 1)));
        }
        it->status_ts = unpack(&ck, upd);       /* when the issue reached its status */
        u32 nc = pick_comment_count(r);
        if (nc > left) nc = left;
        i64 cts = created;
        for (u32 c = 0; c < nc; c++) {
            seed_comment* cm = &comments[nc_total++];
            sb cb = { cm->body, 0, SEED_COMMENT_MAX };
            make_comment(&cb, r);
            cm->len = cb.len;
            i64 gap = remaining > 0 ? (i64)k32_rng_range(r, (u32)((remaining / (i64)(nc + 1)) > 0x7fffffff ? 0x7fffffff : (remaining / (i64)(nc + 1)))) : 0;
            cts += gap + 1;
            if (cts > now - 1000) cts = now - 1000;
            cm->issue = i + 1;
            cm->author = (u8)(1 + k32_rng_range(r, SEED_USERS));
            u32 p = pack(&ck, cts);
            cm->ts = unpack(&ck, p);
            if (p > upd) upd = p;
            left--;
        }
        if (upd > pack(&ck, now)) upd = pack(&ck, now);
        it->updated = unpack(&ck, upd);
    }
    /* leftover comments over random issues */
    u32 guard = 0;
    while (left > 0 && guard++ < n_comments + 16) {
        u32 id = 1 + k32_rng_range(r, n_issues);
        seed_issue* it = &issues[id - 1];
        seed_comment* cm = &comments[nc_total];
        sb cb = { cm->body, 0, SEED_COMMENT_MAX };
        make_comment(&cb, r);
        cm->len = cb.len;
        i64 cts = it->updated + 1 + (i64)k32_rng_range(r, 3600000u);
        if (cts > now - 1000) cts = now - 1000;
        cm->issue = id;
        cm->author = (u8)(1 + k32_rng_range(r, SEED_USERS));
        u32 p = pack(&ck, cts);
        cm->ts = unpack(&ck, p);
        if (cm->ts > it->updated) it->updated = cm->ts;
        nc_total++;
        left--;
    }
    *comment_count = nc_total;
    return 0;
}
