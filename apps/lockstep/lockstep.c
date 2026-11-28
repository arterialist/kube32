/* lockstep: one lane of a computation that several pods run in lockstep and
 * a voter compares bit for bit (apps/voter).
 *
 * The computation is the double pendulum of pendulum.h: binary64 arithmetic
 * of + - * / only, with its own sin and cos, so every CPU computes the same
 * bits. The lane advances by step number, not by time: each tick runs up to
 * STEPS_PER_TICK steps, never past the target step the voter last answered,
 * so lanes on nodes of different speed stay within a few checkpoints of each
 * other. A lane far behind the target ticks again after 10 ms until it is
 * close, and a lane the vote has passed (it started over, took a copy or came
 * back) until it reaches the target: the lanes in the vote trail the target
 * by up to the voter's LEAD, so a lane that stopped short of the target at
 * their pace would stay behind them. Every CHECK_EVERY steps it records a
 * checkpoint, the step and the four f64 values of the state as their bits,
 * and posts the checkpoints the voter does not hold yet to VOTER_URL, at most
 * every REPORT_MS. The voter answers the target step, the last voted step,
 * the last checkpoint it holds of this lane, and a bit to flip when someone
 * asked it for a fault.
 *
 * The state lives in linear memory and globals, so the lane moves and
 * copies like any request pod (docs/migration.md). k32_restored reads its
 * identity from the environment again, because a Copy brings another lane's
 * memory, and starts a new incarnation: the voter compares it afresh from the
 * step it restored at.
 *
 *   POST /report on the voter    (sent by the lane)
 *   GET  /fault?bit=<0..255>     fault injection: flips one bit of the state
 *                                (bit 0 is the lowest bit of theta1, 63 its
 *                                sign; 64..127 theta2, 128..255 the angular
 *                                velocities) and answers the step and hashes
 *   GET  /status (any other)     {"lane":..,"step":..,"hash":..,...}
 *
 * Environment:
 *   LANE            the lane's name, as the voter lists it (a, b, c)
 *   POD_NAME        the pod's name (fieldRef metadata.name)
 *   NODE_NAME       its node (fieldRef spec.nodeName)
 *   VOTER_URL       where reports go (default http://voter/report)
 *   STEPS_PER_TICK  steps a tick runs at most, 1..1000 (default 50)
 *   CHECK_EVERY     steps between two checkpoints, 10..10000 (default 100)
 *   REPORT_MS       pod time between two reports, 50..10000 (default 400)
 *   FAULT_BIT       the bit /fault flips without ?bit= (default 40)
 *
 * Needs SERVICE_CALL for the reports and no hardware, so it moves to any
 * node and takes a copy of another lane's state. */
#include "../../include/runtime/k32_app_abi.h"
#include "pendulum.h"

K32_IMPORT("esp32_get_time_us") int64_t esp32_get_time_us(void);

#define RING 32             /* checkpoints kept until the voter holds them */
#define BATCH 8             /* checkpoints a report carries at most */
#define IO_CAP 1280
#define NOINLINE __attribute__((noinline))

typedef struct {
    uint32_t step;
    uint64_t bits[4];
} entry_t;

static pendulum_t pend;
static uint32_t step, target;
static uint32_t acked, has_ack;     /* the last checkpoint the voter holds of this incarnation */
static uint32_t behind;             /* the last voted step is past that checkpoint */
static entry_t ring[RING];
static uint32_t pushed;             /* checkpoints recorded since the incarnation began */
static uint32_t inc, from;          /* this incarnation: its id and the step it began at */
static uint32_t per_tick, every, report_ms, fault_default;
static uint32_t last_report, reports, report_errors;
static int32_t last_rc;
static uint32_t us_x16;             /* microseconds a step, times 16, averaged */
static uint32_t fault_step, fault_bit, fault_ms, fault_set;

static char lane[16], pod[64], node[64], url[160];
static uint32_t lane_len, pod_len, node_len, url_len;
static char io[IO_CAP];
static uint32_t io_len;
static char req[1024];
static char answer[160];

