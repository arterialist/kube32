/* webhook: forwards channel events to an HTTP endpoint as JSON. A tick and
 * event pod, so it runs on boards whose pods share one interpreter (the
 * Cardputer): k32_event queues each event of a forwarded topic, k32_tick
 * posts the oldest one, one POST per tick. The POST is the only call that
 * waits, and WEBHOOK_TIMEOUT_MS bounds it.
 *
 * Each event goes to WEBHOOK_URL (https:// or http://) as
 *   {"topic":"rfid.tag","payload":"04a1b2c3d4e580","node":"<NODE_NAME>","time":"2026-10-30T12:34:56.789Z"}
 * with "node" when NODE_NAME is set and "time" (when the event arrived) when
 * the node's clock is set. Payload bytes outside printable ASCII are
 * escaped as \u00XX. With WEBHOOK_TEXT_FIELD the body also carries the event
 * as one line of text under that name ("text" for Slack, "content" for
 * Discord): "rfid.tag: 04a1b2c3d4e580 on <node>".
 *
 * A 2xx answer delivers the event. No answer (timeout, refused, TLS failure,
 * busy, denied) or a 408, 429 or 5xx answer keeps it at the head of the
 * queue and the next attempt waits 1 s, then 2, 4, ... up to 60 s. Any other
 * answer drops it. The queue holds QUEUE events; a new event pushes out the
 * oldest when it is full.
 *
 * Environment:
 *   WEBHOOK_URL         where events go (required; a Secret, since a webhook
 *                       URL often carries its credential)
 *   WEBHOOK_TOPICS      topics to forward, comma separated (default
 *                       rfid.tag,ir.received); the pod annotation
 *                       kube32.io/event-topics delivers them
 *   NODE_NAME           sent as "node" (fieldRef spec.nodeName)
 *   WEBHOOK_TEXT_FIELD  name of a one-line text field to add (letters,
 *                       digits, '_'; unset: none)
 *   WEBHOOK_TIMEOUT_MS  per POST (default 8000, 500 to 15000)
 *
 * GET answers {"queued":N,"delivered":N,"rejected":N,"dropped":N,"retrying":bool,"last_result":N}. */
#include "../../include/runtime/k32_app_abi.h"

#define QUEUE 6
#define TOPIC_MAX 48
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define LINE_CAP 200

struct event {
    int64_t unix_ms;                 /* 0: the node had no wall clock */
    uint16_t payload_len;
    uint8_t topic_len;
    char topic[TOPIC_MAX];
    uint8_t payload[K32_MAX_EVENT_BYTES];
};

static struct event g_queue[QUEUE];
static uint32_t g_head, g_count;
static char g_url[384];
static uint32_t g_url_len;
static char g_topics[128] = "rfid.tag,ir.received";
static uint32_t g_topics_len = 20;
static char g_node[64];
static uint32_t g_node_len;
static char g_text_field[24];
static uint32_t g_text_field_len;
static uint32_t g_timeout_ms = 8000;
static uint32_t g_next_ms, g_backoff_ms;
static uint32_t g_delivered, g_rejected, g_dropped, g_dropped_reported;
static int32_t g_last_result;
static char g_body[1600];
static uint32_t g_body_len;
static int g_body_full;
static char g_resp[32];              /* only the status matters (k32_http_status) */
static unsigned char g_block[512];   /* k32_alloc: an event, or a request */
static char g_line[LINE_CAP];
static uint32_t g_line_len;

static const char kHeaders[] = "Content-Type: application/json\r\nUser-Agent: kube32-webhook\r\n";

/* ---- text ---------------------------------------------------------------- */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void line_reset(void) { g_line_len = 0; }
static void put_n(const char* s, uint32_t n) { for (uint32_t i = 0; i < n && g_line_len < LINE_CAP; i++) g_line[g_line_len++] = s[i]; }
static void put(const char* s) { put_n(s, slen(s)); }
static void put_uint(uint32_t v) {
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t && g_line_len < LINE_CAP) g_line[g_line_len++] = tmp[--t];
}
static void put_int(int32_t v) {
    if (v < 0) { put("-"); put_uint((uint32_t)(-v)); } else put_uint((uint32_t)v);
}
static void log_line(void) { k32_log(g_line, g_line_len); }

