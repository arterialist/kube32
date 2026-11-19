/* clusterview: the cluster on a board's screen. One screen shows
 *
 *   nodes     name, Ready or not, and the hardware each offers (from its
 *             capacity: disp keys spk mic sd i2c ir gpio<N>); a node that is
 *             not Ready turns its row to the accent colour
 *   pods      the pods that POD_SELECTOR selects, the node each is bound to
 *             and its status; a pod that changes nodes keeps an accent row,
 *             with the node it left, for MOVE_MS
 *   readings  a JSON field of each READING_<n> Service's answer, large
 *   messages  the last messages on the topics of kube32.io/event-topics,
 *             with their age
 *
 * It reads the API server with k32_http_call: Table lists (Accept:
 * as=Table;v=v1;g=meta.k8s.io) a few rows a page (limit and continue), so an
 * answer fits a 4 KiB buffer, and one Node a refresh for its capacity. Each
 * call is a k32_tick of its own (k32_tick returns 1 until the refresh is
 * done), so the pod never holds the interpreter for more than one call and
 * runs on a board without PSRAM.
 *
 * Environment:
 *   API_URL        the API server (default http://$KUBERNETES_SERVICE_HOST:$KUBERNETES_SERVICE_PORT)
 *   API_TOKEN      bearer token for it, when it has one
 *   POD_SELECTOR   label selector of the pods to show, e.g. app in (irremote,miniscale)
 *   POD_NAMESPACE  their namespace (default default; all: every namespace)
 *   READING_1..3   "<label> <url> <field> [<unit>]", e.g. "scale http://kitchen-scale/ weight_g g"
 *   TOPICS         the topics it listens to, shown before a message arrives
 *   TITLE          the header's title (default cluster)
 *   REFRESH_MS     time between refreshes, 500..60000 (default 2000)
 *   TIMEOUT_MS     per call, 100..30000 (default 2000)
 *   ACCENT         the accent colour, RRGGBB hex (default ffb000)
 *
 * Needs hardware.kube32.io/tft-display (DISPLAY) and SERVICE_CALL; messages
 * need the topics in kube32.io/event-topics. */
#include "../../include/runtime/k32_app_abi.h"

#define NOINLINE __attribute__((noinline))

#define MAX_NODES 8
#define MAX_PODS 8
#define MAX_READINGS 3
#define MAX_MSGS 4
#define NAME_CAP 32
#define RESP_CAP 4096
#define URL_CAP 384
#define PAGE_ROWS 4             /* Table rows a page: about 600 bytes each */
#define MOVE_MS 8000            /* a pod that changed nodes stays highlighted */
#define TAGS_MS 60000           /* a node's hardware is read again after */
#define MAX_SCALE 3

/* ---- state --------------------------------------------------------------- */
struct node {
    char name[NAME_CAP];
    uint8_t name_len, ready, has_tags;
    char tags[48];
    uint8_t tags_len;
    uint32_t tags_at;
};

struct pod {
    char name[NAME_CAP], node[NAME_CAP], from[NAME_CAP], phase[20];
    uint8_t name_len, node_len, from_len, phase_len;
    uint32_t moved_at;          /* 0: not moved */
};

struct reading {
    char label[16], url[96], field[32], unit[8], value[16];
    uint8_t label_len, url_len, field_len, unit_len, value_len;
    int32_t status;             /* HTTP status or K32_ERR_*; 0 before the first call */
};

struct msg {
    char topic[24], text[60];
    uint8_t topic_len, text_len;
    uint32_t at;
};

static struct node nodes[MAX_NODES], next_nodes[MAX_NODES];
static struct pod pods[MAX_PODS], next_pods[MAX_PODS];
static struct reading readings[MAX_READINGS];
static struct msg msgs[MAX_MSGS];
static uint32_t n_nodes, n_next_nodes, n_pods, n_next_pods, n_readings, n_msgs, total_nodes;

static char api[96], selector[96], ns[48], topics[64], title[24];
static uint32_t api_len, selector_len, ns_len, topics_len, title_len;
static char hdr_table[256], hdr_plain[200];
static uint32_t hdr_table_len, hdr_plain_len;
static uint32_t refresh_ms = 2000, timeout_ms = 2000;
static int32_t api_status;      /* the last API call's status or error */
static int32_t logged_status = 200;

static uint8_t resp[RESP_CAP];
static uint32_t resp_len;
static char url[URL_CAP];
static uint32_t url_len;
static char cont[160];          /* the list's continue token */
static uint32_t cont_len;
static uint32_t page_rows = PAGE_ROWS;
static int truncated;           /* a page did not fit: keep what this refresh did not see */

enum { S_IDLE, S_NODES, S_PODS, S_DETAIL, S_READ };
static int step = S_IDLE, step_arg;
static uint32_t cycle_at;
static int started;

static unsigned char block[320];   /* k32_alloc: an event's topic and payload */

