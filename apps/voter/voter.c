/* voter: votes on the lanes of apps/lockstep, checkpoint by checkpoint.
 *
 * Each lane posts the bits of its state every CHECK_EVERY steps. At each
 * checkpoint step the voter compares the lanes in the vote bit for bit. A
 * group of identical states that holds more than half of them, and at least
 * two, is the majority: its state is the voted state, and a lane outside it
 * is outvoted from that checkpoint on. Two lanes that differ, or three that
 * all differ, have no majority: the difference is detected and no lane is
 * outvoted (split). The voter answers each report with the target step, so
 * the lanes advance in lockstep: the target grows at RATE steps a second and
 * stays within LEAD steps of the slowest lane in the vote.
 *
 * A lane that has not reported for LOST_MS leaves the vote and the others go
 * on. One that comes back, or a new incarnation of one (restarted, or
 * restored from a copy), is compared with the voted history as its
 * checkpoints arrive, and is in the vote again once it reaches the voted
 * step. A repair copies a lane of the majority into an outvoted or lagging
 * lane with a Copy checkpoint (docs/migration.md#copies), which the voter
 * creates through the API server; the copy restores on the lane's node and
 * goes on from the healthy lane's state.
 *
 *   POST /report            a lane's report (apps/lockstep/lockstep.c)
 *   GET  /state             the vote as JSON
 *   GET  /lane?name=<lane>  the lane's status as one word: agree, outvoted,
 *                           joining, behind, split, lost or absent
 *   GET  /fault?lane=<lane>[&bit=<0..255>]
 *                           fault injection: the lane flips the bit of its
 *                           state when it next reports
 *   GET  /repair?lane=<lane>[&from=<lane>][&to=<node>]
 *                           copies a lane of the majority (or `from`) into
 *                           the lane, on its node or on `to`; without a lane,
 *                           every outvoted or lagging lane
 *   GET  /native.js         the recorded native runs (native_runs.h)
 *   GET  / (any other)      the page
 *
 * On a node with a screen it draws the pendulum of the voted state with each
 * lane's arms over it in the lane's colour, and a row per lane. On a node
 * with a keyboard, a lane's name (or 1..4) injects a fault into that lane,
 * r repairs every outvoted or lagging lane, and Esc counts a request to give
 * the screen back ("leave" in /state), for whatever runs the voter to act on.
 *
 * Environment:
 *   LANES          the lanes, comma-separated (default a,b,c); others that
 *                  report join while there is room for four
 *   CHECK_EVERY    the lanes' CHECK_EVERY (default 100)
 *   RATE           target steps a second, 1..100000 (default 250)
 *   LEAD           steps the target runs ahead of the slowest lane in the
 *                  vote (default 4 x CHECK_EVERY)
 *   LOST_MS        silence after which a lane leaves the vote (default 8000)
 *   START_MS       wait for every lane in LANES before the first vote, at
 *                  most this long (default 10000)
 *   AUTO_REPAIR_S  repair a lane that is outvoted or lagging for this long,
 *                  0 for never (default 0)
 *   FAULT_BIT      the bit /fault and the keys flip without ?bit= (default 40)
 *   API_URL        the API server (default http://$KUBERNETES_SERVICE_HOST:
 *                  $KUBERNETES_SERVICE_PORT); API_TOKEN, a bearer token for it
 *   POD_NAMESPACE  the namespace of the lanes and Checkpoints (default default)
 *   SCREEN, KEYS   off: no drawing, no keys */
#include "../../include/runtime/k32_app_abi.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#include "../lockstep/pendulum.h"
#pragma clang diagnostic pop
#include "native_runs.h"
#include "build/page.h"

#define NOINLINE __attribute__((noinline))
#define MAX_LANES 4
#define RING 64          /* checkpoints kept per lane */
#define HIST 128         /* voted checkpoints kept */
#define LOGS 8
#define LOG_CAP 160
#define FB_W 320
#define FB_H 240
#define NONE 0xFFFFFFFFu
#define REPAIR_DEADLINE_MS 90000

enum { S_ABSENT, S_JOINING, S_AGREE, S_OUT, S_SPLIT, S_BEHIND, S_LOST };
static const char* const words[] = {"absent", "joining", "agree", "outvoted", "split", "behind", "lost"};

typedef struct {
    char name[16], pod[64], node[48], arch[16], os[16];
    uint32_t name_len, pod_len, node_len, arch_len, os_len, arch_retry;
    uint32_t seen, inc, from, step, latest, first, has_latest, us, last_ms;
    uint32_t status;                 /* S_JOINING, S_AGREE, S_OUT or S_SPLIT */
    uint32_t shown, shown_since;     /* status_of() and since when */
    uint32_t out_step;
    uint32_t fault_seen, fault_step, fault_bit, fault_at;
    uint32_t det, det_injected, det_fault_step, det_bit, det_step, det_ms, det_from;
    uint32_t pending;                /* a bit to flip at the next report, or NONE */
    uint32_t repairing, repair_start, repair_inc, repair_retry, repairs, repair_ms, repair_from;
    uint32_t ring_step[RING];        /* step + 1, 0 for an empty slot */
    uint64_t ring_bits[RING][4];
} lane_t;

static lane_t lanes[MAX_LANES];
static uint32_t n_lanes, n_expected;
static uint32_t now;
static uint32_t every, rate, lead, lost_ms, start_ms, auto_ms, fault_bit_default;
static uint32_t started, any_seen, first_seen, voted_any, voted_step, vote_ms, target, target_ms, acc;
static uint32_t votes, unanimous, last_maj, last_voters, split_logged, last_det = NONE;
static uint32_t leaves;                  /* Esc presses: requests to give the screen back */
static uint32_t hist_step[HIST], hist_ok[HIST];
static uint64_t hist_bits[HIST][4];
static char api[128], token[160], ns[64];
static uint32_t api_len, token_len, ns_len;
static char req[4096];
static char resp[6144];
static char out_buf[2048];
static char logs[LOGS][LOG_CAP];
static uint32_t log_len[LOGS], n_logs;

/* ---- text ------------------------------------------------------------------ */
typedef struct {
    char* p;
    uint32_t n, cap, stream;   /* stream: a full buffer goes out with k32_resp_write */
} buf_t;

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static int eq(const char* a, uint32_t an, const char* b, uint32_t bn) {
    if (an != bn) return 0;
    for (uint32_t i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int eqs(const char* a, uint32_t an, const char* b) { return eq(a, an, b, slen(b)); }

static void b_put(buf_t* b, const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (b->n == b->cap) {
            if (!b->stream) return;
            k32_resp_write(b->p, b->n);
            b->n = 0;
        }
        b->p[b->n++] = s[i];
    }
}
static void b_s(buf_t* b, const char* s) { b_put(b, s, slen(s)); }
static void b_c(buf_t* b, char c) { b_put(b, &c, 1); }
static void b_u(buf_t* b, uint32_t v) {
    char t[10];
    uint32_t k = 0;
    do { t[9 - k++] = (char)('0' + v % 10); v /= 10; } while (v);
    b_put(b, t + 10 - k, k);
}
static void b_hex(buf_t* b, uint64_t v, int digits) {
    static const char hex[] = "0123456789abcdef";
    for (int s = 4 * (digits - 1); s >= 0; s -= 4) b_c(b, hex[(v >> s) & 15]);
}
/* ms as seconds with one decimal: "1.9 s" */
static void b_sec(buf_t* b, uint32_t ms) {
    b_u(b, ms / 1000);
    b_c(b, '.');
    b_u(b, ms % 1000 / 100);
    b_s(b, " s");
}
/* A JSON string of plain text */
static void b_js(buf_t* b, const char* s, uint32_t n) {
    b_c(b, '"');
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] == '"' || s[i] == '\\') b_c(b, '\\');
        if ((unsigned char)s[i] >= 32) b_c(b, s[i]);
    }
    b_c(b, '"');
}

