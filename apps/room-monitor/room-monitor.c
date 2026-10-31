/* room-monitor: one view of a room's devices. It knows Services, not
 * hardware: each request calls the driver behind ENV_URL and SCALE_URL
 * (GET /, see apps/drivers) with k32_http_call and answers with what they
 * said, so it runs on any node, a simulator worker as well, whichever board
 * the parts are wired to.
 *
 *   GET  /       {"room":"kitchen","env":<the env driver's GET / body>,"scale":<the scale's>}
 *                (a device that could not be reached: {"ok":false,"error":"..."})
 *   GET  /view   the same as a small HTML page that reloads itself
 *   POST /tare   {"action":"tare"} to the scale; its answer, or back to the
 *                page when the page's button sent it
 *
 * Paths match on their end, so the app serves the same behind an Ingress
 * prefix (/kitchen/view).
 *
 * Environment:
 *   ROOM        the room's name (default room)
 *   ENV_URL     the environment sensor's Service, e.g. http://kitchen-env
 *   SCALE_URL   the scale's Service, e.g. http://kitchen-scale
 *   TIMEOUT_MS  per call (default 3000)
 * Either URL may be left out. The pod needs the SERVICE_CALL capability. */
#include "../../include/runtime/k32_app_abi.h"

#define URL_CAP 96
#define ANSWER_CAP 400
#define LINE_CAP 160

struct device {
    const char* name;                    /* "env", "scale" */
    const char* env;                     /* its URL variable */
    char url[URL_CAP];
    uint32_t url_len;
    char answer[ANSWER_CAP];             /* the driver's body, or {"ok":false,...} */
    uint32_t answer_len;
    int32_t status;                      /* HTTP status, or a negative K32_ERR_* */
};

static struct device devices[2] = {{"env", "ENV_URL", {0}, 0, {0}, 0, 0}, {"scale", "SCALE_URL", {0}, 0, {0}, 0, 0}};
static char room[48] = "room";
static uint32_t room_len = 4, timeout_ms = 3000;
static char line[LINE_CAP];
static uint32_t line_len;
static uint8_t req[1024];

/* ---- text ---------------------------------------------------------------- */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void put_n(const char* s, uint32_t n) { while (n-- && line_len < LINE_CAP) line[line_len++] = *s++; }
static void put(const char* s) { put_n(s, slen(s)); }
static void put_int(int32_t v) {
    char t[12]; int n = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) put("-");
    while (n) put_n(&t[--n], 1);
}
/* Sends line[] as part of the response */
static void flush(void) { k32_resp_write(line, line_len); line_len = 0; }
static void send(const char* s, uint32_t n) { flush(); k32_resp_write(s, n); }

/* Text from a device's answer in the page: quotes of a JSON string dropped, <, > and & escaped */
static void send_text(const char* s, uint32_t n) {
    if (n >= 2 && s[0] == '"') { s++; n -= 2; }
    for (uint32_t i = 0; i < n; i++) {
        if (line_len > LINE_CAP - 8) flush();
        if (s[i] == '<') put("&lt;");
        else if (s[i] == '>') put("&gt;");
        else if (s[i] == '&') put("&amp;");
        else put_n(s + i, 1);
    }
    flush();
}

static int contains(const char* s, const char* end, const char* what) {
    const uint32_t n = slen(what);
    for (; s + n <= end; s++) {
        uint32_t i = 0;
        while (i < n && s[i] == what[i]) i++;
        if (i == n) return 1;
    }
    return 0;
}

static int ends_with(const char* s, uint32_t n, const char* tail) {
    uint32_t t = slen(tail);
    if (n < t) return 0;
    for (uint32_t i = 0; i < t; i++) if (s[n - t + i] != tail[i]) return 0;
    return 1;
}