/* ---- text ---------------------------------------------------------------- */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static NOINLINE int same(const char* a, uint32_t an, const char* b, uint32_t bn) {
    if (an != bn) return 0;
    for (uint32_t i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static NOINLINE uint32_t copy(char* dst, uint32_t cap, const char* src, uint32_t n) {
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    return n;
}

static NOINLINE uint32_t put_uint(char* out, uint32_t v) {
    char t[10];
    uint32_t n = 0, k = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) out[k++] = t[--n];
    return k;
}

static NOINLINE uint32_t env(const char* key, char* out, uint32_t cap) {
    int32_t n = k32_env_get(key, slen(key), out, cap);
    return n <= 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap;
}

static NOINLINE uint32_t env_uint(const char* key, uint32_t dflt, uint32_t lo, uint32_t hi) {
    char b[12];
    uint32_t n = env(key, b, sizeof b), v = 0;
    if (!n) return dflt;
    for (uint32_t i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return dflt;
        v = v * 10 + (uint32_t)(b[i] - '0');
    }
    return v < lo ? lo : v > hi ? hi : v;
}

static char line[200];
static uint32_t line_len;
static NOINLINE void lput(const char* s, uint32_t n) { line_len += copy(line + line_len, sizeof line - line_len, s, n); }
static void lputs(const char* s) { lput(s, slen(s)); }
static NOINLINE void lint(int32_t v) {
    if (v < 0) { lputs("-"); v = -v; }
    char t[10];
    lput(t, put_uint(t, (uint32_t)v));
}
static void log_line(void) { k32_log(line, line_len); line_len = 0; }

/* ---- JSON ---------------------------------------------------------------- */
/* After the first `pat` in [p, end), or 0 */
static NOINLINE const char* find(const char* p, const char* end, const char* pat) {
    const uint32_t n = slen(pat);
    for (; p + n <= end; p++) {
        uint32_t i = 0;
        while (i < n && p[i] == pat[i]) i++;
        if (i == n) return p + n;
    }
    return 0;
}

/* The JSON string at p (its opening quote) into out, escapes kept as one
 * character; the position after its closing quote, or 0 when it is cut */
static NOINLINE const char* jstr(const char* p, const char* end, char* out, uint32_t cap, uint8_t* len) {
    uint32_t n = 0;
    if (p >= end || *p != '"') return 0;
    for (p++; p < end; p++) {
        char c = *p;
        if (c == '"') {
            if (len) *len = (uint8_t)n;
            return p + 1;
        }
        if (c == '\\') {
            if (++p >= end) return 0;
            c = *p == 'u' ? '?' : *p;
            if (*p == 'u') p += 4;
        }
        if (out && n < cap) out[n++] = c;
    }
    return 0;
}

/* The cells of a Table row from just after "cells":[ up to `count`: cell i
 * into out[i] when want has bit i. The position after the row's ], or 0 */
static NOINLINE const char* row_cells(const char* p, const char* end, uint32_t want, char (*out)[NAME_CAP], uint8_t* lens) {
    for (uint32_t i = 0; p < end; i++) {
        while (p < end && (*p == ' ' || *p == ',')) p++;
        if (p >= end) return 0;
        if (*p == ']') return p + 1;
        if (*p == '"') {
            p = jstr(p, end, i < 8 && (want >> i & 1) ? out[i] : 0, NAME_CAP, i < 8 ? &lens[i] : 0);
            if (!p) return 0;
        } else {
            while (p < end && *p != ',' && *p != ']') p++;
        }
    }
    return 0;
}

/* The continue token of a Table list, from its metadata */
static NOINLINE void read_continue(const char* p, const char* rows) {
    uint8_t n = 0;
    cont_len = 0;
    const char* c = find(p, rows, "\"continue\":");
    if (c && jstr(c, rows, cont, sizeof cont, &n)) cont_len = n;
}

/* ---- calls ----------------------------------------------------------------- */
static NOINLINE void url_put(const char* s, uint32_t n) { url_len += copy(url + url_len, URL_CAP - url_len, s, n); }

/* s percent-encoded, as a query value */
static NOINLINE void url_enc(const char* s, uint32_t n) {
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < n && url_len + 3 < URL_CAP; i++) {
        const char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            url[url_len++] = c;
        } else {
            url[url_len++] = '%';
            url[url_len++] = hex[(unsigned char)c >> 4];
            url[url_len++] = hex[c & 15];
        }
    }
}

/* A Table page of `path` (with or without a query) */
static NOINLINE void list_url(const char* path, const char* sel, uint32_t sel_len) {
    url_len = 0;
    url_put(api, api_len);
    url_put(path, slen(path));
    url_put("?limit=", 7);
    char t[10];
    url_put(t, put_uint(t, page_rows));
    if (sel_len) { url_put("&labelSelector=", 15); url_enc(sel, sel_len); }
    if (cont_len) { url_put("&continue=", 10); url_enc(cont, cont_len); }
}

static NOINLINE int32_t call(const char* hdr, uint32_t hdr_len) {
    resp_len = 0;
    return k32_http_call("GET", 3, url, url_len, hdr, hdr_len, 0, 0, resp, RESP_CAP, &resp_len, timeout_ms);
}

static const char* reason(int32_t rc) {
    switch (rc) {
        case K32_ERR_PERM: return "denied";
        case K32_ERR_NOENT: return "no service";
        case K32_ERR_TIMEOUT: return "no answer";
        case K32_ERR_CONNREFUSED: return "refused";
        case K32_ERR_HOSTUNREACH: return "unreachable";
        case K32_ERR_IO: return "no connection";
        default: return "error";
    }
}

static NOINLINE void note_api(int32_t rc) {
    api_status = rc;
    const int ok = rc == 200 || rc == K32_ERR_TOOBIG;
    if (ok == (logged_status == 200)) return;
    logged_status = ok ? 200 : rc;
    lputs("clusterview: API ");
    lput(api, api_len);
    if (ok) lputs(" answers again");
    else if (rc > 0) { lputs(": HTTP "); lint(rc); }
    else { lputs(": "); lputs(reason(rc)); lputs(" ("); lint(rc); lputs(")"); }
    log_line();
}

static int api_ok(void) { return api_status == 200 || api_status == K32_ERR_TOOBIG || api_status == 0; }

/* ---- nodes ----------------------------------------------------------------- */
static NOINLINE void parse_nodes(void) {
    const char* p = (const char*)resp;
    const char* end = p + resp_len;
    const char* rows = find(p, end, "\"rows\":[");
    if (!rows) return;
    read_continue(p, rows);
    static char cells[2][NAME_CAP];
    uint8_t lens[8];
    for (p = rows; (p = find(p, end, "\"cells\":[")) != 0;) {
        lens[0] = lens[1] = 0;
        p = row_cells(p, end, 3, cells, lens);
        if (!p) break;
        total_nodes++;
        if (n_next_nodes >= MAX_NODES || !lens[0]) continue;
        struct node* n = &next_nodes[n_next_nodes++];
        n->name_len = (uint8_t)copy(n->name, NAME_CAP, cells[0], lens[0]);
        /* "Ready", or "Ready,SchedulingDisabled" for a cordoned node */
        n->ready = lens[1] >= 5 && same(cells[1], 5, "Ready", 5) && (lens[1] == 5 || cells[1][5] == ',');
        n->has_tags = 0;
        n->tags_len = 0;
        n->tags_at = 0;
    }
}