static uint32_t env(const char* key, char* out, uint32_t cap) {
    const int32_t n = k32_env_get(key, slen(key), out, cap);
    return n <= 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap;
}

static uint32_t parse_u(const char* s, uint32_t n, uint32_t dflt) {
    uint32_t v = 0;
    if (!n || n > 10) return dflt;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return dflt;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    return v;
}

static int parse_hex(const char* s, uint32_t n, uint64_t* out) {
    uint64_t v = 0;
    if (!n || n > 16) return 0;
    for (uint32_t i = 0; i < n; i++) {
        const char c = s[i];
        const uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) : 16;
        if (d > 15) return 0;
        v = v << 4 | d;
    }
    *out = v;
    return 1;
}

static uint32_t env_u(const char* key, uint32_t dflt, uint32_t lo, uint32_t hi) {
    char b[12];
    const uint32_t v = parse_u(b, env(key, b, sizeof b), dflt);
    return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t copy(char* dst, uint32_t cap, const char* src, uint32_t n) {
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    return n;
}

/* The value of `key` in a query string, or 0 */
static const char* qparam(const char* q, uint32_t qn, const char* key, uint32_t* len) {
    const uint32_t kn = slen(key);
    for (uint32_t i = 0; i + kn < qn; i++) {
        if ((i && q[i - 1] != '&') || q[i + kn] != '=') continue;
        uint32_t k = 0;
        while (k < kn && q[i + k] == key[k]) k++;
        if (k < kn) continue;
        uint32_t e = i + kn + 1;
        while (e < qn && q[e] != '&') e++;
        *len = e - i - kn - 1;
        return q + i + kn + 1;
    }
    *len = 0;
    return 0;
}

/* The string value after `"key":"` in a JSON text, copied to out */
static uint32_t json_str(const char* j, uint32_t n, const char* key, char* out, uint32_t cap) {
    const uint32_t kn = slen(key);
    for (uint32_t i = 0; i + kn + 3 < n; i++) {
        if (j[i] != '"' || j[i + kn + 1] != '"' || j[i + kn + 2] != ':' || j[i + kn + 3] != '"') continue;
        uint32_t k = 0;
        while (k < kn && j[i + 1 + k] == key[k]) k++;
        if (k < kn) continue;
        uint32_t s = i + kn + 4, e = s;
        while (e < n && j[e] != '"') e++;
        return copy(out, cap, j + s, e - s);
    }
    return 0;
}

/* ---- log ----------------------------------------------------------------------- */
static char lbuf[LOG_CAP + 8];
static buf_t lb = {lbuf, 0, sizeof lbuf, 0};

static void log_begin(void) {
    lb.n = 0;
    b_s(&lb, "voter: ");
}
static void log_end(void) {
    k32_log(lb.p, lb.n);
    const uint32_t i = n_logs++ % LOGS;
    log_len[i] = copy(logs[i], LOG_CAP, lb.p + 7, lb.n - 7);
}

/* ---- lanes ------------------------------------------------------------------------ */
static lane_t* lane_named(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n_lanes; i++)
        if (eq(lanes[i].name, lanes[i].name_len, s, n)) return &lanes[i];
    return 0;
}

static lane_t* lane_add(const char* s, uint32_t n) {
    if (n_lanes == MAX_LANES || !n || n > sizeof lanes[0].name - 1) return 0;
    lane_t* L = &lanes[n_lanes++];
    L->name_len = copy(L->name, sizeof L->name - 1, s, n);
    L->pending = NONE;
    L->repair_from = NONE;
    return L;
}

static uint32_t slot(uint32_t s, uint32_t size) { return s / every % size; }

static const uint64_t* lane_bits(const lane_t* L, uint32_t s) {
    const uint32_t i = slot(s, RING);
    return L->ring_step[i] == s + 1 ? L->ring_bits[i] : 0;
}

static const uint64_t* voted_bits(uint32_t s) {
    const uint32_t i = slot(s, HIST);
    return hist_step[i] == s + 1 && hist_ok[i] ? hist_bits[i] : 0;
}

static int same_bits(const uint64_t* a, const uint64_t* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* FNV-1a of the state's 32 bytes, little-endian: pendulum_hash of the lane */
static uint32_t hash_bits(const uint64_t* b) {
    uint32_t h = 0x811C9DC5u;
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 8; k++) h = (h ^ (uint32_t)(uint8_t)(b[i] >> (8 * k))) * 0x01000193u;
    return h;
}

static int is_lost(const lane_t* L) { return L->seen && now - L->last_ms > lost_ms; }

/* Holds the voted step: not outvoted, not lost, and its checkpoints reach
 * the last voted step */
static int in_step(const lane_t* L) {
    return L->seen && !is_lost(L) && L->status != S_OUT && L->has_latest && (!voted_any || L->latest >= voted_step);
}

static uint32_t status_of(const lane_t* L) {
    if (!L->seen) return S_ABSENT;
    if (is_lost(L)) return S_LOST;
    if (L->status == S_OUT) return S_OUT;
    if (voted_any && (!L->has_latest || L->latest < voted_step)) return S_BEHIND;
    return L->status;
}

static void b_lane(buf_t* b, const lane_t* L) {
    b_s(b, "lane ");
    b_put(b, L->name, L->name_len);
    if (L->node_len) {
        b_s(b, " on node ");
        b_put(b, L->node, L->node_len);
        if (L->arch_len) {
            b_s(b, " (");
            b_put(b, L->arch, L->arch_len);
            b_c(b, ')');
        }
    }
}

static void set_agree(lane_t* L) {
    if (L->status != S_AGREE && L->repairing && L->inc != L->repair_inc) {
        L->repairing = 0;
        L->repairs++;
        L->repair_ms = now - L->repair_start;
        log_begin();
        b_lane(&lb, L);
        b_s(&lb, " agrees again from step ");
        b_u(&lb, L->from);
        b_s(&lb, ", ");
        b_sec(&lb, L->repair_ms);
        b_s(&lb, " after the repair began");
        log_end();
    }
    L->status = S_AGREE;
}

static NOINLINE void mark_out(lane_t* L, uint32_t s, const uint64_t* mine, const uint64_t* maj) {
    L->status = S_OUT;
    L->out_step = s;
    L->det = 1;
    L->det_step = s;
    L->det_injected = L->fault_seen && L->fault_step <= s;
    L->det_fault_step = L->fault_step;
    L->det_bit = L->fault_bit;
    L->det_ms = L->det_injected ? now - L->fault_at : 0;
    L->det_from = L->from;
    last_det = (uint32_t)(L - lanes);
    log_begin();
    b_lane(&lb, L);
    b_s(&lb, " outvoted at step ");
    b_u(&lb, s);
    b_s(&lb, ": hash ");
    b_hex(&lb, hash_bits(mine), 8);
    if (maj) {
        b_s(&lb, ", majority ");
        b_hex(&lb, hash_bits(maj), 8);
    }
    if (L->det_injected) {
        b_s(&lb, "; bit ");
        b_u(&lb, L->fault_bit);
        b_s(&lb, " flipped at step ");
        b_u(&lb, L->fault_step);
        b_s(&lb, ", detected ");
        b_u(&lb, s - L->fault_step);
        b_s(&lb, " steps and ");
        b_u(&lb, L->det_ms);
        b_s(&lb, " ms later");
    } else {
        b_s(&lb, "; no fault injected, ");
        b_u(&lb, s - L->from);
        b_s(&lb, " steps after it began at step ");
        b_u(&lb, L->from);
    }
    log_end();
    if (L->repairing && L->inc != L->repair_inc) L->repairing = 0;
}