/* ---- text ------------------------------------------------------------------ */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static int same(const char* a, uint32_t an, const char* b) {
    for (uint32_t i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return b[an] == 0;
}

static uint32_t env(const char* key, char* out, uint32_t cap) {
    const int32_t n = k32_env_get(key, slen(key), out, cap);
    return n <= 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap;
}

static uint32_t parse_uint(const char* s, uint32_t n, uint32_t dflt) {
    uint32_t v = 0;
    if (!n) return dflt;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return dflt;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    return v;
}

static uint32_t env_uint(const char* key, uint32_t dflt, uint32_t lo, uint32_t hi) {
    char b[12];
    const uint32_t v = parse_uint(b, env(key, b, sizeof b), dflt);
    return v < lo ? lo : v > hi ? hi : v;
}

static void put(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n && io_len < IO_CAP; i++) io[io_len++] = s[i];
}
static void puts_(const char* s) { put(s, slen(s)); }
static void put_u(uint32_t v) {
    char t[10];
    uint32_t k = 0;
    do { t[9 - k++] = (char)('0' + v % 10); v /= 10; } while (v);
    put(t + 10 - k, k);
}
static void put_d(int32_t v) {
    if (v < 0) { put("-", 1); v = -v; }
    put_u((uint32_t)v);
}
static void put_hex(uint64_t v, int digits) {
    static const char hex[] = "0123456789abcdef";
    for (int s = 4 * (digits - 1); s >= 0; s -= 4) put(&hex[(v >> s) & 15], 1);
}

/* ---- the state ---------------------------------------------------------------- */
static void state_bits(uint64_t* out) {
    for (int i = 0; i < 4; i++) {
        union { double d; uint64_t u; } c;
        c.d = pend.v[i];
        out[i] = c.u;
    }
}

static void push(void) {
    entry_t* e = &ring[pushed % RING];
    e->step = step;
    state_bits(e->bits);
    pushed++;
}

static uint32_t unacked(void) {
    uint32_t n = 0;
    for (uint32_t k = pushed > RING ? pushed - RING : 0; k < pushed; k++)
        if (!has_ack || ring[k % RING].step > acked) n++;
    return n;
}

/* Flips bit `bit` of the state's 32 bytes, as Wasm stores them */
static void flip(uint32_t bit) {
    ((unsigned char*)pend.v)[bit >> 3] ^= (unsigned char)(1u << (bit & 7));
    fault_step = step;
    fault_bit = bit;
    fault_ms = (uint32_t)k32_time_ms();
    fault_set = 1;
    io_len = 0;
    puts_("lockstep: lane ");
    put(lane, lane_len);
    puts_(": fault injection flipped bit ");
    put_u(bit);
    puts_(" of the state at step ");
    put_u(step);
    k32_log(io, io_len);
}

/* ---- reports -------------------------------------------------------------------- */
static void line_u(const char* key, uint32_t v) { puts_(key); put_u(v); put("\n", 1); }
static void line_s(const char* key, const char* s, uint32_t n) { puts_(key); put(s, n); put("\n", 1); }

/* The value after "<key> " on a line of the answer, or `dflt` */
static uint32_t answer_value(const char* key, uint32_t n, uint32_t dflt, int* found) {
    const uint32_t kn = slen(key);
    for (uint32_t i = 0; i + kn < n; i++) {
        if (i && answer[i - 1] != '\n') continue;
        uint32_t k = 0;
        while (k < kn && answer[i + k] == key[k]) k++;
        if (k < kn || answer[i + kn] != ' ') continue;
        uint32_t j = i + kn + 1, e = j;
        while (e < n && answer[e] >= '0' && answer[e] <= '9') e++;
        if (found) *found = e > j;
        return parse_uint(answer + j, e - j, dflt);
    }
    if (found) *found = 0;
    return dflt;
}