static NOINLINE struct node* node_named(struct node* list, uint32_t count, const char* name, uint32_t len) {
    for (uint32_t i = 0; i < count; i++) if (same(list[i].name, list[i].name_len, name, len)) return &list[i];
    return 0;
}

static NOINLINE int name_before(const struct node* a, const struct node* b) {
    for (uint32_t i = 0; i < a->name_len && i < b->name_len; i++) {
        if (a->name[i] != b->name[i]) return (unsigned char)a->name[i] < (unsigned char)b->name[i];
    }
    return a->name_len < b->name_len;
}

/* The new list replaces the old one; hardware already read carries over. A
 * refresh that missed rows keeps the nodes it did not see. */
static NOINLINE void commit_nodes(void) {
    for (uint32_t i = 0; i < n_next_nodes; i++) {
        struct node* old = node_named(nodes, n_nodes, next_nodes[i].name, next_nodes[i].name_len);
        if (old) {
            next_nodes[i].has_tags = old->has_tags;
            next_nodes[i].tags_len = (uint8_t)copy(next_nodes[i].tags, sizeof next_nodes[i].tags, old->tags, old->tags_len);
            next_nodes[i].tags_at = old->tags_at;
        }
    }
    for (uint32_t i = 0; truncated && i < n_nodes && n_next_nodes < MAX_NODES; i++) {
        if (!node_named(next_nodes, n_next_nodes, nodes[i].name, nodes[i].name_len)) next_nodes[n_next_nodes++] = nodes[i];
    }
    /* By name, so rows keep their places whatever order the list comes in */
    n_nodes = 0;
    for (uint32_t i = 0; i < n_next_nodes; i++) {
        uint32_t k = n_nodes++;
        while (k && name_before(&next_nodes[i], &nodes[k - 1])) { nodes[k] = nodes[k - 1]; k--; }
        nodes[k] = next_nodes[i];
    }
}

static NOINLINE int tag_is(const char* k, uint32_t kn, const char* name) { return same(k, kn, name, slen(name)); }

static NOINLINE void tag_add(struct node* n, const char* t, uint32_t len) {
    if (n->tags_len) n->tags_len += (uint8_t)copy(n->tags + n->tags_len, sizeof n->tags - n->tags_len, " ", 1);
    n->tags_len += (uint8_t)copy(n->tags + n->tags_len, sizeof n->tags - n->tags_len, t, len);
}

/* Short tags from the Node's capacity */
static NOINLINE void parse_capacity(struct node* n) {
    const char* p = (const char*)resp;
    const char* end = p + resp_len;
    p = find(p, end, "\"capacity\":{");
    if (!p) return;
    enum { DISP = 1, KEYS = 2, SPK = 4, MIC = 8, SD = 16, I2C = 32, IRTX = 64, IRRX = 128, TOUCH = 256 };
    uint32_t has = 0;
    char key[40], val[12], gpio[12];
    uint8_t kn = 0, vn = 0, gn = 0;
    while (p < end && *p != '}') {
        if (*p != '"') { p++; continue; }
        p = jstr(p, end, key, sizeof key, &kn);
        if (!p || (p = find(p, end, ":")) == 0) return;
        p = jstr(p, end, val, sizeof val, &vn);
        if (!p) return;
        if (tag_is(key, kn, "hardware.kube32.io/tft-display")) has |= DISP;
        else if (tag_is(key, kn, "hardware.kube32.io/keyboard")) has |= KEYS;
        else if (tag_is(key, kn, "hardware.kube32.io/touch")) has |= TOUCH;
        else if (tag_is(key, kn, "hardware.kube32.io/speaker")) has |= SPK;
        else if (tag_is(key, kn, "hardware.kube32.io/microphone")) has |= MIC;
        else if (tag_is(key, kn, "hardware.kube32.io/sd")) has |= SD;
        else if (tag_is(key, kn, "esp32.io/i2c")) has |= I2C;
        else if (tag_is(key, kn, "esp32.io/rmt-tx")) has |= IRTX;
        else if (tag_is(key, kn, "esp32.io/rmt-rx")) has |= IRRX;
        else if (tag_is(key, kn, "esp32.io/gpio")) gn = (uint8_t)copy(gpio, sizeof gpio, val, vn);
    }
    n->tags_len = 0;
    if (has & DISP) tag_add(n, "disp", 4);
    if (has & TOUCH) tag_add(n, "touch", 5);
    if (has & KEYS) tag_add(n, "keys", 4);
    if (has & SPK) tag_add(n, "spk", 3);
    if (has & MIC) tag_add(n, "mic", 3);
    if (has & SD) tag_add(n, "sd", 2);
    if (has & I2C) tag_add(n, "i2c", 3);
    if ((has & IRTX) && (has & IRRX)) tag_add(n, "ir", 2);
    else if (has & IRTX) tag_add(n, "ir-tx", 5);
    else if (has & IRRX) tag_add(n, "ir-rx", 5);
    if (gn) {
        char t[16];
        uint32_t k = copy(t, 4, "gpio", 4);
        k += copy(t + k, sizeof t - k, gpio, gn);
        tag_add(n, t, k);
    }
    if (!n->tags_len) tag_add(n, "no devices", 10);
    n->has_tags = 1;
}

/* The node whose hardware is due: never read (and not tried in this
 * refresh) first, then the one read longest ago */
static NOINLINE int detail_due(uint32_t now) {
    int best = -1;
    for (uint32_t i = 0; i < n_nodes; i++) {
        if (!nodes[i].has_tags) {
            if (!nodes[i].tags_at || (int32_t)(nodes[i].tags_at - cycle_at) < 0) return (int)i;
            continue;
        }
        if (now - nodes[i].tags_at >= TAGS_MS && (best < 0 || nodes[i].tags_at < nodes[best].tags_at)) best = (int)i;
    }
    return best;
}

