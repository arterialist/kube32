/* irremote: receives and sends infrared remote codes through the node's RMT
 * channels. A tick/event/request pod: it has no thread of its own and never
 * blocks waiting for a frame.
 *
 * Receiving (esp32.io/rmt-rx): every tick drains the receiver queue. An NEC
 * frame is logged as "irremote: nec addr=0x04 cmd=0x08", a repeat code as
 * "irremote: nec repeat", any other frame as "irremote: raw n=<count>
 * <d1>,<d2>,...". NEC codes and repeats are published on the channel topic
 * ir.received and the last RECENT of them are kept for GET /. Without the
 * claim the first receive returns K32_ERR_PERM: the pod logs that once and
 * only sends.
 *
 * Sending (esp32.io/rmt-tx): a message on the topic ir.send (subscribed
 * with the annotation kube32.io/event-topics), POST /send, and with
 * IR_SEND_EVERY_S set the code IR_SEND_CODE every that many seconds.
 *
 * Messages, request bodies and IR_SEND_CODE are one line of text:
 *   nec <addr> <cmd>     addr 0..0xff (sent with its complement) or
 *                        0x100..0xffff (extended NEC), cmd 0..0xff;
 *                        decimal or 0x hex
 *   nec repeat           the NEC repeat code
 *   raw <d1>,<d2>,...    durations in microseconds, mark first
 * ir.received carries "nec 0x04 0x08" and "nec repeat", so a message taken
 * from it can be sent again as it is.
 *
 * Requests: GET / answers the recent codes as JSON, POST /send transmits
 * the body. examples/irremote.yaml runs it. */
#include "../../include/runtime/k32_app_abi.h"

#define RECENT 8
#define LINE_CAP 200

/* NEC timing in microseconds, and the windows a received duration must fall in */
#define NEC_LEAD_MARK   9000
#define NEC_LEAD_SPACE  4500
#define NEC_REPEAT_SPACE 2250
#define NEC_BIT_MARK    560
#define NEC_ZERO_SPACE  560
#define NEC_ONE_SPACE   1690

enum { CODE_NEC = 1, CODE_REPEAT = 2, CODE_RAW = 3 };

struct code {
    uint32_t t_ms;
    uint16_t addr;
    uint8_t cmd;
    uint8_t kind;
    uint16_t n;    /* durations, for raw frames */
};

static uint16_t g_rx[K32_RMT_MAX_DURATIONS];
static uint16_t g_tx[K32_RMT_MAX_DURATIONS];
static unsigned char g_block[4096];   /* k32_alloc: a request block or an event */
static struct code g_recent[RECENT];
static uint32_t g_recent_next;
static uint32_t g_received, g_sent;
static int g_rx_on = 1;               /* until the first receive says the claim is missing */
static int g_tx_refused, g_publish_refused;
static uint32_t g_every_ms, g_last_send_ms;
static char g_periodic[48];
static uint32_t g_periodic_len;
static char g_line[LINE_CAP];
static uint32_t g_line_len;

/* ---- text ---------------------------------------------------------------- */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static void line_reset(void) { g_line_len = 0; }
static void put(const char* s) { while (*s && g_line_len < LINE_CAP) g_line[g_line_len++] = *s++; }
static void put_n(const char* s, uint32_t n) { for (uint32_t i = 0; i < n && g_line_len < LINE_CAP; i++) g_line[g_line_len++] = s[i]; }

static void put_uint(uint32_t v) {
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t && g_line_len < LINE_CAP) g_line[g_line_len++] = tmp[--t];
}

static void put_int(int32_t v) {
    if (v < 0) { put("-"); put_uint((uint32_t)(-v)); } else put_uint((uint32_t)v);
}

/* 0x followed by 2 hex digits, 4 above 0xff */
static void put_hex(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    int digits = v > 0xff ? 4 : 2;
    put("0x");
    for (int i = digits - 1; i >= 0; i--) {
        if (g_line_len < LINE_CAP) g_line[g_line_len++] = hex[(v >> (4 * i)) & 15];
    }
}

static void log_line(void) { k32_log(g_line, g_line_len); }

/* The code as a message: "nec 0x04 0x08" or "nec repeat" */
static void put_code(const struct code* c) {
    if (c->kind == CODE_REPEAT) { put("nec repeat"); return; }
    if (c->kind == CODE_RAW) { put("raw"); return; }
    put("nec "); put_hex(c->addr); put(" "); put_hex(c->cmd);
}

/* ---- parsing ------------------------------------------------------------- */
struct cursor { const char* p; const char* end; };

static void skip_space(struct cursor* c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n')) c->p++;
}