static NOINLINE void report(uint32_t now) {
    io_len = 0;
    line_s("lane ", lane, lane_len);
    line_s("pod ", pod, pod_len);
    line_s("node ", node, node_len);
    puts_("inc ");
    put_hex(inc, 8);
    put("\n", 1);
    line_u("from ", from);
    line_u("step ", step);
    line_u("us ", us_x16 / 16);
    if (fault_set && now - fault_ms < 120000) {
        puts_("fault ");
        put_u(fault_step);
        put(" ", 1);
        put_u(fault_bit);
        put(" ", 1);
        put_u(now - fault_ms);
        put("\n", 1);
    }
    uint32_t sent = 0;
    for (uint32_t k = pushed > RING ? pushed - RING : 0; k < pushed && sent < BATCH; k++) {
        const entry_t* e = &ring[k % RING];
        if (has_ack && e->step <= acked) continue;
        puts_("c ");
        put_u(e->step);
        for (int i = 0; i < 4; i++) { put(" ", 1); put_hex(e->bits[i], 16); }
        put("\n", 1);
        sent++;
    }
    static const char hdr[] = "Content-Type: text/plain\r\n";
    uint32_t got = 0;
    const int32_t rc = k32_http_call("POST", 4, url, url_len, hdr, sizeof hdr - 1, (const uint8_t*)io, io_len, (uint8_t*)answer,
                                     sizeof answer, &got, 1500);
    last_report = now;
    reports++;
    last_rc = rc;
    if (rc != 200) {
        /* The first error and then every tenth: a voter that moves or restarts is not a flood of lines */
        if (report_errors++ % 10 == 0) {
            io_len = 0;
            puts_("lockstep: lane ");
            put(lane, lane_len);
            puts_(": report to ");
            put(url, url_len);
            puts_(": ");
            put_d(rc);
            k32_log(io, io_len);
        }
        return;
    }
    int found = 0;
    const uint32_t t = answer_value("target", got, target, &found);
    if (found) target = t;
    const uint32_t a = answer_value("ack", got, 0, &found);
    if (found) { acked = a; has_ack = 1; }
    const uint32_t v = answer_value("vote", got, 0, &found);
    behind = found && (!has_ack || acked < v);
    const uint32_t bit = answer_value("fault", got, 0, &found);
    if (found && bit < 256) flip(bit);
}

/* ---- life --------------------------------------------------------------------------- */
static void read_env(void) {
    lane_len = env("LANE", lane, sizeof lane);
    if (!lane_len) lane[lane_len++] = '?';
    pod_len = env("POD_NAME", pod, sizeof pod);
    node_len = env("NODE_NAME", node, sizeof node);
    url_len = env("VOTER_URL", url, sizeof url);
    if (!url_len) {
        static const char dflt[] = "http://voter/report";
        for (url_len = 0; url_len < sizeof dflt - 1; url_len++) url[url_len] = dflt[url_len];
    }
    per_tick = env_uint("STEPS_PER_TICK", 50, 1, 1000);
    every = env_uint("CHECK_EVERY", 100, 10, 10000);
    report_ms = env_uint("REPORT_MS", 400, 50, 10000);
    fault_default = env_uint("FAULT_BIT", 40, 0, 255);
}

/* An incarnation id: the node's random numbers and both of its clocks */
static uint32_t new_inc(void) {
    return (k32_random() ^ (uint32_t)esp32_get_time_us() ^ (uint32_t)k32_unix_time_ms()) | 1;
}

static void log_start(const char* what) {
    io_len = 0;
    puts_("lockstep: lane ");
    put(lane, lane_len);
    puts_(what);
    put_u(step);
    puts_(" on node ");
    put(node, node_len);
    puts_(", incarnation ");
    put_hex(inc, 8);
    k32_log(io, io_len);
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    read_env();
    pendulum_start(&pend);
    step = target = 0;
    pushed = has_ack = behind = fault_set = 0;
    inc = new_inc();
    from = 0;
    push();   /* step 0: every lane starts from the same state */
    log_start(" starts at step ");
    return 0;
}