/* ---- pods ------------------------------------------------------------------ */
static NOINLINE void parse_pods(void) {
    const char* p = (const char*)resp;
    const char* end = p + resp_len;
    const char* rows = find(p, end, "\"rows\":[");
    if (!rows) return;
    read_continue(p, rows);
    static char cells[8][NAME_CAP];
    uint8_t lens[8];
    for (p = rows; (p = find(p, end, "\"cells\":[")) != 0;) {
        for (int i = 0; i < 8; i++) lens[i] = 0;
        p = row_cells(p, end, 1 | 4 | 64, cells, lens);   /* NAME, STATUS, NODE */
        if (!p) break;
        if (n_next_pods >= MAX_PODS || !lens[0]) continue;
        struct pod* d = &next_pods[n_next_pods++];
        d->name_len = (uint8_t)copy(d->name, NAME_CAP, cells[0], lens[0]);
        d->phase_len = (uint8_t)copy(d->phase, sizeof d->phase, cells[2], lens[2]);
        d->node_len = same(cells[6], lens[6], "<none>", 6) ? 0 : (uint8_t)copy(d->node, NAME_CAP, cells[6], lens[6]);
        d->from_len = 0;
        d->moved_at = 0;
    }
}

/* A pod's name without the suffix its owner generated ("web-7d4f9-x2kqp":
 * "web-7d4f9"), or the whole name */
static NOINLINE uint32_t base_len(const struct pod* p) {
    uint32_t n = p->name_len, k = 0;
    while (k < n && p->name[n - 1 - k] != '-') k++;
    return k == 5 && k < n ? n - k - 1 : n;
}

static NOINLINE struct pod* pod_named(struct pod* list, uint32_t count, const char* name, uint32_t len) {
    for (uint32_t i = 0; i < count; i++) if (same(list[i].name, list[i].name_len, name, len)) return &list[i];
    return 0;
}

/* A pod of the old list that `d` replaces: one of the same owner (the same
 * name before the suffix) on a node, gone from the new list or terminating */
static NOINLINE struct pod* predecessor(const struct pod* d) {
    const uint32_t b = base_len(d);
    if (b == d->name_len) return 0;
    for (uint32_t k = 0; k < n_pods; k++) {
        struct pod* o = &pods[k];
        if (!o->node_len || base_len(o) != b || !same(o->name, b, d->name, b) || same(o->name, o->name_len, d->name, d->name_len)) continue;
        const struct pod* still = pod_named(next_pods, n_next_pods, o->name, o->name_len);
        if (!still || same(still->phase, still->phase_len, "Terminating", 11)) return o;
    }
    return 0;
}

/* The new list replaces the old one. A pod on another node than before, by
 * name (a move keeps it) or as the replacement of a pod of the same owner,
 * is marked moved. A replacement not yet bound keeps the node it replaces
 * in `from` until it is. */
static NOINLINE void commit_pods(uint32_t now) {
    for (uint32_t i = 0; i < n_next_pods; i++) {
        struct pod* d = &next_pods[i];
        struct pod* old = pod_named(pods, n_pods, d->name, d->name_len);
        const char* before = 0;
        uint32_t before_len = 0;
        if (old && old->node_len) { before = old->node; before_len = old->node_len; }
        else if (old && !old->moved_at && old->from_len) { before = old->from; before_len = old->from_len; }
        else {
            const struct pod* o = predecessor(d);
            if (o) { before = o->node; before_len = o->node_len; }
        }
        if (d->node_len && before_len && !same(d->node, d->node_len, before, before_len)) {
            d->from_len = (uint8_t)copy(d->from, NAME_CAP, before, before_len);
            d->moved_at = now ? now : 1;
        } else if (!d->node_len && before_len) {
            d->from_len = (uint8_t)copy(d->from, NAME_CAP, before, before_len);
        } else if (old && old->moved_at && now - old->moved_at < MOVE_MS) {
            d->from_len = (uint8_t)copy(d->from, NAME_CAP, old->from, old->from_len);
            d->moved_at = old->moved_at;
        }
    }
    for (uint32_t i = 0; truncated && i < n_pods && n_next_pods < MAX_PODS; i++) {
        if (!pod_named(next_pods, n_next_pods, pods[i].name, pods[i].name_len)) next_pods[n_next_pods++] = pods[i];
    }
    for (uint32_t i = 0; i < n_next_pods; i++) pods[i] = next_pods[i];
    n_pods = n_next_pods;
}

/* ---- readings ---------------------------------------------------------------- */
static NOINLINE void read_value(struct reading* r) {
    const char* p = (const char*)resp;
    const char* end = p + resp_len;
    r->value_len = 0;
    char pat[36];
    uint32_t k = copy(pat, 1, "\"", 1);
    k += copy(pat + k, sizeof pat - 3 - k, r->field, r->field_len);
    k += copy(pat + k, 2, "\":", 2);
    pat[k] = 0;
    p = find(p, end, pat);
    if (!p) return;
    while (p < end && *p == ' ') p++;
    if (p < end && *p == '"') {
        jstr(p, end, r->value, sizeof r->value, &r->value_len);
        return;
    }
    uint32_t n = 0;
    while (p + n < end && p[n] != ',' && p[n] != '}' && p[n] != ']' && p[n] != ' ') n++;
    r->value_len = (uint8_t)copy(r->value, sizeof r->value, p, n);
}

/* "<label> <url> <field> [<unit>]" */
static NOINLINE void parse_reading(const char* s, uint32_t n) {
    struct reading* r = &readings[n_readings];
    char* dst[4] = {r->label, r->url, r->field, r->unit};
    const uint32_t caps[4] = {sizeof r->label, sizeof r->url, sizeof r->field, sizeof r->unit};
    uint8_t* lens[4] = {&r->label_len, &r->url_len, &r->field_len, &r->unit_len};
    uint32_t i = 0;
    for (int f = 0; f < 4; f++) {
        while (i < n && s[i] == ' ') i++;
        uint32_t b = i;
        while (i < n && s[i] != ' ') i++;
        *lens[f] = (uint8_t)copy(dst[f], caps[f], s + b, i - b);
    }
    if (r->url_len && r->field_len) n_readings++;
}