static int word(struct cursor* c, const char* w) {
    skip_space(c);
    uint32_t n = slen(w);
    if ((uint32_t)(c->end - c->p) < n) return 0;
    for (uint32_t i = 0; i < n; i++) if (c->p[i] != w[i]) return 0;
    if (c->p + n < c->end && c->p[n] != ' ' && c->p[n] != '\t' && c->p[n] != '\r' && c->p[n] != '\n') return 0;
    c->p += n;
    return 1;
}

/* A decimal or 0x hex number up to `max`; 0 when there is none */
static int number(struct cursor* c, uint32_t max, uint32_t* out) {
    skip_space(c);
    uint32_t v = 0, digits = 0, base = 10;
    if (c->end - c->p > 2 && c->p[0] == '0' && (c->p[1] == 'x' || c->p[1] == 'X')) { base = 16; c->p += 2; }
    while (c->p < c->end) {
        char ch = *c->p;
        uint32_t d;
        if (ch >= '0' && ch <= '9') d = (uint32_t)(ch - '0');
        else if (base == 16 && ch >= 'a' && ch <= 'f') d = (uint32_t)(ch - 'a' + 10);
        else if (base == 16 && ch >= 'A' && ch <= 'F') d = (uint32_t)(ch - 'A' + 10);
        else break;
        v = v * base + d;
        if (v > max) return 0;
        digits++;
        c->p++;
    }
    if (!digits) return 0;
    *out = v;
    return 1;
}

/* ---- NEC ------------------------------------------------------------------- */
static int near(uint32_t v, uint32_t lo, uint32_t hi) { return v >= lo && v <= hi; }
static int bit_mark(uint32_t v) { return near(v, 300, 900); }

/* Decodes one received frame into `c`; 0 when it is not NEC */
static int nec_decode(const uint16_t* d, int32_t n, struct code* c) {
    if (n < 3 || !near(d[0], 7000, 11000)) return 0;
    if (n <= 4 && near(d[1], 1700, 2800) && bit_mark(d[2])) {
        c->kind = CODE_REPEAT;
        return 1;
    }
    if (n < 67 || n > 68 || !near(d[1], 3500, 5500)) return 0;
    uint32_t bits = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t mark = d[2 + 2 * i], space = d[3 + 2 * i];
        if (!bit_mark(mark)) return 0;
        if (near(space, 1200, 2100)) bits |= 1u << i;
        else if (!near(space, 300, 900)) return 0;
    }
    if (!bit_mark(d[66])) return 0;
    uint32_t a0 = bits & 0xff, a1 = (bits >> 8) & 0xff, cmd = (bits >> 16) & 0xff, inv = bits >> 24;
    if ((cmd ^ inv) != 0xff) return 0;
    c->kind = CODE_NEC;
    c->addr = (uint16_t)((a0 ^ a1) == 0xff ? a0 : (a0 | (a1 << 8)));
    c->cmd = (uint8_t)cmd;
    return 1;
}

/* NEC frame for addr/cmd into g_tx; the number of durations */
static uint32_t nec_encode(uint32_t addr, uint32_t cmd) {
    uint32_t a = addr > 0xff ? addr : (addr | ((addr ^ 0xff) << 8));
    uint32_t bits = a | (cmd << 16) | ((cmd ^ 0xff) << 24);
    uint32_t n = 0;
    g_tx[n++] = NEC_LEAD_MARK;
    g_tx[n++] = NEC_LEAD_SPACE;
    for (int i = 0; i < 32; i++) {
        g_tx[n++] = NEC_BIT_MARK;
        g_tx[n++] = (bits >> i) & 1 ? NEC_ONE_SPACE : NEC_ZERO_SPACE;
    }
    g_tx[n++] = NEC_BIT_MARK;
    return n;
}

/* ---- sending ------------------------------------------------------------- */
/* Parses a message ("nec 4 8", "nec repeat", "raw 9000,4500,...") into
 * g_tx; the number of durations, or 0 with g_line holding the reason. */
static uint32_t parse_frame(const char* text, uint32_t len, struct code* what) {
    struct cursor c = {text, text + len};
    uint32_t a, b;
    what->kind = CODE_RAW;
    if (word(&c, "nec")) {
        if (word(&c, "repeat")) {
            what->kind = CODE_REPEAT;
            g_tx[0] = NEC_LEAD_MARK; g_tx[1] = NEC_REPEAT_SPACE; g_tx[2] = NEC_BIT_MARK;
            return 3;
        }
        if (!number(&c, 0xffff, &a) || !number(&c, 0xff, &b)) { put("want \"nec <addr> <cmd>\" or \"nec repeat\""); return 0; }
        what->kind = CODE_NEC;
        what->addr = (uint16_t)a;
        what->cmd = (uint8_t)b;
        return nec_encode(a, b);
    }
    if (word(&c, "raw")) {
        uint32_t n = 0;
        for (;;) {
            if (n == K32_RMT_MAX_DURATIONS) { put("more than "); put_uint(K32_RMT_MAX_DURATIONS); put(" durations"); return 0; }
            if (!number(&c, 0xffff, &a) || a == 0) { put("durations are 1..65535 us, separated by commas"); return 0; }
            g_tx[n++] = (uint16_t)a;
            skip_space(&c);
            if (c.p < c.end && *c.p == ',') { c.p++; continue; }
            break;
        }
        skip_space(&c);
        if (c.p != c.end) { put("durations are 1..65535 us, separated by commas"); return 0; }
        what->n = (uint16_t)n;
        return n;
    }
    put("want \"nec <addr> <cmd>\", \"nec repeat\" or \"raw <d1>,<d2>,...\"");
    return 0;
}