static NOINLINE void ingest(lane_t* L, uint32_t s, const uint64_t* bits) {
    if (s % every) return;
    const uint32_t i = slot(s, RING);
    L->ring_step[i] = s + 1;
    for (int k = 0; k < 4; k++) L->ring_bits[i][k] = bits[k];
    if (!L->has_latest) {
        L->first = L->latest = s;
        L->has_latest = 1;
    }
    if (s > L->latest) L->latest = s;
    if (s < L->first) L->first = s;
    /* A checkpoint the vote has passed: compared with the voted history */
    if (L->status != S_OUT && voted_any && s <= voted_step) {
        const uint64_t* v = voted_bits(s);
        if (v && same_bits(v, bits)) set_agree(L);
        else if (v) mark_out(L, s, bits, v);
    }
}

static NOINLINE void vote(void) {
    for (uint32_t guard = 0; guard < 256; guard++) {
        uint32_t next = voted_step + every;
        if (!voted_any) {
            /* The first vote: the latest first checkpoint of the lanes */
            uint32_t n = 0;
            if (!started) return;
            next = 0;
            for (uint32_t i = 0; i < n_lanes; i++) {
                if (!in_step(&lanes[i])) continue;
                if (lanes[i].first > next) next = lanes[i].first;
                n++;
            }
            if (!n) return;
        }
        lane_t* who[MAX_LANES];
        const uint64_t* bits[MAX_LANES];
        uint32_t nv = 0, nc = 0;
        for (uint32_t i = 0; i < n_lanes; i++) {
            lane_t* L = &lanes[i];
            if (!in_step(L) || L->from > next) continue;
            nc++;
            if (L->latest < next) return;   /* waits for it */
            const uint64_t* b = lane_bits(L, next);
            if (b) {
                who[nv] = L;
                bits[nv++] = b;
            }
        }
        if (!nc) return;
        uint32_t grp[MAX_LANES], size[MAX_LANES], head[MAX_LANES], ng = 0, best = 0;
        for (uint32_t i = 0; i < nv; i++) {
            uint32_t j = 0;
            while (j < ng && !same_bits(bits[head[j]], bits[i])) j++;
            if (j == ng) {
                head[ng] = i;
                size[ng++] = 0;
            }
            grp[i] = j;
            size[j]++;
        }
        for (uint32_t j = 1; j < ng; j++)
            if (size[j] > size[best]) best = j;
        const int majority = nv && size[best] >= 2 && 2 * size[best] > nv;
        const uint32_t h = slot(next, HIST);
        hist_step[h] = next + 1;
        hist_ok[h] = (uint32_t)majority;
        votes++;
        last_voters = nv;
        last_maj = majority ? size[best] : 0;
        if (majority) {
            for (int k = 0; k < 4; k++) hist_bits[h][k] = bits[head[best]][k];
            if (ng == 1) unanimous++;
            for (uint32_t i = 0; i < nv; i++) {
                if (grp[i] == best) set_agree(who[i]);
                else mark_out(who[i], next, bits[i], bits[head[best]]);
            }
            split_logged = 0;
        } else if (nv >= 2) {
            for (uint32_t i = 0; i < nv; i++) who[i]->status = S_SPLIT;
            if (!split_logged) {
                split_logged = 1;
                log_begin();
                b_s(&lb, "no majority at step ");
                b_u(&lb, next);
                for (uint32_t i = 0; i < nv; i++) {
                    b_s(&lb, i ? ", lane " : ": lane ");
                    b_put(&lb, who[i]->name, who[i]->name_len);
                    b_c(&lb, ' ');
                    b_hex(&lb, hash_bits(bits[i]), 8);
                }
                log_end();
            }
        }
        voted_step = next;
        voted_any = 1;
        vote_ms = now;
    }
}

static void maybe_start(void) {
    if (started) return;
    uint32_t seen = 0;
    for (uint32_t i = 0; i < n_lanes; i++) seen += lanes[i].seen;
    if (!seen || (seen < n_expected && now - first_seen < start_ms)) return;
    started = 1;
    target_ms = now;
    acc = 0;
    log_begin();
    b_s(&lb, "voting starts with ");
    b_u(&lb, seen);
    b_s(&lb, " lanes at step ");
    b_u(&lb, target);
    log_end();
}

/* The target grows at RATE steps a second, at most LEAD steps past the
 * slowest lane in the vote, and holds while no lane is in it */
static void advance_target(void) {
    uint32_t dt = now - target_ms;
    target_ms = now;
    if (!started) return;
    if (dt > 10000) dt = 10000;
    acc += dt * rate;
    const uint32_t adv = acc / 1000;
    acc %= 1000;
    uint32_t slow = NONE;
    for (uint32_t i = 0; i < n_lanes; i++)
        if (in_step(&lanes[i]) && lanes[i].step < slow) slow = lanes[i].step;
    if (slow == NONE) return;
    uint32_t t = target + adv;
    if (t > slow + lead) {
        t = slow + lead;
        acc = 0;
    }
    if (t > target) target = t;
}

/* ---- the API server ---------------------------------------------------------------- */
static char url[256];
static char hdr[256];
static uint32_t url_len, hdr_len;

static void url_begin(const char* path) {
    url_len = copy(url, sizeof url, api, api_len);
    url_len += copy(url + url_len, sizeof url - url_len, path, slen(path));
}
static void url_put(const char* s, uint32_t n) { url_len += copy(url + url_len, sizeof url - url_len, s, n); }

static void headers(int json) {
    hdr_len = 0;
    if (json) hdr_len = copy(hdr, sizeof hdr, "Content-Type: application/json\r\n", 32);
    if (token_len) {
        hdr_len += copy(hdr + hdr_len, sizeof hdr - hdr_len, "Authorization: Bearer ", 22);
        hdr_len += copy(hdr + hdr_len, sizeof hdr - hdr_len - 2, token, token_len);
        hdr_len += copy(hdr + hdr_len, sizeof hdr - hdr_len, "\r\n", 2);
    }
}

/* The node's kubernetes.io/arch and kubernetes.io/os labels */
static NOINLINE void lookup_arch(lane_t* L) {
    url_begin("/api/v1/nodes/");
    url_put(L->node, L->node_len);
    headers(0);
    uint32_t got = 0;
    const int32_t rc = k32_http_call("GET", 3, url, url_len, hdr, hdr_len, 0, 0, (uint8_t*)resp, sizeof resp, &got, 2000);
    L->arch_retry = now + 5000;
    if (rc != 200 && !(rc == K32_ERR_TOOBIG && k32_http_status() == 200)) return;
    L->arch_len = json_str(resp, got, "kubernetes.io/arch", L->arch, sizeof L->arch);
    L->os_len = json_str(resp, got, "kubernetes.io/os", L->os, sizeof L->os);
    if (!L->arch_len) L->arch_retry = now + 30000;
}

/* A lane of the majority that can be copied into L */
static lane_t* healthy(const lane_t* L) {
    lane_t* best = 0;
    for (uint32_t i = 0; i < n_lanes; i++) {
        lane_t* c = &lanes[i];
        if (c == L || !c->pod_len || status_of(c) != S_AGREE) continue;
        if (!best || c->latest > best->latest) best = c;
    }
    return best;
}

/* A Copy checkpoint of `src` into L, through the API server. The HTTP status
 * of the create, or a negative error; `why` receives the reason of a failure */