K32_EXPORT("k32_restored") int32_t k32_restored(void) {
    /* The memory may be another lane's: its name, node and history are not this lane's */
    read_env();
    inc = new_inc();
    from = step;
    pushed = has_ack = behind = fault_set = 0;
    us_x16 = 0;   /* the step time of this node, not the source's */
    if (step % every == 0) push();
    last_report = (uint32_t)k32_time_ms() - 100000;
    log_start(" restored at step ");
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    uint32_t n = target > step ? target - step : 0;
    if (n > per_tick) n = per_tick;
    if (n) {
        const int64_t t0 = esp32_get_time_us();
        for (uint32_t i = 0; i < n; i++) {
            pendulum_step(&pend);
            if (++step % every == 0) push();
        }
        const uint32_t us = (uint32_t)((esp32_get_time_us() - t0) * 16 / n);
        us_x16 = us_x16 ? us_x16 - us_x16 / 8 + us / 8 : us;
    }
    /* New checkpoints and a lane waiting for its target report every
     * REPORT_MS; a lane between checkpoints every 2 s, so the voter sees it */
    const uint32_t since = now - last_report;
    if (since >= 2000 || (since >= report_ms && (unacked() || !n))) report(now);
    /* Far behind the target (a lane that started over), or passed by the vote
     * and short of the target: the next tick in 10 ms instead of at the tick
     * interval */
    return target > step + 4 * per_tick || (behind && target > step) ? 1 : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    /* "METHOD SP target LF ...": the path's last segment and the query */
    uint32_t i = 0;
    while (i < len && m[i] != ' ') i++;
    uint32_t seg = ++i, end = i;
    for (; end < len && m[end] != '\n' && m[end] != ' ' && m[end] != '?'; end++)
        if (m[end] == '/') seg = end + 1;
    uint32_t bit = fault_default;
    int bad = 0;
    if (end < len && m[end] == '?') {
        for (uint32_t q = end + 1; q + 4 <= len && m[q] != ' ' && m[q] != '\n'; q++) {
            if ((q == end + 1 || m[q - 1] == '&') && m[q] == 'b' && m[q + 1] == 'i' && m[q + 2] == 't' && m[q + 3] == '=') {
                uint32_t e = q + 4;
                while (e < len && m[e] != '&' && m[e] != ' ' && m[e] != '\n') e++;
                bit = parse_uint(m + q + 4, e - q - 4, 256);
                bad = bit > 255;
            }
        }
    }
    const int fault = same(m + seg, end - seg, "fault");
    if (fault && bad) {
        static const char msg[] = "{\"error\":\"bit is 0..255\"}";
        k32_resp_begin(400, "application/json", 16);
        k32_resp_write(msg, sizeof msg - 1);
        k32_resp_end();
        return 400;
    }
    const uint32_t hash_before = pendulum_hash(&pend);
    if (fault) flip(bit);
    io_len = 0;
    puts_("{\"lane\":\"");
    put(lane, lane_len);
    puts_("\",\"pod\":\"");
    put(pod, pod_len);
    puts_("\",\"node\":\"");
    put(node, node_len);
    puts_("\",\"step\":");
    put_u(step);
    puts_(",\"target\":");
    put_u(target);
    puts_(",\"hash\":\"");
    put_hex(pendulum_hash(&pend), 8);
    puts_("\",\"inc\":\"");
    put_hex(inc, 8);
    puts_("\",\"from\":");
    put_u(from);
    puts_(",\"us_per_step\":");
    put_u(us_x16 / 16);
    puts_(",\"reports\":");
    put_u(reports);
    puts_(",\"report_errors\":");
    put_u(report_errors);
    puts_(",\"last_rc\":");
    put_d(last_rc);
    if (fault_set) {
        puts_(",\"fault_step\":");
        put_u(fault_step);
        puts_(",\"fault_bit\":");
        put_u(fault_bit);
    }
    if (fault) {
        puts_(",\"hash_before\":\"");
        put_hex(hash_before, 8);
        put("\"", 1);
    }
    uint64_t bits[4];
    state_bits(bits);
    puts_(",\"theta1_bits\":\"");
    put_hex(bits[0], 16);
    puts_("\"}");
    k32_resp_begin(200, "application/json", 16);
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    k32_resp_write(io, io_len);
    k32_resp_end();
    return 200;
}