static const char* error_name(int32_t rc) {
    switch (rc) {
        case K32_ERR_PERM: return "denied (NetworkPolicy, or no SERVICE_CALL capability)";
        case K32_ERR_NOENT: return "no such Service, or no ready pod behind it";
        case K32_ERR_IO: return "the device's node could not be reached";
        case K32_ERR_TOOBIG: return "the answer is too large";
        case K32_ERR_TIMEOUT: return "no answer in time";
        case K32_ERR_LOOP: return "the call would loop";
        default: return "call failed";
    }
}

/* ---- calls --------------------------------------------------------------- */
/* Calls the device's driver: `body` (or GET) at its URL plus `path` */
static void call(struct device* d, const char* method, const char* path, const char* body) {
    char url[URL_CAP + 8];
    uint32_t n = 0;
    for (uint32_t i = 0; i < d->url_len; i++) url[n++] = d->url[i];
    for (const char* p = path; *p; p++) url[n++] = *p;
    static const char json[] = "Content-Type: application/json\r\n";
    uint32_t got = 0;
    d->status = k32_http_call(method, slen(method), url, n, body ? json : 0, body ? sizeof json - 1 : 0, (const uint8_t*)body,
                              body ? slen(body) : 0, (uint8_t*)d->answer, sizeof d->answer, &got, timeout_ms);
    d->answer_len = got;
    if (d->status > 0 && got && d->answer[0] == '{') {
        while (d->answer_len && (d->answer[d->answer_len - 1] == '\n' || d->answer[d->answer_len - 1] == '\r')) d->answer_len--;
        return;
    }
    /* No JSON answer: say why */
    line_len = 0;
    put("{\"ok\":false,\"error\":\"");
    if (d->status > 0) { put("HTTP "); put_int(d->status); }
    else { put(error_name(d->status)); put(" ("); put_int(d->status); put(")"); }
    put("\"}");
    for (d->answer_len = 0; d->answer_len < line_len; d->answer_len++) d->answer[d->answer_len] = line[d->answer_len];
    line_len = 0;
}

/* The raw value of "key" in a device's answer (a number, true, false, null
 * or a string with its quotes): its length, 0 when absent */
static uint32_t value(const struct device* d, const char* key, const char** out) {
    const uint32_t k = slen(key);
    for (uint32_t i = 0; i + k + 3 < d->answer_len; i++) {
        const char* s = d->answer + i;
        uint32_t j = 0;
        while (j < k && s[1 + j] == key[j]) j++;
        if (j < k || s[0] != '"' || s[k + 1] != '"' || s[k + 2] != ':') continue;
        const char* v = s + k + 3;
        uint32_t n = 0;
        if (*v == '"') { n = 1; while (i + k + 3 + n < d->answer_len && v[n] != '"') n++; n++; }
        else while (i + k + 3 + n < d->answer_len && v[n] != ',' && v[n] != '}') n++;
        *out = v;
        return n;
    }
    return 0;
}

/* ---- answers --------------------------------------------------------------- */
static void answer_json(void) {
    k32_resp_begin(200, "application/json", 16);
    put("{\"room\":\""); put_n(room, room_len); put("\"");
    for (int i = 0; i < 2; i++) {
        struct device* d = &devices[i];
        if (!d->url_len) continue;
        put(",\""); put(d->name); put("\":");
        send(d->answer, d->answer_len);
    }
    put("}\n");
    flush();
    k32_resp_end();
}

/* One row of the page: the label and the reading with its unit, when the device has it */
static void row(const struct device* d, const char* key, const char* label, const char* unit) {
    const char* v;
    uint32_t n = value(d, key, &v);
    if (!n) return;
    put("<tr><td>"); put(label); put("</td><td>");
    send_text(v, n);
    put(" "); put(unit); put("</td></tr>");
    flush();
}