static NOINLINE int32_t repair(lane_t* L, lane_t* src, const char* to, uint32_t to_len, buf_t* why) {
    if (!api_len) {
        b_s(why, "no API server: set API_URL");
        return 503;
    }
    if (!L->pod_len) {
        b_s(why, "the lane has not reported its pod");
        return 409;
    }
    if (!src) src = healthy(L);
    if (!src || src == L || !src->pod_len) {
        b_s(why, "no other lane agrees with a majority");
        return 409;
    }
    char name[64], body[512];
    buf_t nb = {name, 0, sizeof name, 0}, bb = {body, 0, sizeof body, 0};
    b_put(&nb, L->pod, L->pod_len < 50 ? L->pod_len : 50);
    b_c(&nb, '-');
    b_hex(&nb, k32_random(), 5);
    b_s(&bb, "{\"apiVersion\":\"kube32.io/v1\",\"kind\":\"Checkpoint\",\"metadata\":{\"name\":\"");
    b_put(&bb, name, nb.n);
    b_s(&bb, "\",\"labels\":{\"app\":\"voter\"}},\"spec\":{\"podName\":\"");
    b_put(&bb, src->pod, src->pod_len);
    b_s(&bb, "\",\"action\":\"Copy\",\"target\":{\"podName\":\"");
    b_put(&bb, L->pod, L->pod_len);
    if (to_len) {
        b_s(&bb, "\",\"nodeName\":\"");
        b_put(&bb, to, to_len);
    }
    b_s(&bb, "\"}}}");
    url_begin("/apis/kube32.io/v1/namespaces/");
    url_put(ns, ns_len);
    url_put("/checkpoints", 12);
    headers(1);
    uint32_t got = 0;
    const int32_t rc = k32_http_call("POST", 4, url, url_len, hdr, hdr_len, (const uint8_t*)body, bb.n, (uint8_t*)resp, sizeof resp, &got, 5000);
    log_begin();
    if (rc == 201 || rc == 200) {
        L->repairing = 1;
        L->repair_start = now;
        L->repair_inc = L->inc;
        L->repair_from = (uint32_t)(src - lanes);
        b_s(&lb, "repair: copying ");
        b_lane(&lb, src);
        b_s(&lb, " into ");
        b_lane(&lb, L);
        b_s(&lb, " with checkpoint ");
        b_put(&lb, name, nb.n);
        b_put(why, name, nb.n);
    } else {
        char msg[96];
        const uint32_t mn = rc > 0 ? json_str(resp, got, "message", msg, sizeof msg) : 0;
        L->repair_retry = now + 15000;
        b_s(&lb, "repair of ");
        b_lane(&lb, L);
        b_s(&lb, ": the API server answered ");
        if (rc < 0) b_c(&lb, '-');
        b_u(&lb, rc < 0 ? (uint32_t)-rc : (uint32_t)rc);
        if (mn) {
            b_s(&lb, ": ");
            b_put(&lb, msg, mn);
        }
        b_put(why, lb.p + 7, lb.n - 7);
    }
    log_end();
    return rc;
}

static uint32_t repair_all(void) {
    uint32_t n = 0;
    char w[160];
    for (uint32_t i = 0; i < n_lanes; i++) {
        lane_t* L = &lanes[i];
        const uint32_t st = status_of(L);
        if ((st != S_OUT && st != S_BEHIND) || L->repairing) continue;
        buf_t wb = {w, 0, sizeof w, 0};
        const int32_t rc = repair(L, 0, 0, 0, &wb);
        n += rc == 201 || rc == 200;
    }
    return n;
}

static void queue_fault(lane_t* L, uint32_t bit) {
    L->pending = bit;
    log_begin();
    b_s(&lb, "fault injection: bit ");
    b_u(&lb, bit);
    b_s(&lb, " of ");
    b_lane(&lb, L);
    b_s(&lb, " at its next report");
    log_end();
}

/* ---- reports ------------------------------------------------------------------------ */
static void new_incarnation(lane_t* L, uint32_t inc, uint32_t from) {
    if (L->seen) {
        log_begin();
        b_lane(&lb, L);
        b_s(&lb, ": incarnation ");
        b_hex(&lb, inc, 8);
        b_s(&lb, " from step ");
        b_u(&lb, from);
        log_end();
    }
    L->inc = inc;
    L->from = from;
    L->has_latest = L->latest = L->first = 0;
    L->status = S_JOINING;
    L->fault_seen = 0;
    for (uint32_t i = 0; i < RING; i++) L->ring_step[i] = 0;
}

/* The next line of [*at, end), split at its first space */
static int next_line(const char* b, uint32_t end, uint32_t* at, const char** key, uint32_t* kn, const char** val, uint32_t* vn) {
    if (*at >= end) return 0;
    uint32_t s = *at, e = s;
    while (e < end && b[e] != '\n') e++;
    *at = e + 1;
    uint32_t sp = s;
    while (sp < e && b[sp] != ' ') sp++;
    *key = b + s;
    *kn = sp - s;
    *val = sp < e ? b + sp + 1 : b + e;
    *vn = sp < e ? e - sp - 1 : 0;
    return 1;
}

/* The n-th space-separated field of v */
static const char* field(const char* v, uint32_t vn, uint32_t n, uint32_t* len) {
    uint32_t i = 0;
    for (uint32_t f = 0; f <= n; f++) {
        while (i < vn && v[i] == ' ') i++;
        const uint32_t s = i;
        while (i < vn && v[i] != ' ') i++;
        if (f == n) {
            *len = i - s;
            return v + s;
        }
    }
    *len = 0;
    return v;
}

static NOINLINE int32_t on_report(const char* b, uint32_t n) {
    const char *key, *val;
    uint32_t kn, vn, at = 0;
    lane_t* L = 0;
    uint64_t inc = 0;
    uint32_t from = 0, step = 0, us = 0, has_fault = 0, fs = 0, fb = 0, fage = 0;
    const char *pod = 0, *node = 0;
    uint32_t pod_n = 0, node_n = 0;
    while (next_line(b, n, &at, &key, &kn, &val, &vn)) {
        if (eqs(key, kn, "lane")) L = lane_named(val, vn) ? lane_named(val, vn) : lane_add(val, vn);
        else if (eqs(key, kn, "pod")) pod = val, pod_n = vn;
        else if (eqs(key, kn, "node")) node = val, node_n = vn;
        else if (eqs(key, kn, "inc")) parse_hex(val, vn, &inc);
        else if (eqs(key, kn, "from")) from = parse_u(val, vn, 0);
        else if (eqs(key, kn, "step")) step = parse_u(val, vn, 0);
        else if (eqs(key, kn, "us")) us = parse_u(val, vn, 0);
        else if (eqs(key, kn, "fault")) {
            uint32_t l;
            const char* f = field(val, vn, 0, &l);
            fs = parse_u(f, l, 0);
            f = field(val, vn, 1, &l);
            fb = parse_u(f, l, 0);
            f = field(val, vn, 2, &l);
            fage = parse_u(f, l, 0);
            has_fault = 1;
        }
    }
    if (!L) return 400;
    /* A pod that started again or took a copy: another incarnation, or another first step */
    if (!L->seen || (uint32_t)inc != L->inc || from != L->from) new_incarnation(L, (uint32_t)inc, from);
    if (!any_seen) {
        any_seen = 1;
        first_seen = now;
    }
    L->seen = 1;
    L->last_ms = now;
    if (pod) L->pod_len = copy(L->pod, sizeof L->pod, pod, pod_n);
    if (node && !eq(L->node, L->node_len, node, node_n)) {
        L->node_len = copy(L->node, sizeof L->node, node, node_n);
        L->arch_len = L->os_len = L->arch_retry = 0;
    }
    L->step = step;
    L->us = us;
    if (has_fault && (!L->fault_seen || fs != L->fault_step || fb != L->fault_bit)) {
        L->fault_seen = 1;
        L->fault_step = fs;
        L->fault_bit = fb;
        L->fault_at = now - fage;
    }
    at = 0;
    while (next_line(b, n, &at, &key, &kn, &val, &vn)) {
        if (!eqs(key, kn, "c")) continue;
        uint64_t bits[4];
        uint32_t l, ok = 1;
        const char* f = field(val, vn, 0, &l);
        const uint32_t s = parse_u(f, l, NONE);
        for (uint32_t i = 0; i < 4; i++) {
            f = field(val, vn, i + 1, &l);
            ok &= (uint32_t)parse_hex(f, l, &bits[i]);
        }
        if (ok && s != NONE) ingest(L, s, bits);
    }
    if (!started && step > target) target = step;
    maybe_start();
    vote();
    advance_target();
    char a[64];
    buf_t ab = {a, 0, sizeof a, 0};
    b_s(&ab, "target ");
    b_u(&ab, target);
    b_c(&ab, '\n');
    if (L->has_latest) {
        b_s(&ab, "ack ");
        b_u(&ab, L->latest);
        b_c(&ab, '\n');
    }
    if (L->pending != NONE) {
        b_s(&ab, "fault ");
        b_u(&ab, L->pending);
        b_c(&ab, '\n');
        L->pending = NONE;
    }
    k32_resp_begin(200, "text/plain", 10);
    k32_resp_write(a, ab.n);
    k32_resp_end();
    return 200;
}

