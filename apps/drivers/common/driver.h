/* What every device driver shares (apps/drivers/<name>): the environment the
 * Device controller sets, the driver HTTP API, events on DEVICE_TOPIC and
 * error lines that do not flood the pod's log.
 *
 * A driver pod runs one part for a Device (docs/devices.md):
 *
 *   GET /          200 {"device":..,"driver":..,"node":..,"ok":true,"at":<unix ms, 0 without a clock>,
 *                       "uptime_ms":<node uptime>,"readings":{..}}   (at and uptime_ms: when the reading was taken)
 *                  503 {"device":..,"driver":..,"node":..,"ok":false,"error":".."} while the part does not answer
 *   GET /describe  200 the readings with their units, and the actions
 *   POST /         {"action":"<name>",..}: 200 with the GET / body, 400 {"ok":false,"error":".."}, or 503
 *
 * Each reading and each change of an actuator is published on DEVICE_TOPIC
 * with the GET / body; so is the part going silent. A body longer than an
 * event (K32_MAX_EVENT_BYTES) leaves out "uptime_ms", then "node", then
 * "device" and "driver" (the topic names the device).
 *
 * Environment:
 *   DEVICE_NAME         the Device (default: the driver's name)
 *   NODE_NAME           the node the part is wired to
 *   DEVICE_TOPIC        topic for readings (default device.<DEVICE_NAME>)
 *   DEVICE_INTERVAL_MS  reading period; the pod annotation
 *                       kube32.io/tick-interval sets how often k32_tick runs
 *   I2C_BUS, I2C_ADDR   I2C drivers: bus (default 0) and address in hex,
 *                       "0x77" or "77" (default I2C_DEFAULT_ADDR)
 *   PIN_PIN             GPIO drivers: the GPIO number
 *
 * The driver defines DRIVER (its catalog name), DESCRIBE (the GET /describe
 * body), INTERVAL_MS (the default period), I2C_DEFAULT_ADDR for an I2C part
 * or GPIO_DRIVER for a pin, ACTIONS when it takes any, includes this file
 * and implements:
 *
 *   static void start(void);            k32_init, after the environment is read
 *   static int  poll(uint32_t now);     k32_tick: 1 to be ticked again in 10 ms, else 0
 *   static void put_readings(void);     the members of "readings", with key() and put_*()
 *   static int  act(const char* name, uint32_t n, const char* body, const char* end);
 *                                       ACTIONS, POST /: 0 done, ACT_BAD (act_error says why), ACT_FAILED (the part)
 *
 * Device and node names are Kubernetes names and go into JSON as they are. */
#include "../../../include/runtime/k32_app_abi.h"

#pragma clang diagnostic ignored "-Wunused-function"   /* helpers a driver has no use for */

#define OUT_CAP 320
#define QUIET_MS 60000      /* at most one error line a minute */
#define ACT_BAD 400
#define ACT_FAILED 503
#define NO_PIN 0xFFFFFFFF

static void start(void);
static int poll(uint32_t now);
static void put_readings(void);

static char out[OUT_CAP];       /* the response, event or log line being built */
static uint32_t out_len;
static char dev[64], node[64], topic[64];
static uint32_t dev_len, node_len, topic_len;
static uint32_t interval_ms;
static int state = -1;          /* 1: the last attempt read the part, 0: it failed, -1: not tried yet */
static const char* why = "no reading yet";   /* while state != 1, with why_rc */
static int32_t why_rc;
static int64_t at_unix;
static uint32_t at_uptime, last_now;
static uint32_t quiet_until, held;
static int first, emit_failed, fail_logged;
static uint8_t req[512];

#ifdef I2C_DEFAULT_ADDR
static uint32_t bus, addr = I2C_DEFAULT_ADDR;
#endif
#ifdef GPIO_DRIVER
static uint32_t pin = NO_PIN;
#endif

/* ---- text ----------------------------------------------------------------- */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void put_n(const char* s, uint32_t n) { while (n-- && out_len < OUT_CAP) out[out_len++] = *s++; }
static void put(const char* s) { put_n(s, slen(s)); }