/* ---- screen ------------------------------------------------------------------ */
/* The boards' font: 5x7 glyphs of ASCII 32..126 in 6x8 cells, a byte a column
 * (include/hal/display_font.hpp), drawn larger than k32_display_text does */
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

static int32_t W, H, has_screen;
static uint16_t bg, fg, dim, rule, accent;
static uint16_t glyph[6 * MAX_SCALE * 8 * MAX_SCALE];

/* Layout, from the screen and what there is to show */
static int32_t node_y, node_h, node_scale, name_cols, low_y, left_w, right_x, pod_h, read_scale;
static uint32_t layout_key, drawn_key;

static uint16_t rgb565(uint32_t rgb) { return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F)); }

/* What each large field on the screen shows, by position: a field drawn
 * again with the same text and colours is left as it is */
#define BIG_FIELDS 16
static uint32_t big_at[BIG_FIELDS], big_sum[BIG_FIELDS];
static uint32_t n_big;

static NOINLINE int big_unchanged(int32_t x, int32_t y, const char* s, uint32_t n, int32_t cols, uint16_t f, uint16_t b) {
    const uint32_t at = (uint32_t)x << 16 | (uint32_t)y;
    uint32_t sum = 2166136261u ^ ((uint32_t)f << 16 | b);
    for (int32_t i = 0; i < cols; i++) sum = (sum ^ (uint8_t)((uint32_t)i < n ? s[i] : ' ')) * 16777619u;
    for (uint32_t i = 0; i < n_big; i++) {
        if (big_at[i] != at) continue;
        if (big_sum[i] == sum) return 1;
        big_sum[i] = sum;
        return 0;
    }
    if (n_big < BIG_FIELDS) {
        big_at[n_big] = at;
        big_sum[n_big++] = sum;
    }
    return 0;
}

/* n characters of s in exactly cols cells (cut, or padded with spaces) at
 * scale sc: k32_display_text at 1, else glyph by glyph through a blit, each
 * font row widened once and copied sc times */
static NOINLINE void field(int32_t x, int32_t y, const char* s, uint32_t n, int32_t cols, int32_t sc, uint16_t f, uint16_t b) {
    if (cols <= 0) return;
    if (sc <= 1) {
        char t[64];
        if (cols > 64) cols = 64;
        for (int32_t i = 0; i < cols; i++) t[i] = (uint32_t)i < n ? s[i] : ' ';
        k32_display_text(x, y, t, (uint32_t)cols, f, b);
        return;
    }
    if (big_unchanged(x, y, s, n, cols, f, b)) return;
    const int32_t gw = 6 * sc;
    for (int32_t i = 0; i < cols; i++) {
        const char c = (uint32_t)i < n ? s[i] : ' ';
        const uint8_t* g = font[(c < 32 || c > 126 ? '?' : c) - 32];
        uint16_t* row = glyph;
        for (int32_t r = 0; r < 8; r++, row += gw * sc) {
            uint16_t* p = row;
            for (int32_t col = 0; col < 6; col++) {
                const uint16_t v = col < 5 && (g[col] >> r & 1) ? f : b;
                for (int32_t k = 0; k < sc; k++) *p++ = v;
            }
            for (int32_t k = 1; k < sc; k++) __builtin_memcpy(row + k * gw, row, (uint32_t)gw * 2);
        }
        k32_display_blit(x + i * gw, y, gw, 8 * sc, glyph);
    }
}

/* Age as "now", "42s", "7m", "3h" */
static NOINLINE uint32_t age(char* out, uint32_t ms) {
    const uint32_t s = ms / 1000;
    if (s < 1) return copy(out, 3, "now", 3);
    uint32_t v = s < 60 ? s : s < 3600 ? s / 60 : s / 3600, k = put_uint(out, v);
    out[k++] = s < 60 ? 's' : s < 3600 ? 'm' : 'h';
    return k;
}

static NOINLINE void layout(void) {
    const int show_pods = selector_len > 0;
    const int show_right = n_readings > 0 || topics_len > 0 || n_msgs > 0;
    node_y = 13;
    /* Names at twice the font's size while the nodes take at most 3/5 of the screen */
    node_scale = 2;
    node_h = 17;
    if (node_y + (int32_t)n_nodes * node_h > H * 3 / 5) { node_scale = 1; node_h = 10; }
    name_cols = 4;
    for (uint32_t i = 0; i < n_nodes; i++) if (nodes[i].name_len > name_cols) name_cols = nodes[i].name_len;
    const int32_t max_cols = (W * 3 / 5) / (6 * node_scale);
    if (name_cols > max_cols) name_cols = max_cols;
    low_y = node_y + (int32_t)(n_nodes ? n_nodes : 1) * node_h + 3;
    left_w = !show_pods ? 0 : !show_right ? W : W * 11 / 20;
    right_x = !show_right ? W : left_w ? left_w + 6 : 0;
    pod_h = 19;
    if (low_y + 10 + (int32_t)n_pods * pod_h > H) pod_h = 10;
    read_scale = W - right_x >= 18 * 7 ? 3 : 2;
    layout_key = (uint32_t)n_nodes | (uint32_t)node_scale << 4 | (uint32_t)name_cols << 6 | (uint32_t)(pod_h == 19) << 12 |
                 (uint32_t)show_right << 13 | (uint32_t)show_pods << 14 | (uint32_t)n_pods << 15 | (uint32_t)n_readings << 19;
}

static NOINLINE void draw_header(void) {
    uint32_t ready = 0;
    for (uint32_t i = 0; i < n_nodes; i++) ready += nodes[i].ready;
    const int32_t cols = W / 6;
    char t[48];
    uint32_t k = 0;
    uint16_t c = dim;
    if (!api_ok()) {
        k = copy(t, sizeof t, "api ", 4);
        if (api_status > 0) { k += copy(t + k, 5, "HTTP ", 5); k += put_uint(t + k, (uint32_t)api_status); }
        else { const char* r = reason(api_status); k += copy(t + k, sizeof t - k, r, slen(r)); }
        c = accent;
    } else if (n_nodes) {
        if (ready < n_nodes) {
            k = put_uint(t, n_nodes - ready);
            k += copy(t + k, sizeof t - k, " not ready", 10);
            c = accent;
        } else {
            k = put_uint(t, ready);
            t[k++] = '/';
            k += put_uint(t + k, total_nodes > n_nodes ? total_nodes : n_nodes);
            k += copy(t + k, sizeof t - k, " ready", 6);
        }
    }
    const int32_t tcols = cols - (int32_t)k - 1;
    field(0, 1, title, title_len, tcols, 1, fg, bg);
    field(tcols * 6, 1, " ", 1, 1, 1, fg, bg);
    field((tcols + 1) * 6, 1, t, k, (int32_t)k, 1, c, bg);
    k32_display_rect(0, 11, W, 1, rule);
}