static const char* send_error(int32_t r) {
    if (r == K32_ERR_PERM) return " (no esp32.io/rmt-tx claim)";
    if (r == K32_ERR_INVAL) return " (frame too long or a zero duration)";
    return "";
}

/* Sends a message; the esp32_rmt_send result. Logs what went out or why not. */
static int32_t send_text(const char* text, uint32_t len) {
    struct code what = {0, 0, 0, 0, 0};
    line_reset();
    put("irremote: cannot send: ");
    uint32_t n = parse_frame(text, len, &what);
    if (!n) { log_line(); return K32_ERR_INVAL; }
    int32_t r = esp32_rmt_send(g_tx, n);
    line_reset();
    if (r < 0) {
        if (r == K32_ERR_PERM && g_tx_refused++) return r;   /* said once */
        put("irremote: send failed: "); put_int(r); put(send_error(r));
    } else {
        g_sent++;
        put("irremote: sent ");
        if (what.kind == CODE_NEC) { put("nec addr="); put_hex(what.addr); put(" cmd="); put_hex(what.cmd); }
        else if (what.kind == CODE_REPEAT) put("nec repeat");
        else { put("raw n="); put_uint(n); }
    }
    log_line();
    return r;
}

/* ---- receiving ----------------------------------------------------------- */
static void publish(const struct code* c) {
    line_reset();
    put_code(c);
    int32_t r = k32_event_emit("ir.received", 11, g_line, g_line_len);
    if (r == K32_ERR_PERM && !g_publish_refused++) {
        line_reset();
        put("irremote: not publishing on ir.received: no CHANNEL_IPC grant (hardware.kube32.io/ipc)");
        log_line();
    }
}

static void on_frame(const uint16_t* d, int32_t n) {
    struct code c = {0, 0, 0, 0, 0};
    g_received++;
    c.t_ms = (uint32_t)k32_time_ms();
    line_reset();
    put("irremote: ");
    if (nec_decode(d, n, &c)) {
        if (c.kind == CODE_NEC) { put("nec addr="); put_hex(c.addr); put(" cmd="); put_hex(c.cmd); }
        else put("nec repeat");
        log_line();
        publish(&c);
    } else {
        c.kind = CODE_RAW;
        c.n = (uint16_t)n;
        put("raw n="); put_uint((uint32_t)n); put(" ");
        for (int32_t i = 0; i < n; i++) {
            if (g_line_len > LINE_CAP - 10) { put("..."); break; }
            if (i) put(",");
            put_uint(d[i]);
        }
        log_line();
    }
    g_recent[g_recent_next++ % RECENT] = c;
}

/* Drains up to `max` frames; 1 when one was taken */
static int drain(int max) {
    int got = 0;
    while (g_rx_on && max-- > 0) {
        int32_t n = esp32_rmt_receive(g_rx, K32_RMT_MAX_DURATIONS);
        if (n == K32_ERR_AGAIN) break;
        if (n == K32_ERR_PERM) {
            g_rx_on = 0;
            line_reset();
            put("irremote: no esp32.io/rmt-rx claim: not receiving, sending only");
            log_line();
            break;
        }
        if (n < 0) {
            line_reset();
            put("irremote: receive failed: "); put_int(n);
            log_line();
            break;
        }
        on_frame(g_rx, n);
        got = 1;
    }
    return got;
}

/* ---- environment ----------------------------------------------------------- */
static uint32_t env_uint(const char* key, uint32_t def) {
    char v[12];
    int32_t n = k32_env_get(key, slen(key), v, sizeof v - 1);
    if (n <= 0 || n >= (int32_t)sizeof v) return def;
    struct cursor c = {v, v + n};
    uint32_t r;
    return number(&c, 86400, &r) ? r : def;
}

