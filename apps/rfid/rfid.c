/* RFID reader for MFRC522-compatible 13.56 MHz front ends on I2C (the M5Stack
 * RFID 2 unit's WS1850S answers at 0x28). Every tick it wakes the ISO 14443-A
 * tags in the field, reads the UID of one (4, 7 or 10 bytes) and halts it.
 *
 * A tag that comes into the field is logged, published on the channel topic
 * "rfid.tag" as its UID in hex, and when the environment maps the UID to a
 * message (TAG_<UID>=<text>, or TAG_DEFAULT for tags without an entry of
 * their own), that text is published on the topic named by ACTION_TOPIC. GET
 * answers the reader state and the last tags as JSON.
 *
 * Environment:
 *   I2C_BUS       bus the reader is on, 0 or 1 (default 0); claimed with
 *                 esp32.io/i2c-bus-N
 *   I2C_ADDR      reader address in hex (default 28)
 *   ACTION_TOPIC  topic for the mapped messages (default ir.send)
 *   TAG_<UID>     message for a tag, UID in upper-case hex: TAG_04A1B2C3D4E580
 *   TAG_DEFAULT   message for any other tag */
#include "../../include/runtime/k32_app_abi.h"

#define RECENT 8
#define MISSES_UNTIL_GONE 3

static uint32_t bus, addr = 0x28;
#include "mfrc522.h"

static int reader_ok;
static char action_topic[32] = "ir.send";
static uint32_t action_topic_len = 7;

static struct tag current;
static int have_current, misses;
static struct tag recent[RECENT];
static uint32_t recent_count, tags_seen;
static uint32_t polls, timeouts, errors;   /* frame exchanges per poll outcome */

static uint8_t req_buf[1024];

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static uint32_t env_hex(const char* key, uint32_t def) {
    char v[12];
    int32_t n = k32_env_get(key, slen(key), v, sizeof v);
    if (n <= 0 || n > (int32_t)sizeof v) return def;
    uint32_t r = 0;
    for (int32_t i = 0; i < n; i++) {
        char c = v[i];
        uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                   : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 16;
        if (d > 15) return def;
        r = r * 16 + d;
    }
    return r;
}

/* ---- Output ---- */
struct line { char buf[160]; uint32_t len; };

static void put(struct line* l, const char* s) { while (*s && l->len < sizeof l->buf) l->buf[l->len++] = *s++; }

static void put_hex(struct line* l, const uint8_t* p, uint32_t n, int upper) {
    const char* hex = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (uint32_t i = 0; i < n && l->len + 2 <= sizeof l->buf; i++) {
        l->buf[l->len++] = hex[p[i] >> 4];
        l->buf[l->len++] = hex[p[i] & 15];
    }
}

static void put_uint(struct line* l, uint32_t v) {
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t && l->len < sizeof l->buf) l->buf[l->len++] = tmp[--t];
}

static void log_line(struct line* l) { k32_log(l->buf, l->len); }