/* Tags wrapped at word ends into lines of cols */
static NOINLINE uint32_t wrap(const char* s, uint32_t n, uint32_t cols) {
    if (n <= cols) return n;
    uint32_t k = cols;
    while (k && s[k] != ' ') k--;
    return k ? k : cols;
}

static NOINLINE void draw_nodes(void) {
    const int32_t tag_x = name_cols * 6 * node_scale + 6;
    const int32_t tag_cols = (W - tag_x) / 6;
    if (!n_nodes) {
        field(0, node_y, "no nodes", 8, W / 6, 1, dim, bg);
        return;
    }
    for (uint32_t i = 0; i < n_nodes; i++) {
        const struct node* n = &nodes[i];
        const int32_t y = node_y + (int32_t)i * node_h;
        const uint16_t b = n->ready ? bg : accent;
        /* names dim while the API server does not answer: what they say is old */
        field(0, y, n->name, n->name_len, name_cols, node_scale, !n->ready ? bg : api_ok() ? fg : dim, b);
        k32_display_rect(tag_x - 6, y, 6, 8 * node_scale, b);
        char t[64];
        uint32_t k = 0;
        if (!n->ready) k = copy(t, sizeof t, "not ready ", 10);
        k += copy(t + k, sizeof t - k, n->tags, n->tags_len);
        const uint16_t tf = n->ready ? dim : bg;
        if (tag_x + tag_cols * 6 < W) k32_display_rect(tag_x + tag_cols * 6, y, W - tag_x - tag_cols * 6, 8 * node_scale, b);
        if (node_scale == 1) {
            field(tag_x, y, t, k, tag_cols, 1, tf, b);
            continue;
        }
        const uint32_t first = wrap(t, k, (uint32_t)tag_cols);
        const uint32_t rest = first < k && t[first] == ' ' ? first + 1 : first;
        field(tag_x, y, t, first, tag_cols, 1, tf, b);
        field(tag_x, y + 8, t + rest, k - rest, tag_cols, 1, tf, b);
    }
}

static NOINLINE void draw_pods(uint32_t now) {
    if (!left_w) return;
    const int32_t cols = left_w / 6 - 1;
    char t[64];
    uint32_t k = copy(t, sizeof t, "pods ", 5);
    k += copy(t + k, sizeof t - k, selector, selector_len);
    field(0, low_y, t, k, cols, 1, dim, bg);
    int32_t y = low_y + 11;
    if (!n_pods) {
        field(0, y, "none selected", 13, cols, 1, dim, bg);
        return;
    }
    for (uint32_t i = 0; i < n_pods && y + pod_h - 1 <= H; i++, y += pod_h) {
        const struct pod* p = &pods[i];
        const int moved = p->moved_at && now - p->moved_at < MOVE_MS;
        const uint16_t b = moved ? accent : bg, f = moved ? bg : fg, d = moved ? bg : dim;
        const int running = same(p->phase, p->phase_len, "Running", 7);
        int32_t pc = (int32_t)p->phase_len;
        if (pod_h == 10) {
            /* name, node: one line */
            const int32_t nc = cols * 3 / 5;
            field(0, y, p->name, p->name_len, nc, 1, f, b);
            field(nc * 6, y, p->node, p->node_len, cols - nc, 1, running ? d : f, b);
            continue;
        }
        /* The status only when it is not Running, at the right */
        if (running) pc = 0;
        else if (pc > cols / 2) pc = cols / 2;
        field(0, y, p->name, p->name_len, cols - pc, 1, f, b);
        if (pc) {
            field((cols - pc - 1) * 6, y, " ", 1, 1, 1, f, b);
            field((cols - pc) * 6, y, p->phase, p->phase_len, pc, 1, moved ? bg : accent, b);
        }
        k = 0;
        if (moved) {
            k = copy(t, 24, p->from, p->from_len);
            k += copy(t + k, 3, " > ", 3);
        } else {
            k = copy(t, 3, "on ", 3);
        }
        k += copy(t + k, sizeof t - k, p->node_len ? p->node : "no node", p->node_len ? p->node_len : 7);
        field(0, y + 8, t, k, cols, 1, d, b);
        k32_display_rect(0, y + 16, cols * 6, 2, bg);
    }
}

static NOINLINE int32_t draw_readings(void) {
    const int32_t x = right_x, cols = (W - x) / 6;
    int32_t y = low_y;
    for (uint32_t i = 0; i < n_readings; i++) {
        const struct reading* r = &readings[i];
        const int ok = r->status == 200 && r->value_len;
        char t[48];
        uint32_t k = copy(t, sizeof t, r->label, r->label_len);
        if (!ok && r->status) {
            k += copy(t + k, 1, " ", 1);
            if (r->status > 0) { k += copy(t + k, 5, "HTTP ", 5); k += put_uint(t + k, (uint32_t)r->status); }
            else { const char* s = reason(r->status); k += copy(t + k, sizeof t - k, s, slen(s)); }
        }
        field(x, y, t, k, cols, 1, ok || !r->status ? dim : accent, bg);
        y += 10;
        const int32_t unit_cols = r->unit_len ? r->unit_len + 1 : 0;
        int32_t vcols = (cols - unit_cols) / read_scale;
        if (vcols < 1) vcols = 1;
        /* right-aligned, next to its unit */
        const char* v = ok ? r->value : "--";
        int32_t vn = ok ? r->value_len : 2;
        if (vn > vcols) vn = vcols;
        k = 0;
        while ((int32_t)k < vcols - vn) t[k++] = ' ';
        k += copy(t + k, sizeof t - k, v, (uint32_t)vn);
        field(x, y, t, k, vcols, read_scale, ok ? fg : dim, bg);
        const int32_t ux = x + vcols * 6 * read_scale;
        if (unit_cols) {
            k = copy(t, 1, " ", 1);
            k += copy(t + k, sizeof t - k, r->unit, r->unit_len);
            k32_display_rect(ux, y, unit_cols * 6, 8 * read_scale - 8, bg);
            field(ux, y + 8 * read_scale - 8, t, k, unit_cols, 1, dim, bg);
        }
        y += 8 * read_scale + 4;
    }
    return y;
}