static void put_uint(uint64_t v) {
    char t[20]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put_n(&t[--n], 1);
}

static void put_int(int32_t v) {
    if (v < 0) put("-");
    put_uint(v < 0 ? 0u - (uint32_t)v : (uint32_t)v);
}

/* v / 10^decimals with that many decimals (1 or 2): put_fixed(2143, 2) -> 21.43 */
static void put_fixed(int32_t v, int decimals) {
    const uint32_t div = decimals == 1 ? 10 : 100;
    if (v < 0) { put("-"); v = -v; }
    put_uint((uint32_t)v / div);
    put((uint32_t)v % div < 10 && decimals == 2 ? ".0" : ".");
    put_uint((uint32_t)v % div);
}

static void put_hex(uint32_t v) {
    const char* hex = "0123456789abcdef";
    put("0x");
    put_n(&hex[v >> 4 & 15], 1);
    put_n(&hex[v & 15], 1);
}

/* The next member of "readings" */
static void key(const char* k) {
    put(first ? "\"" : ",\"");
    first = 0;
    put(k);
    put("\":");
}

static void put_bool(int v) { put(v ? "true" : "false"); }

/* Where the part is, for log lines and errors */
static void put_where(void) {
#ifdef I2C_DEFAULT_ADDR
    put(" at "); put_hex(addr); put(" on bus "); put_uint(bus);
#endif
#ifdef GPIO_DRIVER
    if (pin != NO_PIN) { put(" on GPIO "); put_uint(pin); }
#endif
}

static void log_out(void) { k32_log(out, out_len); }

/* Logs the error line in out[] unless one went out less than a minute ago;
 * then it is counted, and the next one says how many were held back.
 * Returns 1 when logged. */
static int log_limited(void) {
    if (quiet_until && (int32_t)(last_now - quiet_until) < 0) { held++; return 0; }
    if (held) { put(" ("); put_uint(held); put(" more held back)"); held = 0; }
    quiet_until = (last_now + QUIET_MS) | 1;
    log_out();
    return 1;
}

#ifdef I2C_DEFAULT_ADDR
/* ---- I2C ------------------------------------------------------------------- */
/* Reads n bytes from register reg: 0, or the error */
static int32_t reg_read(uint8_t reg, void* buf, uint32_t n) {
    int32_t r = esp32_i2c_write(bus, addr, &reg, 1);
    if (r == 1) {
        r = esp32_i2c_read(bus, addr, buf, n);
        if (r == (int32_t)n) return 0;
    }
    return r < 0 ? r : K32_ERR_IO;
}

/* Writes data: the register, then its bytes. 0, or the error */
static int32_t reg_write(const uint8_t* data, uint32_t n) {
    int32_t r = esp32_i2c_write(bus, addr, data, n);
    return r < 0 ? r : (r == (int32_t)n ? 0 : K32_ERR_IO);
}
#endif

/* ---- environment ----------------------------------------------------------- */
/* The value's length, 0 when unset or longer than cap */
static uint32_t env_text(const char* k, char* buf, uint32_t cap) {
    int32_t n = k32_env_get(k, slen(k), buf, cap);
    return n > 0 && n <= (int32_t)cap ? (uint32_t)n : 0;
}

/* A number: decimal (hex when `hex`), or hex after "0x"; `def` when unset or not a number */
static uint32_t env_num(const char* k, uint32_t def, int hex) {
    char v[12];
    uint32_t n = env_text(k, v, sizeof v), i = 0, base = hex ? 16 : 10, r = 0;
    if (!n) return def;
    if (n > 2 && v[1] == 'x') { i = 2; base = 16; }
    for (; i < n; i++) {
        uint32_t c = (uint32_t)(v[i] | 0x20);
        uint32_t d = c - '0' < 10 ? c - '0' : c - 'a' + 10;
        if (d >= base) return def;
        r = r * base + d;
    }
    return r;
}