/* ---- the screen --------------------------------------------------------------------- */
/* The boards' font: 5x7 glyphs of ASCII 32..126 in 6x8 cells, a byte a column
 * (include/hal/display_font.hpp) */
static const uint8_t font[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00}, {0x14, 0x7F, 0x14, 0x7F, 0x14},
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62}, {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00},
    {0x00, 0x1C, 0x22, 0x41, 0x00}, {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00}, {0x20, 0x10, 0x08, 0x04, 0x02},
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31},
    {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00}, {0x00, 0x56, 0x36, 0x00, 0x00},
    {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14}, {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06},
    {0x32, 0x49, 0x79, 0x41, 0x3E}, {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01}, {0x3E, 0x41, 0x49, 0x49, 0x7A},
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41},
    {0x7F, 0x40, 0x40, 0x40, 0x40}, {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31},
    {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x7F, 0x20, 0x18, 0x20, 0x7F},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04}, {0x40, 0x40, 0x40, 0x40, 0x40},
    {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78}, {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20},
    {0x38, 0x44, 0x44, 0x48, 0x7F}, {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00}, {0x7F, 0x10, 0x28, 0x44, 0x00},
    {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78}, {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38},
    {0x7C, 0x14, 0x14, 0x14, 0x08}, {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C}, {0x3C, 0x40, 0x30, 0x40, 0x3C},
    {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C}, {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00},
    {0x00, 0x00, 0x7F, 0x00, 0x00}, {0x00, 0x41, 0x36, 0x08, 0x00}, {0x08, 0x08, 0x2A, 0x1C, 0x08}};

enum { C_BG, C_FG, C_TRAIL, C_TEXT, C_DIM, C_GREEN, C_RED, C_AMBER, C_LANE };
static const uint32_t colors[] = {0x0b0d10, 0xf2f4f6, 0x2a3038, 0xb8c0c8, 0x6a727c, 0x46e07a, 0xff4d4d, 0xffa020,
                                  0x33d6ff, 0xff5cd6, 0xffe14d, 0xb5e550};
static uint8_t fb[FB_W * FB_H];
static uint16_t pal[256];
static int32_t SW, SH, screen, keys;
static uint32_t last_draw;

static void px(int32_t x, int32_t y, uint8_t c) {
    if ((uint32_t)x < (uint32_t)SW && (uint32_t)y < (uint32_t)SH) fb[y * SW + x] = c;
}

static void dot(int32_t x, int32_t y, int32_t r, uint8_t c) {
    for (int32_t dy = -r; dy <= r; dy++)
        for (int32_t dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r) px(x + dx, y + dy, c);
}

static NOINLINE void line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t r, uint8_t c) {
    const int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    const int32_t sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int32_t e = dx + dy;
    for (int32_t i = 0; i < 2048; i++) {
        if (r) dot(x0, y0, r, c);
        else px(x0, y0, c);
        if (x0 == x1 && y0 == y1) return;
        const int32_t e2 = 2 * e;
        if (e2 >= dy) e += dy, x0 += sx;
        if (e2 <= dx) e += dx, y0 += sy;
    }
}

static NOINLINE int32_t text(int32_t x, int32_t y, const char* s, uint32_t n, uint8_t c, int32_t sc) {
    for (uint32_t i = 0; i < n; i++, x += 6 * sc) {
        const char ch = s[i];
        const uint8_t* g = font[(ch < 32 || ch > 126 ? '?' : ch) - 32];
        for (int32_t col = 0; col < 5; col++)
            for (int32_t row = 0; row < 7; row++)
                if (g[col] >> row & 1)
                    for (int32_t k = 0; k < sc * sc; k++) px(x + col * sc + k % sc, y + row * sc + k / sc, c);
    }
    return x;
}

static int32_t text_b(int32_t x, int32_t y, buf_t* b, uint8_t c) {
    x = text(x, y, b->p, b->n, c, 1);
    b->n = 0;
    return x;
}

static double f64(uint64_t u) {
    union { uint64_t u; double d; } v;
    v.u = u;
    return v.d;
}

/* An angle to draw: finite, within a turn of zero */
static int angle(double a, double* out) {
    if (!(a > -1e9 && a < 1e9)) return 0;
    const int32_t k = (int32_t)(a / 6.283185307179586);
    *out = a - k * 6.283185307179586;
    return 1;
}

static int32_t to_px(double v) { return v > -1e5 && v < 1e5 ? (int32_t)(v + 100000.5) - 100000 : -1; }

/* The lower bob's position, and the arms drawn with radius r when c is set */
static NOINLINE int arms(double t1, double t2, int32_t cx, int32_t cy, double len, int32_t r, int32_t c, int32_t* ox, int32_t* oy) {
    double a, b;
    if (!angle(t1, &a) || !angle(t2, &b)) return 0;
    const double x1 = cx + len * pendulum_sin(a), y1 = cy + len * pendulum_cos(a);
    const double x2 = x1 + len * pendulum_sin(b), y2 = y1 + len * pendulum_cos(b);
    *ox = to_px(x2);
    *oy = to_px(y2);
    if (c < 0) return 1;
    line(cx, cy, to_px(x1), to_px(y1), r, (uint8_t)c);
    line(to_px(x1), to_px(y1), *ox, *oy, r, (uint8_t)c);
    dot(to_px(x1), to_px(y1), r + 1, (uint8_t)c);
    dot(*ox, *oy, r + 2, (uint8_t)c);
    return 1;
}

/* The angles of `bits` at s0 and s1, interpolated by k */
static int lerp(const uint64_t* b0, const uint64_t* b1, double k, double* t1, double* t2) {
    if (!b1) return 0;
    *t1 = f64(b1[0]);
    *t2 = f64(b1[1]);
    if (b0) {
        const double a1 = f64(b0[0]), a2 = f64(b0[1]);
        *t1 = a1 + (*t1 - a1) * k;
        *t2 = a2 + (*t2 - a2) * k;
    }
    return 1;
}