static int32_t msgs_y;

static NOINLINE void draw_msgs(uint32_t now) {
    if (right_x >= W || (!topics_len && !n_msgs)) return;
    const int32_t x = right_x, cols = (W - x) / 6;
    int32_t y = msgs_y;
    if (!n_msgs) {
        char t[80];
        uint32_t k = copy(t, sizeof t, "waiting for ", 12);
        k += copy(t + k, sizeof t - k, topics, topics_len);
        field(x, y, t, k, cols, 1, dim, bg);
        return;
    }
    for (uint32_t i = 0; i < n_msgs && y + 18 <= H; i++, y += 20) {
        const struct msg* m = &msgs[i];
        char a[8];
        const uint32_t an = age(a, now > m->at ? now - m->at : 0);
        field(x, y, m->topic, m->topic_len, cols - (int32_t)an, 1, dim, bg);
        field(x + (cols - (int32_t)an) * 6, y, a, an, (int32_t)an, 1, dim, bg);
        field(x, y + 8, m->text, m->text_len, cols, 1, i ? dim : fg, bg);
    }
}

/* The whole screen, or between refreshes (full 0) what changes with time:
 * the pods' highlights and the messages' ages */
static NOINLINE void draw(uint32_t now, int full) {
    if (!has_screen) return;
    layout();
    if (!full && layout_key == drawn_key) {
        draw_pods(now);
        draw_msgs(now);
        return;
    }
    if (layout_key != drawn_key) {
        k32_display_rect(0, 0, W, H, bg);
        drawn_key = layout_key;
        n_big = 0;
    }
    draw_header();
    draw_nodes();
    if (low_y < H) k32_display_rect(0, low_y - 2, W, 1, rule);
    if (left_w && right_x < W && low_y < H) k32_display_rect(left_w + 2, low_y, 1, H - low_y, rule);
    draw_pods(now);
    msgs_y = draw_readings();
    draw_msgs(now);
}

/* ---- refresh ------------------------------------------------------------------ */
/* A page of a Table list: 1 when it was read, 0 to ask again with fewer rows,
 * -1 when the list cannot be read now. A page that does not fit even with
 * one row is read as far as it goes, and the refresh keeps what it missed. */
static NOINLINE int list_page(int32_t rc) {
    if (rc == K32_ERR_TOOBIG && page_rows > 1) {
        page_rows--;
        return 0;
    }
    if (rc == K32_ERR_TOOBIG) truncated = 1;
    return rc == 200 || rc == K32_ERR_TOOBIG ? 1 : -1;
}

/* One call of the refresh; 1 while more remain */
static NOINLINE int run_step(uint32_t now) {
    int32_t rc;
    int got;
    switch (step) {
        case S_NODES:
            list_url("/api/v1/nodes", 0, 0);
            rc = call(hdr_table, hdr_table_len);
            note_api(rc);
            got = list_page(rc);
            if (got < 0) {
                /* the API does not answer: the readings may still */
                step = S_READ;
                step_arg = 0;
                return 1;
            }
            if (!got) return 1;
            parse_nodes();
            if (cont_len) return 1;
            commit_nodes();
            truncated = 0;
            step = selector_len ? S_PODS : S_DETAIL;
            step_arg = 0;
            return 1;
        case S_PODS: {
            const int all = same(ns, ns_len, "all", 3);
            char path[80];
            uint32_t k = copy(path, sizeof path, "/api/v1", 7);
            if (!all) {
                k += copy(path + k, sizeof path - k, "/namespaces/", 12);
                k += copy(path + k, sizeof path - k - 6, ns, ns_len);
            }
            k += copy(path + k, 5, "/pods", 5);
            path[k] = 0;
            list_url(path, selector, selector_len);
            rc = call(hdr_table, hdr_table_len);
            got = list_page(rc);
            if (got < 0) {
                note_api(rc);
                step = S_DETAIL;
                step_arg = 0;
                cont_len = 0;
                return 1;
            }
            if (!got) return 1;
            parse_pods();
            if (cont_len) return 1;
            commit_pods(now);
            truncated = 0;
            step = S_DETAIL;
            step_arg = 0;
            return 1;
        }
        case S_DETAIL: {
            /* Every node not read yet, else the one read longest ago */
            const int i = step_arg < (int)n_nodes ? detail_due(now) : -1;
            if (i < 0 || (step_arg && nodes[i].has_tags)) {
                step = S_READ;
                step_arg = 0;
                return 1;
            }
            step_arg++;
            struct node* n = &nodes[i];
            url_len = 0;
            url_put(api, api_len);
            url_put("/api/v1/nodes/", 14);
            url_enc(n->name, n->name_len);
            rc = call(hdr_plain, hdr_plain_len);
            n->tags_at = now ? now : 1;
            if (rc == 200 || rc == K32_ERR_TOOBIG) parse_capacity(n);
            return 1;
        }
        case S_READ:
            if (step_arg < (int)n_readings) {
                struct reading* r = &readings[step_arg++];
                url_len = 0;
                url_put(r->url, r->url_len);
                rc = call(0, 0);
                r->status = rc == K32_ERR_TOOBIG ? 200 : rc;
                if (rc == 200 || rc == K32_ERR_TOOBIG) read_value(r);
                return 1;
            }
            step = S_IDLE;
            return 0;
    }
    step = S_IDLE;
    return 0;
}