/* Which of the words ("up\0down\0none\0") the value is, `def` when unset or none */
static int env_word(const char* k, const char* words, int def) {
    char v[12];
    uint32_t n = env_text(k, v, sizeof v);
    for (int i = 0; n && *words; i++, words += slen(words) + 1) {
        uint32_t j = 0;
        while (j < n && words[j] == v[j]) j++;
        if (j == n && !words[j]) return i;
    }
    return def;
}

/* ---- state, events and log lines ------------------------------------------- */
/* Why the part was not read: "no answer at 0x77 on bus 0 (error -19)" */
static void put_why(void) {
    put(why);
    if (state != 0) return;
    put_where();
    if (why_rc) { put(" (error "); put_int(why_rc); put(")"); }
}

/* The GET / body; level 1 leaves out "uptime_ms", level 2 "node" too, level 3 "device" and "driver" too */
static void body(int level) {
    out_len = 0;
    put("{");
    if (level < 3) { put("\"device\":\""); put_n(dev, dev_len); put("\",\"driver\":\"" DRIVER "\","); }
    if (level < 2 && node_len) { put("\"node\":\""); put_n(node, node_len); put("\","); }
    if (state != 1) {
        put("\"ok\":false,\"error\":\""); put_why(); put("\"}");
        return;
    }
    put("\"ok\":true,\"at\":"); put_uint((uint64_t)at_unix);
    if (level < 1) { put(",\"uptime_ms\":"); put_uint(at_uptime); }
    put(",\"readings\":{");
    first = 1;
    put_readings();
    put("}}");
}

/* Publishes the GET / body on DEVICE_TOPIC */
static void publish(void) {
    int level = 0;
    do body(level); while (out_len > K32_MAX_EVENT_BYTES && ++level < 4);
    int32_t r = k32_event_emit(topic, topic_len, out, out_len);
    if (r < 0 && !emit_failed) {
        emit_failed = 1;
        out_len = 0;
        put(DRIVER ": cannot publish on "); put_n(topic, topic_len); put(" (error "); put_int(r);
        put(r == K32_ERR_PERM ? ", no hardware.kube32.io/ipc grant)" : ")");
        log_out();
    }
}

/* A reading taken at `now`: 1 when the part had not answered before. The
 * part answering again is logged when its failure was. */
static int dev_ok(uint32_t now) {
    const int was = state;
    last_now = now;
    state = 1;
    at_unix = k32_unix_time_ms();
    at_uptime = now;
    if (fail_logged) {
        fail_logged = 0;
        out_len = 0;
        put(DRIVER ": answering again"); put_where();
        log_out();
    }
    return was != 1;
}

/* The part did not answer (rc: the error). The first failure after a
 * reading is logged and published. */
static void dev_fail(uint32_t now, const char* what, int32_t rc) {
    const int was = state;
    last_now = now;
    state = 0;
    why = rc == K32_ERR_PERM ? "not granted to this pod" : what;
    why_rc = rc;
    if (was == 0) return;
    out_len = 0;
    put(DRIVER ": "); put_why(); put(", retrying every "); put_uint(interval_ms); put(" ms");
    fail_logged = log_limited();
    publish();
}

/* ---- actions --------------------------------------------------------------- */
#ifdef ACTIONS
static int act(const char* name, uint32_t n, const char* body, const char* end);
static const char* act_error;

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* The value of "k" in the JSON text [s, end): its first byte, or 0 */
static const char* json_at(const char* s, const char* end, const char* k) {
    const uint32_t n = slen(k);
    for (; s + n + 2 < end; s++) {
        uint32_t i = 0;
        while (i < n && s[1 + i] == k[i]) i++;
        if (i < n || s[0] != '"' || s[n + 1] != '"') continue;
        const char* v = s + n + 2;
        while (v < end && (is_space(*v) || *v == ':')) v++;
        return v < end ? v : 0;
    }
    return 0;
}

/* A boolean member: 1, 0, or -1 when absent or not a boolean */
static int json_bool(const char* s, const char* end, const char* k) {
    const char* v = json_at(s, end, k);
    if (!v) return -1;
    if (end - v >= 4 && v[0] == 't' && v[1] == 'r' && v[2] == 'u' && v[3] == 'e') return 1;
    if (end - v >= 5 && v[0] == 'f' && v[1] == 'a' && v[2] == 'l' && v[3] == 's' && v[4] == 'e') return 0;
    return -1;
}