static const char* error_name(int32_t rc) {
    switch (rc) {
        case K32_ERR_PERM: return "denied (NetworkPolicy, or no SERVICE_CALL capability)";
        case K32_ERR_NOENT: return "host not found";
        case K32_ERR_IO: return "connection failed";
        case K32_ERR_NOMEM: return "out of memory";
        case K32_ERR_BUSY: return "every TLS session in use";
        case K32_ERR_INVAL: return "bad URL";
        case K32_ERR_TLS: return "TLS handshake failed";
        case K32_ERR_NOTSUP: return "https:// not available in this cluster";
        case K32_ERR_TIMEOUT: return "no answer in time";
        case K32_ERR_CONNREFUSED: return "connection refused";
        case K32_ERR_HOSTUNREACH: return "no route to host";
        case K32_ERR_CERT: return "certificate rejected";
        default: return "error";
    }
}

/* ---- the request body ---------------------------------------------------- */
static void body_raw(const char* s, uint32_t n) {
    if (g_body_len + n > sizeof g_body) { g_body_full = 1; return; }
    for (uint32_t i = 0; i < n; i++) g_body[g_body_len++] = s[i];
}

static void body_str(const char* s) { body_raw(s, slen(s)); }

/* A JSON string's contents */
static void body_escaped(const uint8_t* s, uint32_t n) {
    static const char hex[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t c = s[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            body_raw(e, 2);
        } else if (c >= 0x20 && c < 0x7f) {
            body_raw((const char*)&c, 1);
        } else {
            char e[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            body_raw(e, 6);
        }
    }
}

static void body_digits(uint32_t v, int width) {
    char d[10];
    for (int i = width - 1; i >= 0; i--) { d[i] = (char)('0' + v % 10); v /= 10; }
    body_raw(d, (uint32_t)width);
}

/* RFC 3339 UTC with milliseconds (days to civil date: H. Hinnant's algorithm) */
static void body_time(int64_t unix_ms) {
    const int64_t secs = unix_ms / 1000;
    const int64_t z = secs / 86400 + 719468;
    const uint32_t sod = (uint32_t)(secs % 86400);
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    const uint32_t day = doy - (153 * mp + 2) / 5 + 1;
    const uint32_t month = mp < 10 ? mp + 3 : mp - 9;
    const uint32_t year = (uint32_t)(yoe + era * 400 + (month <= 2));
    body_digits(year, 4); body_raw("-", 1); body_digits(month, 2); body_raw("-", 1); body_digits(day, 2);
    body_raw("T", 1); body_digits(sod / 3600, 2); body_raw(":", 1); body_digits(sod / 60 % 60, 2); body_raw(":", 1);
    body_digits(sod % 60, 2); body_raw(".", 1); body_digits((uint32_t)(unix_ms % 1000), 3); body_raw("Z", 1);
}

static int build_body(const struct event* e) {
    g_body_len = 0;
    g_body_full = 0;
    body_str("{\"topic\":\"");
    body_escaped((const uint8_t*)e->topic, e->topic_len);
    body_str("\",\"payload\":\"");
    body_escaped(e->payload, e->payload_len);
    body_str("\"");
    if (g_node_len) {
        body_str(",\"node\":\"");
        body_escaped((const uint8_t*)g_node, g_node_len);
        body_str("\"");
    }
    if (e->unix_ms > 0) {
        body_str(",\"time\":\"");
        body_time(e->unix_ms);
        body_str("\"");
    }
    if (g_text_field_len) {
        body_str(",\"");
        body_raw(g_text_field, g_text_field_len);
        body_str("\":\"");
        body_escaped((const uint8_t*)e->topic, e->topic_len);
        body_str(": ");
        body_escaped(e->payload, e->payload_len);
        if (g_node_len) {
            body_str(" on ");
            body_escaped((const uint8_t*)g_node, g_node_len);
        }
        body_str("\"");
    }
    body_str("}");
    return !g_body_full;
}

/* ---- the queue ----------------------------------------------------------- */
static void pop(void) {
    g_head = (g_head + 1) % QUEUE;
    g_count--;
}

static int forwarded(const char* topic, uint32_t len) {
    uint32_t i = 0;
    while (i < g_topics_len) {
        while (i < g_topics_len && (g_topics[i] == ',' || g_topics[i] == ' ')) i++;
        uint32_t start = i;
        while (i < g_topics_len && g_topics[i] != ',') i++;
        uint32_t end = i;
        while (end > start && g_topics[end - 1] == ' ') end--;
        if (end - start == len) {
            uint32_t k = 0;
            while (k < len && g_topics[start + k] == topic[k]) k++;
            if (k == len) return 1;
        }
    }
    return 0;
}

/* ---- exports ------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

static int32_t env_text(const char* key, char* out, uint32_t cap) {
    int32_t n = k32_env_get(key, slen(key), out, cap);
    return (n > (int32_t)cap) ? -2 : n;   /* -2: longer than the buffer */
}