static uint8_t status_color(uint32_t st) {
    return st == S_AGREE ? C_GREEN : st == S_OUT ? C_RED : st == S_LOST || st == S_ABSENT ? C_DIM : C_AMBER;
}

static NOINLINE void draw(void) {
    __builtin_memset(fb, C_BG, (uint32_t)(SW * SH));
    const int32_t P = SH < SW / 2 ? SH : SW / 2, c = P / 2;
    const double len = (P / 2 - 6) / 2.0;
    /* The display moves from the previous voted step to the last over the
     * time the vote takes to advance a checkpoint */
    const uint32_t s1 = voted_step, s0 = voted_step >= every ? voted_step - every : voted_step;
    double k = rate ? (double)(now - vote_ms) * rate / (1000.0 * every) : 1;
    if (k > 1) k = 1;
    double t1, t2;
    int32_t x, y, px0 = -1, py0 = -1;
    if (voted_any) {
        /* the trail of the lower bob over the voted history */
        for (uint32_t i = HIST - 1; i > 0; i--) {
            if (i * every > s1) continue;
            const uint64_t* b = voted_bits(s1 - i * every);
            if (!b || !arms(f64(b[0]), f64(b[1]), c, c, len, 0, -1, &x, &y)) {
                px0 = -1;
                continue;
            }
            if (px0 >= 0) line(px0, py0, x, y, 0, C_TRAIL);
            px0 = x;
            py0 = y;
        }
        for (uint32_t i = 0; i < n_lanes; i++) {
            const lane_t* L = &lanes[i];
            if (!L->seen || is_lost(L)) continue;
            /* Each lane thinner than the one before: lanes that agree show as one striped arm */
            if (lerp(lane_bits(L, s0), lane_bits(L, s1), k, &t1, &t2))
                arms(t1, t2, c, c, len, (int32_t)(n_lanes - i), (int32_t)(C_LANE + i), &x, &y);
        }
        if (lerp(voted_bits(s0), voted_bits(s1), k, &t1, &t2)) arms(t1, t2, c, c, len, 0, C_FG, &x, &y);
    }
    dot(c, c, 2, C_TEXT);

    char t[48];
    buf_t b = {t, 0, sizeof t, 0};
    const int32_t x0 = P + 6;
    uint32_t in_vote = 0;
    for (uint32_t i = 0; i < n_lanes; i++) {
        const uint32_t st = status_of(&lanes[i]);
        in_vote += st == S_AGREE;
    }
    uint8_t hc = C_DIM;
    if (!voted_any) b_s(&b, started ? "VOTING" : "WAITING");
    else if (!last_maj) b_s(&b, "NO MAJORITY"), hc = C_RED;
    else {
        b_s(&b, "VOTE ");
        b_u(&b, in_vote);
        b_c(&b, '/');
        b_u(&b, n_lanes);
        hc = in_vote == n_lanes ? C_GREEN : C_AMBER;
    }
    text(x0, 2, t, b.n, hc, 2);
    b.n = 0;
    b_s(&b, "step ");
    b_u(&b, voted_step);
    b_s(&b, "  t ");
    b_u(&b, voted_step / 1000);
    b_c(&b, '.');
    b_u(&b, voted_step % 1000 / 100);
    b_c(&b, 's');
    text_b(x0, 20, &b, C_TEXT);
    b_s(&b, "target ");
    b_u(&b, target);
    b_c(&b, ' ');
    b_u(&b, rate);
    b_s(&b, "/s");
    text_b(x0, 29, &b, C_DIM);

    const int32_t foot = SH - 3 * 9 - 1;
    int32_t block = n_lanes ? (foot - 41) / (int32_t)n_lanes : 0;
    if (block > 30) block = 30;
    const int lines = block >= 27 ? 3 : 2;
    for (uint32_t i = 0; i < n_lanes; i++) {
        const lane_t* L = &lanes[i];
        const uint32_t st = status_of(L);
        const int32_t y0 = 41 + (int32_t)i * block;
        for (int32_t r = 0; r < 7; r++)
            for (int32_t q = 0; q < 5; q++) px(x0 + q, y0 + r, (uint8_t)(C_LANE + i));
        b_put(&b, L->name, L->name_len);
        int32_t xx = text_b(x0 + 8, y0, &b, C_FG);
        b_s(&b, L->repairing ? "REPAIRING" : st == S_OUT ? "OUTVOTED" : st == S_AGREE ? "AGREE" : words[st]);
        xx = text_b(xx + 6, y0, &b, L->repairing ? C_AMBER : status_color(st));
        if (L->us && st != S_LOST) {
            b_u(&b, L->us);
            b_s(&b, "us");
            text_b(xx + 6, y0, &b, C_DIM);
        }
        int32_t yy = y0 + 9;
        if (lines == 3) {
            b_put(&b, L->node, L->node_len);
            text_b(x0 + 8, yy, &b, C_TEXT);
            yy += 9;
        }
        b_put(&b, L->arch_len ? L->arch : "?", L->arch_len ? L->arch_len : 1);
        b_c(&b, ' ');
        b_u(&b, L->step);
        const uint64_t* lb_ = voted_any ? lane_bits(L, voted_step) : 0;
        if (lb_) {
            b_c(&b, ' ');
            b_hex(&b, hash_bits(lb_), 8);
        }
        text_b(x0 + 8, yy, &b, lb_ && voted_bits(voted_step) && !same_bits(lb_, voted_bits(voted_step)) ? C_RED : C_TEXT);
    }
    if (last_det != NONE) {
        const lane_t* L = &lanes[last_det];
        b_put(&b, L->name, L->name_len);
        b_s(&b, " out @");
        b_u(&b, L->det_step);
        if (L->det_injected) {
            b_s(&b, " bit ");
            b_u(&b, L->det_bit);
            b_s(&b, " @");
            b_u(&b, L->det_fault_step);
        }
        text_b(x0, foot, &b, C_RED);
        if (L->det_injected) {
            b_s(&b, "latency ");
            b_u(&b, L->det_step - L->det_fault_step);
            b_s(&b, " st ");
            b_u(&b, L->det_ms);
            b_s(&b, " ms");
        } else {
            b_s(&b, "no fault injected");
        }
        text_b(x0, foot + 9, &b, C_TEXT);
    }
    for (uint32_t i = 0; i < n_lanes; i++) {
        const lane_t* L = &lanes[i];
        if (!L->repairs && !L->repairing) continue;
        b_put(&b, L->name, L->name_len);
        if (L->repairing) {
            b_s(&b, " repairing ");
            b_u(&b, (now - L->repair_start) / 1000);
            b_c(&b, 's');
        } else {
            b_s(&b, " repaired in ");
            b_sec(&b, L->repair_ms);
        }
        text_b(x0, foot + 18, &b, C_AMBER);
        break;
    }
    k32_display_blit_pal(0, 0, SW, SH, fb, pal);
}

/* ---- state as JSON ---------------------------------------------------------------- */
static void j_bits(buf_t* b, const uint64_t* v) {
    if (!v) {
        b_s(b, "null");
        return;
    }
    b_c(b, '[');
    for (int i = 0; i < 4; i++) {
        if (i) b_c(b, ',');
        b_c(b, '"');
        b_hex(b, v[i], 16);
        b_c(b, '"');
    }
    b_c(b, ']');
}
static void j_u(buf_t* b, const char* key, uint32_t v) {
    b_s(b, key);
    b_u(b, v);
}