static void tag_arrived(const struct tag* t) {
    struct line l = {{0}, 0};
    put(&l, "rfid: tag "); put_hex(&l, t->uid, t->len, 0); put(&l, " sak 0x"); put_hex(&l, &t->sak, 1, 0);
    log_line(&l);

    struct line uid = {{0}, 0};
    put_hex(&uid, t->uid, t->len, 0);
    k32_event_emit("rfid.tag", 8, uid.buf, uid.len);

    /* TAG_<UID>: a message for this tag */
    struct line key = {{0}, 0};
    put(&key, "TAG_"); put_hex(&key, t->uid, t->len, 1);
    key.buf[key.len] = 0;
    char msg[K32_MAX_EVENT_BYTES];
    int32_t n = k32_env_get(key.buf, key.len, msg, sizeof msg);
    if (n <= 0) n = k32_env_get("TAG_DEFAULT", 11, msg, sizeof msg);
    if (n > 0 && n <= (int32_t)sizeof msg) {
        int32_t r = k32_event_emit(action_topic, action_topic_len, msg, (uint32_t)n);
        struct line a = {{0}, 0};
        put(&a, "rfid: "); put(&a, r >= 0 ? "published " : "could not publish "); put(&a, key.buf + 4);
        put(&a, "'s message on "); put(&a, action_topic);
        log_line(&a);
    }

    recent[recent_count % RECENT] = *t;
    recent_count++;
    tags_seen++;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    bus = env_hex("I2C_BUS", 0);
    addr = env_hex("I2C_ADDR", 0x28);
    int32_t n = k32_env_get("ACTION_TOPIC", 12, action_topic, sizeof action_topic - 1);
    if (n > 0 && n < (int32_t)sizeof action_topic) { action_topic[n] = 0; action_topic_len = (uint32_t)n; }
    struct line l = {{0}, 0};
    if (init_reader() == 0) {
        reader_ok = 1;
        put(&l, "rfid: reader at 0x"); put_hex(&l, (const uint8_t*)&addr, 1, 0);
        put(&l, " on bus "); put_uint(&l, bus); put(&l, ", version 0x"); put_hex(&l, &version, 1, 0);
    } else {
        put(&l, "rfid: no reader at 0x"); put_hex(&l, (const uint8_t*)&addr, 1, 0); put(&l, " on bus "); put_uint(&l, bus);
    }
    log_line(&l);
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    if (!reader_ok) {
        if (init_reader() != 0) return 0;      /* plugged in later */
        reader_ok = 1;
        k32_log("rfid: reader found", 18);
    }
    struct tag t;
    int st = read_tag(&t);
    polls++;
    if (st == ST_TIMEOUT) timeouts++;
    else if (st == ST_ERROR || st == ST_COLLISION) errors++;
    if (st == ST_BUS) {
        reader_ok = 0;
        have_current = 0;
        k32_log("rfid: reader stopped answering", 30);
        return 0;
    }
    if (st == ST_OK) {
        misses = 0;
        t.seen_ms = uptime_ms;
        int same = have_current && t.len == current.len;
        for (uint8_t i = 0; same && i < t.len; i++) same = t.uid[i] == current.uid[i];
        current = t;
        if (!same) {
            have_current = 1;
            tag_arrived(&t);
        }
    } else if (have_current && ++misses >= MISSES_UNTIL_GONE) {
        have_current = 0;
        struct line l = {{0}, 0};
        put(&l, "rfid: tag "); put_hex(&l, current.uid, current.len, 0); put(&l, " left");
        log_line(&l);
    }
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

/* GET: {"reader":..,"version":..,"present":..,"seen":..,"polls":..,"empty":..,"errors":..,
 *       "recent":[{"uid":..,"ms":..}]}; "empty" counts polls no tag answered */
K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr; (void)len;
    k32_resp_begin(200, "application/json", 16);
    struct line l = {{0}, 0};
    put(&l, "{\"reader\":"); put(&l, reader_ok ? "true" : "false");
    put(&l, ",\"address\":\"0x"); put_hex(&l, (const uint8_t*)&addr, 1, 0);
    put(&l, "\",\"version\":\"0x"); put_hex(&l, &version, 1, 0);
    put(&l, "\",\"present\":");
    if (have_current) { put(&l, "\""); put_hex(&l, current.uid, current.len, 0); put(&l, "\""); }
    else put(&l, "null");
    k32_resp_write(l.buf, l.len);
    l.len = 0;
    put(&l, ",\"seen\":"); put_uint(&l, tags_seen);
    put(&l, ",\"polls\":"); put_uint(&l, polls);
    put(&l, ",\"empty\":"); put_uint(&l, timeouts);
    put(&l, ",\"errors\":"); put_uint(&l, errors);
    put(&l, ",\"bus_errors\":"); put_uint(&l, bus_errors);
    put(&l, ",\"last_bus_error\":"); put_uint(&l, (uint32_t)(-last_bus_error));
    put(&l, ",\"recent\":[");
    k32_resp_write(l.buf, l.len);
    uint32_t n = recent_count < RECENT ? recent_count : RECENT;
    for (uint32_t i = 0; i < n; i++) {
        const struct tag* t = &recent[(recent_count - 1 - i) % RECENT];
        struct line e = {{0}, 0};
        if (i) put(&e, ",");
        put(&e, "{\"uid\":\""); put_hex(&e, t->uid, t->len, 0); put(&e, "\",\"ms\":"); put_uint(&e, t->seen_ms); put(&e, "}");
        k32_resp_write(e.buf, e.len);
    }
    k32_resp_write("]}\n", 3);
    k32_resp_end();
    return 200;
}