static void answer_page(void) {
    static const char head[] = "<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width\">"
                               "<meta http-equiv=refresh content=5><style>body{font:16px system-ui;margin:2em}td{padding:.2em 1em .2em 0}</style><title>";
    k32_resp_begin(200, "text/html; charset=utf-8", 24);
    send(head, sizeof head - 1);
    put_n(room, room_len);
    put("</title><h1>");
    put_n(room, room_len);
    put("</h1><table>");
    flush();
    for (int i = 0; i < 2; i++) {
        const struct device* d = &devices[i];
        if (!d->url_len) continue;
        const char* v;
        if (value(d, "ok", &v) && v[0] != 't') {
            uint32_t n = value(d, "error", &v);
            put("<tr><td>"); put(d->name); put("</td><td>");
            send_text(v, n);
            put("</td></tr>");
            flush();
            continue;
        }
        row(d, "temperature_c", "Temperature", "&deg;C");
        row(d, "humidity_pct", "Humidity", "%");
        row(d, "pressure_hpa", "Pressure", "hPa");
        row(d, "gas_ohms", "Gas", "&Omega;");
        row(d, "weight_g", "Weight", "g");
    }
    put("</table>");
    if (devices[1].url_len) put("<form method=post action=tare><button>Tare</button></form>");
    flush();
    k32_resp_end();
}

/* ---- exports --------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    int32_t n = k32_env_get("ROOM", 4, room, sizeof room);
    if (n > 0 && n <= (int32_t)sizeof room) room_len = (uint32_t)n;
    char num[8];
    n = k32_env_get("TIMEOUT_MS", 10, num, sizeof num);
    if (n > 0 && n <= (int32_t)sizeof num) {
        uint32_t v = 0;
        for (int32_t i = 0; i < n && num[i] >= '0' && num[i] <= '9'; i++) v = v * 10 + (uint32_t)(num[i] - '0');
        if (v >= 100 && v <= 30000) timeout_ms = v;
    }
    line_len = 0;
    put("room-monitor: "); put_n(room, room_len);
    for (int i = 0; i < 2; i++) {
        struct device* d = &devices[i];
        n = k32_env_get(d->env, slen(d->env), d->url, sizeof d->url);
        if (n > 0 && n <= (int32_t)sizeof d->url) {
            d->url_len = (uint32_t)n;
            while (d->url_len && d->url[d->url_len - 1] == '/') d->url_len--;
            put(", "); put(d->name); put(" "); put_n(d->url, d->url_len);
        }
    }
    if (!devices[0].url_len && !devices[1].url_len) put(": neither ENV_URL nor SCALE_URL is set");
    k32_log(line, line_len);
    line_len = 0;
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    const char* end = r + len;
    const char* path = r;
    while (path < end && *path++ != ' ') {}
    uint32_t plen = 0;
    while (path + plen < end && path[plen] != ' ' && path[plen] != '?' && path[plen] != '\n') plen++;
    while (plen > 1 && path[plen - 1] == '/') plen--;
    line_len = 0;
    if (r[0] == 'P' && ends_with(path, plen, "/tare")) {
        struct device* scale = &devices[1];
        if (!scale->url_len) {
            k32_resp_begin(404, "application/json", 16);
            put("{\"ok\":false,\"error\":\"SCALE_URL is not set\"}\n");
            flush();
            k32_resp_end();
            return 404;
        }
        call(scale, "POST", "/", "{\"action\":\"tare\"}");
        /* The page's form: back to the page */
        if (contains(r, end, "Content-Type: application/x-www-form-urlencoded")) {
            k32_resp_header("Location", 8, "view", 4);
            k32_resp_begin(303, "text/plain", 10);
            k32_resp_end();
            return 303;
        }
        const int32_t status = scale->status > 0 ? scale->status : 502;
        k32_resp_begin(status, "application/json", 16);
        send(scale->answer, scale->answer_len);
        send("\n", 1);
        k32_resp_end();
        return status;
    }
    for (int i = 0; i < 2; i++) if (devices[i].url_len) call(&devices[i], "GET", "/", 0);
    if (ends_with(path, plen, "/view")) answer_page();
    else answer_json();
    return 200;
}