static NOINLINE void send_state(void) {
    buf_t b = {out_buf, 0, sizeof out_buf, 1};
    k32_resp_begin(200, "application/json", 16);
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    const uint64_t* v = voted_any ? voted_bits(voted_step) : 0;
    j_u(&b, "{\"step\":", voted_step);
    b_s(&b, voted_any ? ",\"voted\":true" : ",\"voted\":false");
    b_s(&b, ",\"bits\":");
    j_bits(&b, v);
    b_s(&b, ",\"hash\":\"");
    if (v) b_hex(&b, hash_bits(v), 8);
    j_u(&b, "\",\"target\":", target);
    j_u(&b, ",\"rate\":", rate);
    j_u(&b, ",\"every\":", every);
    j_u(&b, ",\"lead\":", lead);
    b_s(&b, started ? ",\"started\":true" : ",\"started\":false");
    j_u(&b, ",\"votes\":", votes);
    j_u(&b, ",\"unanimous\":", unanimous);
    j_u(&b, ",\"leave\":", leaves);
    j_u(&b, ",\"majority\":", last_maj);
    j_u(&b, ",\"voters\":", last_voters);
    b_s(&b, ",\"lanes\":[");
    for (uint32_t i = 0; i < n_lanes; i++) {
        const lane_t* L = &lanes[i];
        const uint64_t* at = voted_any ? lane_bits(L, voted_step) : 0;
        b_s(&b, i ? ",{\"name\":" : "{\"name\":");
        b_js(&b, L->name, L->name_len);
        b_s(&b, ",\"pod\":");
        b_js(&b, L->pod, L->pod_len);
        b_s(&b, ",\"node\":");
        b_js(&b, L->node, L->node_len);
        b_s(&b, ",\"arch\":");
        b_js(&b, L->arch, L->arch_len);
        b_s(&b, ",\"os\":");
        b_js(&b, L->os, L->os_len);
        b_s(&b, ",\"status\":\"");
        b_s(&b, words[status_of(L)]);
        b_s(&b, "\",\"inc\":\"");
        b_hex(&b, L->inc, 8);
        j_u(&b, "\",\"from\":", L->from);
        j_u(&b, ",\"step\":", L->step);
        j_u(&b, ",\"latest\":", L->latest);
        j_u(&b, ",\"us\":", L->us);
        j_u(&b, ",\"age_ms\":", L->seen ? now - L->last_ms : 0);
        b_s(&b, ",\"at\":");
        j_bits(&b, at);
        b_s(&b, ",\"hash\":\"");
        if (at) b_hex(&b, hash_bits(at), 8);
        b_c(&b, '"');
        if (L->status == S_OUT) j_u(&b, ",\"out_step\":", L->out_step);
        if (L->det) {
            b_s(&b, L->det_injected ? ",\"det\":{\"injected\":true" : ",\"det\":{\"injected\":false");
            j_u(&b, ",\"step\":", L->det_step);
            j_u(&b, ",\"from\":", L->det_from);
            if (L->det_injected) {
                j_u(&b, ",\"fault_step\":", L->det_fault_step);
                j_u(&b, ",\"bit\":", L->det_bit);
                j_u(&b, ",\"steps\":", L->det_step - L->det_fault_step);
                j_u(&b, ",\"ms\":", L->det_ms);
            }
            b_c(&b, '}');
        }
        b_s(&b, L->repairing ? ",\"repairing\":true" : ",\"repairing\":false");
        j_u(&b, ",\"repairs\":", L->repairs);
        j_u(&b, ",\"repair_ms\":", L->repair_ms);
        if (L->repair_from != NONE) {
            b_s(&b, ",\"repair_from\":");
            b_js(&b, lanes[L->repair_from].name, lanes[L->repair_from].name_len);
        }
        b_c(&b, '}');
    }
    b_s(&b, "],\"log\":[");
    const uint32_t first = n_logs > LOGS ? n_logs - LOGS : 0;
    for (uint32_t k = n_logs; k > first; k--) {
        const uint32_t i = (k - 1) % LOGS;
        if (k != n_logs) b_c(&b, ',');
        b_js(&b, logs[i], log_len[i]);
    }
    b_s(&b, "]}");
    k32_resp_write(b.p, b.n);
    k32_resp_end();
}

static void send_json(int32_t status, buf_t* b) {
    k32_resp_begin(status, "application/json", 16);
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    k32_resp_write(b->p, b->n);
    k32_resp_end();
}