/* ---- exports ------------------------------------------------------------- */
K32_EXPORT("k32_init") int32_t k32_init(void) {
    g_every_ms = env_uint("IR_SEND_EVERY_S", 0) * 1000;
    int32_t n = k32_env_get("IR_SEND_CODE", 12, g_periodic, sizeof g_periodic);
    if (n > 0 && n <= (int32_t)sizeof g_periodic) g_periodic_len = (uint32_t)n;
    else { const char* def = "nec 0x04 0x08"; g_periodic_len = slen(def); for (uint32_t i = 0; i < g_periodic_len; i++) g_periodic[i] = def[i]; }
    drain(RECENT);   /* also finds out whether the pod receives */
    if (g_rx_on) { line_reset(); put("irremote: receiving"); log_line(); }
    if (g_every_ms) {
        line_reset();
        put("irremote: sending \""); put_n(g_periodic, g_periodic_len); put("\" every "); put_uint(g_every_ms / 1000); put(" s");
        log_line();
    }
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    if (size > sizeof g_block) return 0;
    return (uint32_t)(uintptr_t)g_block;
}

/* Returns 1 after taking a frame, so the node ticks again within ~10 ms
 * while a remote is sending; 1 Hz otherwise. */
K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    int busy = drain(RECENT);
    if (g_every_ms && (g_last_send_ms == 0 || uptime_ms - g_last_send_ms >= g_every_ms)) {
        g_last_send_ms = uptime_ms ? uptime_ms : 1;
        send_text(g_periodic, g_periodic_len);
    }
    return busy;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t tptr, uint32_t tlen, uint32_t ptr, uint32_t len) {
    const char* topic = (const char*)(uintptr_t)tptr;
    if (tlen != 7 || topic[0] != 'i' || topic[1] != 'r' || topic[2] != '.' || topic[3] != 's' || topic[4] != 'e' || topic[5] != 'n' ||
        topic[6] != 'd') {
        return 0;
    }
    send_text((const char*)(uintptr_t)ptr, len);
    return 0;
}

static void reply(int32_t status, const char* json) {
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(json, slen(json));
    k32_resp_end();
}

static void reply_line(int32_t status) {
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(g_line, g_line_len);
    k32_resp_end();
}

/* {"received":N,"sent":N,"receiving":true,"codes":[{"t_ms":N,"code":"nec 0x04 0x08"},...]}, newest first */
static void reply_codes(void) {
    k32_resp_begin(200, "application/json", 16);
    line_reset();
    put("{\"received\":"); put_uint(g_received);
    put(",\"sent\":"); put_uint(g_sent);
    put(g_rx_on ? ",\"receiving\":true,\"codes\":[" : ",\"receiving\":false,\"codes\":[");
    k32_resp_write(g_line, g_line_len);
    uint32_t have = g_recent_next < RECENT ? g_recent_next : RECENT;
    for (uint32_t i = 0; i < have; i++) {
        const struct code* c = &g_recent[(g_recent_next - 1 - i) % RECENT];
        line_reset();
        if (i) put(",");
        put("{\"t_ms\":"); put_uint(c->t_ms); put(",\"code\":\""); put_code(c); put("\"");
        if (c->kind == CODE_RAW) { put(",\"n\":"); put_uint(c->n); }
        put("}");
        k32_resp_write(g_line, g_line_len);
    }
    k32_resp_write("]}\n", 3);
    k32_resp_end();
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    const char* end = req + len;
    const char* p = req;
    while (p < end && *p != ' ') p++;
    uint32_t mlen = (uint32_t)(p - req);
    const char* path = p < end ? p + 1 : end;
    const char* q = path;
    while (q < end && *q != ' ' && *q != '?' && *q != '\n') q++;
    uint32_t plen = (uint32_t)(q - path);
    int is_post = mlen == 4 && req[0] == 'P' && req[1] == 'O' && req[2] == 'S' && req[3] == 'T';
    int is_get = mlen == 3 && req[0] == 'G' && req[1] == 'E' && req[2] == 'T';
    int ends_send = plen >= 5 && path[plen - 5] == '/' && path[plen - 4] == 's' && path[plen - 3] == 'e' && path[plen - 2] == 'n' &&
                    path[plen - 1] == 'd';
    if (is_post && ends_send) {
        /* body after the blank line that ends the header block */
        const char* body = end;
        for (const char* s = req; s + 1 < end; s++) {
            if (s[0] == '\n' && s[1] == '\n') { body = s + 2; break; }
        }
        int32_t r = send_text(body, (uint32_t)(end - body));
        line_reset();
        if (r >= 0) { put("{\"sent\":"); put_uint((uint32_t)r); put("}\n"); reply_line(200); return 200; }
        int32_t status = r == K32_ERR_PERM ? 403 : (r == K32_ERR_INVAL ? 400 : 500);
        put("{\"error\":"); put_int(r); put("}\n");
        reply_line(status);
        return status;
    }
    if (is_get) { reply_codes(); return 200; }
    reply(405, "{\"error\":\"GET / or POST /send\"}\n");
    return 405;
}