/* An integer member from 0 to max: its value, `def` when absent, -1 when out of range or not a number */
static int32_t json_uint(const char* s, const char* end, const char* k, int32_t max, int32_t def) {
    const char* v = json_at(s, end, k);
    int32_t r = 0, digits = 0;
    if (!v) return def;
    for (; v < end && *v >= '0' && *v <= '9' && r <= max; v++, digits++) r = r * 10 + (*v - '0');
    return digits && r <= max ? r : -1;
}

/* Whether the action's name [name, name + n) is `word` */
static int is(const char* name, uint32_t n, const char* word) {
    uint32_t i = 0;
    while (i < n && name[i] == word[i]) i++;
    return i == n && !word[n];
}

/* POST /: the status; out[] holds the answer */
static int32_t post(const char* b, const char* end) {
    const char* v = json_at(b, end, "action");
    const char* name = v ? v + 1 : end;
    uint32_t n = 0;
    if (v && *v == '"') while (name + n < end && name[n] != '"') n++;
    int rc = ACT_BAD;
    act_error = "the body must name an action";
    if (n) {
        act_error = "no such action (GET /describe lists them)";
        rc = act(name, n, b, end);
    }
    if (rc == ACT_BAD) {
        out_len = 0;
        put("{\"ok\":false,\"error\":\""); put(act_error); put("\"}");
        return 400;
    }
    body(0);
    return rc || state != 1 ? 503 : 200;
}
#endif

/* ---- exports --------------------------------------------------------------- */
K32_EXPORT("k32_init") int32_t k32_init(void) {
    dev_len = env_text("DEVICE_NAME", dev, sizeof dev);
    if (!dev_len) dev_len = slen(DRIVER), __builtin_memcpy(dev, DRIVER, sizeof DRIVER);
    node_len = env_text("NODE_NAME", node, sizeof node);
    topic_len = env_text("DEVICE_TOPIC", topic, sizeof topic);
    if (!topic_len) {
        out_len = 0;
        put("device."); put_n(dev, dev_len);
        topic_len = out_len < sizeof topic ? out_len : sizeof topic;
        __builtin_memcpy(topic, out, topic_len);
    }
    interval_ms = env_num("DEVICE_INTERVAL_MS", 0, 0);
    if (!interval_ms) interval_ms = INTERVAL_MS;
#ifdef I2C_DEFAULT_ADDR
    bus = env_num("I2C_BUS", 0, 0);
    addr = env_num("I2C_ADDR", I2C_DEFAULT_ADDR, 1);
#endif
#ifdef GPIO_DRIVER
    pin = env_num("PIN_PIN", NO_PIN, 0);
#endif
    start();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) { return poll(now); }

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

/* GET / and /describe, POST / (other methods are taken for GET) */
K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    const char* end = r + len;
    const char* path = r;
    while (path < end && *path++ != ' ') {}
    const char* p = path;
    while (p < end && *p != ' ' && *p != '?' && *p != '\n') p++;
    int32_t status = 200;
    if (p - path == 9 && path[1] == 'd') {                       /* /describe */
        out_len = 0;
        put(DESCRIBE);
    } else if (p - path != 1) {
        out_len = 0;
        put("{\"ok\":false,\"error\":\"GET / or /describe, POST /\"}");
        status = 404;
    } else if (r[0] == 'P' && r[1] == 'O') {
#ifdef ACTIONS
        const char* b = p;                                       /* the body follows the empty line */
        while (b + 1 < end && !(b[0] == '\n' && b[1] == '\n')) b++;
        status = post(b + 1 < end ? b + 2 : end, end);
#else
        out_len = 0;
        put("{\"ok\":false,\"error\":\"" DRIVER " takes no actions\"}");
        status = 400;
#endif
    } else {
        body(0);
        status = state == 1 ? 200 : 503;
    }
    put("\n");
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
    return status;
}