/* ---- entry points ------------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    now = (uint32_t)k32_time_ms();
    every = env_u("CHECK_EVERY", 100, 10, 10000);
    rate = env_u("RATE", 250, 1, 100000);
    lead = env_u("LEAD", 4 * every, every, 1000000);
    lost_ms = env_u("LOST_MS", 8000, 1000, 600000);
    start_ms = env_u("START_MS", 10000, 0, 600000);
    auto_ms = env_u("AUTO_REPAIR_S", 0, 0, 3600) * 1000;
    fault_bit_default = env_u("FAULT_BIT", 40, 0, 255);
    char v[64];
    uint32_t n = env("LANES", v, sizeof v);
    if (!n) n = copy(v, sizeof v, "a,b,c", 5);
    for (uint32_t s = 0, e = 0; e <= n; e++) {
        if (e < n && v[e] != ',') continue;
        while (s < e && v[s] == ' ') s++;
        if (e > s && !lane_named(v + s, e - s)) lane_add(v + s, e - s);
        s = e + 1;
    }
    n_expected = n_lanes;
    ns_len = env("POD_NAMESPACE", ns, sizeof ns);
    if (!ns_len) ns_len = copy(ns, sizeof ns, "default", 7);
    token_len = env("API_TOKEN", token, sizeof token);
    api_len = env("API_URL", api, sizeof api);
    if (!api_len) {
        char host[48], port[8];
        const uint32_t hn = env("KUBERNETES_SERVICE_HOST", host, sizeof host), pn = env("KUBERNETES_SERVICE_PORT", port, sizeof port);
        if (hn) {
            api_len = copy(api, sizeof api, "http://", 7);
            api_len += copy(api + api_len, sizeof api - api_len, host, hn);
            if (pn) {
                api_len += copy(api + api_len, sizeof api - api_len, ":", 1);
                api_len += copy(api + api_len, sizeof api - api_len, port, pn);
            }
        }
    }
    while (api_len && api[api_len - 1] == '/') api_len--;
    n = env("SCREEN", v, sizeof v);
    int32_t w = 0, h = 0, fmt = 0;
    screen = !eqs(v, n, "off") && k32_display_info(&w, &h, &fmt) == 0 && w > 0 && h > 0;
    SW = w < FB_W ? w : FB_W;
    SH = h < FB_H ? h : FB_H;
    n = env("KEYS", v, sizeof v);
    keys = !eqs(v, n, "off");
    for (uint32_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        const uint32_t c = colors[i];
        pal[i] = (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
    target_ms = now;
    log_begin();
    b_s(&lb, "lanes ");
    for (uint32_t i = 0; i < n_lanes; i++) {
        if (i) b_c(&lb, ',');
        b_put(&lb, lanes[i].name, lanes[i].name_len);
    }
    j_u(&lb, ", checkpoint every ", every);
    j_u(&lb, " steps, ", rate);
    j_u(&lb, " steps/s, lead ", lead);
    b_s(&lb, api_len ? ", API server " : ", no API server");
    b_put(&lb, api, api_len);
    if (screen) {
        j_u(&lb, ", screen ", (uint32_t)SW);
        j_u(&lb, "x", (uint32_t)SH);
    }
    log_end();
    return 0;
}

static void on_key(int32_t k) {
    if (k == 27) {
        leaves++;
        return;
    }
    if (k == 'r' || k == 'R') {
        repair_all();
        return;
    }
    lane_t* L = k >= '1' && k <= '4' && (uint32_t)(k - '1') < n_lanes ? &lanes[k - '1'] : 0;
    if (!L) {
        const char c = (char)k;
        L = lane_named(&c, 1);
    }
    if (L) queue_fault(L, fault_bit_default);
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime) {
    (void)uptime;
    now = (uint32_t)k32_time_ms();
    for (int i = 0; keys && i < 8; i++) {
        const int32_t k = k32_key_read();
        if (k == K32_ERR_AGAIN) break;
        if (k < 0) {
            keys = 0;
            break;
        }
        on_key(k);
    }
    maybe_start();
    vote();
    advance_target();
    for (uint32_t i = 0; i < n_lanes; i++) {
        lane_t* L = &lanes[i];
        const uint32_t st = status_of(L);
        if (st != L->shown) {
            if (L->seen && (st == S_LOST || L->shown == S_LOST)) {
                log_begin();
                b_lane(&lb, L);
                b_s(&lb, st == S_LOST ? ": no report for " : ": reports again after ");
                b_sec(&lb, now - L->last_ms > lost_ms ? now - L->last_ms : now - L->shown_since);
                log_end();
            }
            L->shown = st;
            L->shown_since = now;
        }
        if (L->repairing && now - L->repair_start > REPAIR_DEADLINE_MS) {
            L->repairing = 0;
            L->repair_retry = now + 15000;
            log_begin();
            b_lane(&lb, L);
            b_s(&lb, " did not agree within 90 s of its repair");
            log_end();
        }
        if (auto_ms && (st == S_OUT || st == S_BEHIND) && !L->repairing && now - L->shown_since >= auto_ms && (int32_t)(now - L->repair_retry) >= 0) {
            char w[160];
            buf_t wb = {w, 0, sizeof w, 0};
            repair(L, 0, 0, 0, &wb);
        }
    }
    /* One node a tick: the architecture of each lane's node */
    for (uint32_t i = 0; api_len && i < n_lanes; i++) {
        lane_t* L = &lanes[i];
        if (L->node_len && !L->arch_len && (int32_t)(now - L->arch_retry) >= 0) {
            lookup_arch(L);
            break;
        }
    }
    if (screen && now - last_draw >= 100) {
        last_draw = now;
        draw();
    }
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    now = (uint32_t)k32_time_ms();
    /* "METHOD SP target LF headers LF LF body" */
    uint32_t i = 0;
    while (i < len && m[i] != ' ') i++;
    const uint32_t path = ++i;
    uint32_t seg = path, end = path;
    for (; end < len && m[end] != '\n' && m[end] != ' ' && m[end] != '?'; end++)
        if (m[end] == '/') seg = end + 1;
    const char* q = m + end;
    uint32_t qn = 0;
    if (end < len && m[end] == '?') {
        q = m + end + 1;
        while (end + 1 + qn < len && q[qn] != '\n' && q[qn] != ' ') qn++;
    }
    const char* last = m + seg;
    const uint32_t last_len = end - seg;

    if (eqs(last, last_len, "report")) {
        uint32_t body = path;
        while (body + 1 < len && !(m[body] == '\n' && m[body + 1] == '\n')) body++;
        body = body + 2 < len ? body + 2 : len;
        return on_report(m + body, len - body);
    }
    if (eqs(last, last_len, "state")) {
        send_state();
        return 200;
    }
    char o[512];
    buf_t b = {o, 0, sizeof o, 0};
    uint32_t ln = 0;
    const char* lane = qparam(q, qn, eqs(last, last_len, "lane") ? "name" : "lane", &ln);
    lane_t* L = lane ? lane_named(lane, ln) : 0;
    if (eqs(last, last_len, "lane")) {
        const char* w = L ? words[status_of(L)] : "absent";
        k32_resp_begin(L ? 200 : 404, "text/plain", 10);
        k32_resp_header("Cache-Control", 13, "no-store", 8);
        k32_resp_write(w, slen(w));
        k32_resp_write("\n", 1);
        k32_resp_end();
        return L ? 200 : 404;
    }
    if (eqs(last, last_len, "fault")) {
        uint32_t bn = 0;
        const char* bit = qparam(q, qn, "bit", &bn);
        const uint32_t v = bit ? parse_u(bit, bn, 256) : fault_bit_default;
        if (!L || v > 255) {
            b_s(&b, L ? "{\"error\":\"bit is 0..255\"}" : "{\"error\":\"no such lane\"}");
            send_json(L ? 400 : 404, &b);
            return L ? 400 : 404;
        }
        queue_fault(L, v);
        b_s(&b, "{\"lane\":");
        b_js(&b, L->name, L->name_len);
        j_u(&b, ",\"bit\":", v);
        b_s(&b, ",\"queued\":true}");
        send_json(202, &b);
        return 202;
    }
    if (eqs(last, last_len, "repair")) {
        if (!lane || !ln) {
            j_u(&b, "{\"started\":", repair_all());
            b_c(&b, '}');
            send_json(200, &b);
            return 200;
        }
        if (!L) {
            b_s(&b, "{\"error\":\"no such lane\"}");
            send_json(404, &b);
            return 404;
        }
        uint32_t fn = 0, tn = 0;
        const char* from = qparam(q, qn, "from", &fn);
        const char* to = qparam(q, qn, "to", &tn);
        lane_t* src = from && fn ? lane_named(from, fn) : 0;
        if (from && fn && !src) {
            b_s(&b, "{\"error\":\"no such lane to copy\"}");
            send_json(404, &b);
            return 404;
        }
        char w[192];
        buf_t wb = {w, 0, sizeof w, 0};
        const int32_t rc = repair(L, src, to, tn, &wb);
        const int ok = rc == 201 || rc == 200;
        b_s(&b, "{\"lane\":");
        b_js(&b, L->name, L->name_len);
        if (ok) {
            b_s(&b, ",\"from\":");
            b_js(&b, lanes[L->repair_from].name, lanes[L->repair_from].name_len);
            b_s(&b, ",\"checkpoint\":");
        } else {
            b_s(&b, ",\"error\":");
        }
        b_js(&b, w, wb.n);
        b_c(&b, '}');
        const int32_t status = ok ? 201 : rc > 0 && rc < 600 ? (rc < 400 ? 502 : rc) : 502;
        send_json(status, &b);
        return status;
    }
    if (eqs(last, last_len, "native.js")) {
        k32_resp_begin(200, "text/javascript", 15);
        k32_resp_header("Cache-Control", 13, "max-age=3600", 12);
        k32_resp_write(native_runs_js, sizeof native_runs_js - 1);
        k32_resp_end();
        return 200;
    }
    /* Any other path is the page, whose links are relative to it as a
     * directory: /voter is /voter/ */
    char base[96];
    uint32_t bn = 0;
    for (uint32_t k = path; k < end && bn < sizeof base - 2; k++)
        if (m[k] != '"' && m[k] != '<' && m[k] != '\\' && m[k] != '&') base[bn++] = m[k];
    if (last_len || !bn) base[bn++] = '/';
    static const char marker[] = "@BASE@";
    uint32_t at = 0;
    while (at + 6 <= sizeof page_html - 1 && !eq(page_html + at, 6, marker, 6)) at++;
    k32_resp_begin(200, "text/html; charset=utf-8", 24);
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    k32_resp_write(page_html, at);
    k32_resp_write(base, bn);
    if (at + 6 <= sizeof page_html - 1) k32_resp_write(page_html + at + 6, sizeof page_html - 1 - at - 6);
    k32_resp_end();
    return 200;
}