static int starts_with(const char* s, uint32_t n, const char* p) {
    uint32_t pn = slen(p);
    if (n < pn) return 0;
    for (uint32_t i = 0; i < pn; i++) if (s[i] != p[i]) return 0;
    return 1;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    int32_t n = env_text("WEBHOOK_URL", g_url, sizeof g_url);
    line_reset();
    if (n == -2) {
        put("webhook: WEBHOOK_URL is longer than "); put_uint(sizeof g_url); put(" bytes");
        log_line();
        return 1;
    }
    if (n <= 0 || !(starts_with(g_url, (uint32_t)n, "https://") || starts_with(g_url, (uint32_t)n, "http://"))) {
        put("webhook: WEBHOOK_URL must be set to an https:// or http:// URL");
        log_line();
        return 1;
    }
    g_url_len = (uint32_t)n;
    n = env_text("WEBHOOK_TOPICS", g_topics, sizeof g_topics);
    if (n > 0) g_topics_len = (uint32_t)n;
    else if (n == 0 || n == -2) {
        if (n == -2) { put("webhook: WEBHOOK_TOPICS is too long, using the default"); log_line(); line_reset(); }
        const char* d = "rfid.tag,ir.received";
        g_topics_len = slen(d);
        for (uint32_t i = 0; i < g_topics_len; i++) g_topics[i] = d[i];
    }
    n = env_text("NODE_NAME", g_node, sizeof g_node);
    g_node_len = n > 0 ? (uint32_t)n : 0;
    n = env_text("WEBHOOK_TEXT_FIELD", g_text_field, sizeof g_text_field);
    g_text_field_len = n > 0 ? (uint32_t)n : 0;
    for (uint32_t i = 0; i < g_text_field_len; i++) {
        const char c = g_text_field[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) g_text_field_len = 0;
    }
    if (n != 0 && n != -1 && !g_text_field_len) {
        put("webhook: WEBHOOK_TEXT_FIELD must be letters, digits and '_' (at most 24): no text field");
        log_line();
        line_reset();
    }
    char num[12];
    n = env_text("WEBHOOK_TIMEOUT_MS", num, sizeof num);
    if (n > 0) {
        uint32_t v = 0;
        for (int32_t i = 0; i < n && num[i] >= '0' && num[i] <= '9'; i++) v = v * 10 + (uint32_t)(num[i] - '0');
        g_timeout_ms = v < 500 ? 500 : (v > 15000 ? 15000 : v);
    }
    /* The URL may carry a credential: only its scheme and host are logged */
    uint32_t host_end = starts_with(g_url, g_url_len, "https://") ? 8 : 7;
    while (host_end < g_url_len && g_url[host_end] != '/' && g_url[host_end] != '?') host_end++;
    put("webhook: forwarding "); put_n(g_topics, g_topics_len); put(" to "); put_n(g_url, host_end);
    log_line();
    return 0;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t tptr, uint32_t tlen, uint32_t ptr, uint32_t len) {
    const char* topic = (const char*)(uintptr_t)tptr;
    if (!forwarded(topic, tlen)) return 0;
    if (tlen > TOPIC_MAX) tlen = TOPIC_MAX;
    if (len > K32_MAX_EVENT_BYTES) len = K32_MAX_EVENT_BYTES;
    if (g_count == QUEUE) {
        line_reset();
        if (g_dropped == g_dropped_reported) {   /* once per run of drops */
            put("webhook: queue full, dropping the oldest events ("); put_n(g_queue[g_head].topic, g_queue[g_head].topic_len); put(" first)");
            log_line();
        }
        pop();
        g_dropped++;
    }
    struct event* e = &g_queue[(g_head + g_count) % QUEUE];
    e->unix_ms = k32_unix_time_ms();
    e->topic_len = (uint8_t)tlen;
    for (uint32_t i = 0; i < tlen; i++) e->topic[i] = topic[i];
    e->payload_len = (uint16_t)len;
    const uint8_t* payload = (const uint8_t*)(uintptr_t)ptr;
    for (uint32_t i = 0; i < len; i++) e->payload[i] = payload[i];
    g_count++;
    return 0;
}