/* ---- entry points --------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof block ? (uint32_t)(uintptr_t)block : 0;
}

static NOINLINE void build_headers(void) {
    static const char accept[] = "Accept: application/json;as=Table;v=v1;g=meta.k8s.io\r\n";
    char token[128];
    const uint32_t tn = env("API_TOKEN", token, sizeof token);
    hdr_plain_len = 0;
    if (tn) {
        hdr_plain_len = copy(hdr_plain, sizeof hdr_plain, "Authorization: Bearer ", 22);
        hdr_plain_len += copy(hdr_plain + hdr_plain_len, sizeof hdr_plain - 2 - hdr_plain_len, token, tn);
        hdr_plain_len += copy(hdr_plain + hdr_plain_len, 2, "\r\n", 2);
    }
    hdr_table_len = copy(hdr_table, sizeof hdr_table, accept, sizeof accept - 1);
    hdr_table_len += copy(hdr_table + hdr_table_len, sizeof hdr_table - hdr_table_len, hdr_plain, hdr_plain_len);
}

static NOINLINE uint32_t hex_color(const char* key, uint32_t dflt) {
    char b[8];
    const uint32_t n = env(key, b, sizeof b);
    if (n != 6) return dflt;
    uint32_t v = 0;
    for (uint32_t i = 0; i < 6; i++) {
        const char c = b[i];
        const uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0') : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : 16;
        if (d > 15) return dflt;
        v = v << 4 | d;
    }
    return v;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
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
    build_headers();
    selector_len = env("POD_SELECTOR", selector, sizeof selector);
    ns_len = env("POD_NAMESPACE", ns, sizeof ns);
    if (!ns_len) ns_len = copy(ns, sizeof ns, "default", 7);
    topics_len = env("TOPICS", topics, sizeof topics);
    title_len = env("TITLE", title, sizeof title);
    if (!title_len) title_len = copy(title, sizeof title, "cluster", 7);
    refresh_ms = env_uint("REFRESH_MS", 2000, 500, 60000);
    timeout_ms = env_uint("TIMEOUT_MS", 2000, 100, 30000);
    static const char* const reading_keys[MAX_READINGS] = {"READING_1", "READING_2", "READING_3"};
    for (uint32_t i = 0; i < MAX_READINGS; i++) {
        char r[160];
        const uint32_t n = env(reading_keys[i], r, sizeof r);
        if (n) parse_reading(r, n);
    }
    bg = rgb565(0x0b0d10);
    fg = rgb565(0xe8eaed);
    dim = rgb565(0x8a929c);
    rule = rgb565(0x262b31);
    accent = rgb565(hex_color("ACCENT", 0xffb000));

    int32_t w = 0, h = 0, f = 0;
    const int32_t rc = k32_display_info(&w, &h, &f);
    has_screen = rc == 0 && f == K32_DISPLAY_RGB565 && w >= 120 && h >= 64;
    W = w;
    H = h;
    lputs("clusterview: ");
    if (has_screen) { lint(w); lputs("x"); lint(h); }
    else { lputs("no screen (k32_display_info "); lint(rc); lputs(", needs hardware.kube32.io/tft-display)"); }
    lputs(", API ");
    if (api_len) lput(api, api_len);
    else lputs("unknown (set API_URL)");
    if (selector_len) { lputs(", pods "); lput(selector, selector_len); lputs(" in "); lput(ns, ns_len); }
    lputs(", ");
    lint((int32_t)n_readings);
    lputs(" reading(s)");
    log_line();
    if (has_screen) {
        k32_display_rect(0, 0, W, H, bg);
        field(0, 1, title, title_len, W / 6, 1, fg, bg);
        k32_display_rect(0, 11, W, 1, rule);
        if (api_len) field(0, 14, "reading the API server", 22, W / 6, 1, dim, bg);
        else field(0, 14, "no API server: set API_URL", 26, W / 6, 1, accent, bg);
    }
    return 0;
}

/* The tick's clock (the node's uptime, which k32_time_ms also gives) at
 * the last tick, and k32_time_ms then: an event's time on the tick's clock */
static uint32_t tick_now, tick_ms;
static NOINLINE uint32_t clock_now(void) {
    const uint32_t ms = (uint32_t)k32_time_ms();
    return tick_ms ? tick_now + (ms - tick_ms) : ms;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    tick_now = now;
    tick_ms = (uint32_t)k32_time_ms();
    if (!tick_ms) tick_ms = 1;
    if (!api_len || !has_screen) return 0;
    if (step == S_IDLE) {
        if (started && now - cycle_at < refresh_ms) {
            draw(now, 0);
            return 0;
        }
        started = 1;
        cycle_at = now;
        step = S_NODES;
        cont_len = 0;
        n_next_nodes = n_next_pods = total_nodes = 0;
    }
    const int more = run_step(now);
    if (!more) draw(now, 1);
    return more;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic_ptr, uint32_t topic_len, uint32_t ptr, uint32_t len) {
    const char* topic = (const char*)(uintptr_t)topic_ptr;
    const char* s = (const char*)(uintptr_t)ptr;
    for (uint32_t i = MAX_MSGS - 1; i > 0; i--) msgs[i] = msgs[i - 1];
    struct msg* m = &msgs[0];
    m->topic_len = (uint8_t)copy(m->topic, sizeof m->topic, topic, topic_len);
    /* JSON's punctuation dropped: {"cmd":12,"ok":true} reads cmd 12 ok true */
    uint32_t n = 0;
    for (uint32_t i = 0; i < len && n < sizeof m->text; i++) {
        char c = s[i];
        if (c == '{' || c == '}' || c == '"' || c == '[' || c == ']') continue;
        if (c == ':' || c == ',' || c < 32 || c > 126) c = ' ';
        if (c == ' ' && (n == 0 || m->text[n - 1] == ' ')) continue;
        m->text[n++] = c;
    }
    while (n && m->text[n - 1] == ' ') n--;
    m->text_len = (uint8_t)n;
    m->at = clock_now();
    if (n_msgs < MAX_MSGS) n_msgs++;
    if (has_screen && drawn_key) {
        layout();
        if (layout_key == drawn_key) draw_msgs(m->at);
    }
    return 0;
}