/* A failure the next attempt may not have: no answer, or 408, 429, 5xx */
static int retryable(int32_t rc) {
    if (rc < 0) return rc != K32_ERR_INVAL && rc != K32_ERR_NOTSUP;
    return rc == 408 || rc == 429 || rc >= 500 || rc < 200;
}

/* Returns 1 while events wait and the last POST went through, so the node
 * ticks again soon */
K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    if (!g_count || !g_url_len) return 0;
    if (g_backoff_ms && (int32_t)(uptime_ms - g_next_ms) < 0) return 0;
    struct event* e = &g_queue[g_head];
    line_reset();
    put("webhook: ");
    put_n(e->topic, e->topic_len);
    if (!build_body(e)) {
        put(" does not fit the request buffer, dropped");
        log_line();
        pop();
        g_rejected++;
        return g_count ? 1 : 0;
    }
    uint32_t got = 0;
    const int64_t t0 = k32_time_ms();
    int32_t rc = k32_http_call("POST", 4, g_url, g_url_len, kHeaders, sizeof kHeaders - 1, (const uint8_t*)g_body, g_body_len,
                               (uint8_t*)g_resp, sizeof g_resp, &got, g_timeout_ms);
    const uint32_t ms = (uint32_t)(k32_time_ms() - t0);
    /* A body larger than g_resp (an error page, often): the status decides */
    if (rc == K32_ERR_TOOBIG && k32_http_status() > 0) rc = k32_http_status();
    g_last_result = rc;
    if (rc >= 200 && rc < 300) {
        put(" delivered ("); put_int(rc); put(", "); put_uint(ms); put(" ms)");
        log_line();
        pop();
        g_delivered++;
        g_backoff_ms = 0;
        if (g_dropped != g_dropped_reported) {
            line_reset();
            put("webhook: "); put_uint(g_dropped - g_dropped_reported); put(" event(s) dropped while the queue was full");
            log_line();
            g_dropped_reported = g_dropped;
        }
        return g_count ? 1 : 0;
    }
    if (!retryable(rc)) {
        put(" rejected (");
        if (rc < 0) { put(error_name(rc)); put(", "); }
        put_int(rc); put("), dropped");
        log_line();
        pop();
        g_rejected++;
        g_backoff_ms = 0;
        return g_count ? 1 : 0;
    }
    g_backoff_ms = g_backoff_ms ? (g_backoff_ms >= BACKOFF_MAX_MS / 2 ? BACKOFF_MAX_MS : g_backoff_ms * 2) : BACKOFF_MIN_MS;
    g_next_ms = uptime_ms + g_backoff_ms;
    put(" not delivered (");
    if (rc < 0) { put(error_name(rc)); put(", "); }
    put_int(rc); put("), retry in "); put_uint(g_backoff_ms / 1000); put(" s");
    log_line();
    return 0;
}

static void reply_line(void) { k32_resp_write(g_line, g_line_len); }

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr; (void)len;
    k32_resp_begin(200, "application/json", 16);
    line_reset();
    put("{\"queued\":"); put_uint(g_count);
    put(",\"delivered\":"); put_uint(g_delivered);
    put(",\"rejected\":"); put_uint(g_rejected);
    put(",\"dropped\":"); put_uint(g_dropped);
    put(",\"retrying\":"); put(g_backoff_ms ? "true" : "false");
    put(",\"last_result\":"); put_int(g_last_result);
    put("}\n");
    reply_line();
    k32_resp_end();
    return 200;
}
