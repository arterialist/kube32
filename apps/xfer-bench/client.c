/* xfer-client: measures how fast this pod fetches bodies from xfer-server.
 *
 * For each size in SIZES (default 4096,8192,16384) it makes CALLS (default
 * 200) back-to-back GET URL?n=<size> calls with k32_http_call and checks
 * every body: its size, its trailer against the words received and its
 * trailer against the value xfer.h gives for the size. Three warm-up calls
 * per size go first and are not counted. Per size it logs one line:
 *
 *   xfer n=8192 calls=200 ok=200 wall_ms=... calls_per_s=... call_ms p50=.. p95=.. max=.. min=..
 *        mean=.. over_500ms=..
 *        fail: rc_<error>=.. status=.. size=.. sum=..
 *
 * calls_per_s counts the verification and the tick gaps in the wall time.
 * The first few failures are also logged one by one. Then it logs "xfer
 * done" and stays idle. Calls run from k32_tick, back-to-back for up to
 * 200 ms a tick; the pod returns a positive value to be ticked again 10 ms
 * later. Runs in 24 KiB of linear memory.
 *
 * Environment: URL (default http://xfer-server/bytes), SIZES, CALLS (at most
 * 200), TIMEOUT_MS (default 3000). */
#include "../../include/runtime/k32_app_abi.h"
#include "xfer.h"

#define NOINLINE __attribute__((noinline))
#define MAX_CALLS 200
#define MAX_SIZES 6
#define WARMUP 3
#define SLICE_MS 200

static uint32_t body[XFER_MAX / 4];
static uint16_t lat[MAX_CALLS];
static char url[128];
static char line[320];
static uint32_t line_len;

static uint32_t sizes[MAX_SIZES], n_sizes, calls = MAX_CALLS, timeout_ms = 3000;
static uint32_t url_base_len, url_len;

/* state of the size under test */
static uint32_t cur, done_calls, warm, ok, expect_sum, logged_fail;
static uint32_t t_start, started;
static uint32_t f_rc[8], f_status, f_size, f_sum;
static int finished;

static const int32_t rc_code[8] = {K32_ERR_TIMEOUT, K32_ERR_IO, K32_ERR_NOENT, K32_ERR_TOOBIG,
                                   K32_ERR_PERM, K32_ERR_NOMEM, K32_ERR_CONNREFUSED, 0};
static const char* const rc_name[8] = {"timeout", "io", "noent", "toobig", "perm", "nomem", "refused", "other"};

/* Times are 32-bit milliseconds: the differences stay right across a wrap */
static NOINLINE uint32_t now_ms(void) { return (uint32_t)k32_time_ms(); }

static NOINLINE void put(const char* s) {
    while (*s && line_len < sizeof line - 1) line[line_len++] = *s++;
}
static NOINLINE void putn(int64_t v) {
    char tmp[24];
    int t = 0;
    if (v < 0) { put("-"); v = -v; }
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t && line_len < sizeof line - 1) line[line_len++] = tmp[--t];
}
static NOINLINE void flush(void) {
    k32_log(line, line_len);
    line_len = 0;
}

static NOINLINE uint32_t parse_u(const char* s, uint32_t len, uint32_t* pos) {
    uint32_t v = 0;
    while (*pos < len && (s[*pos] < '0' || s[*pos] > '9')) (*pos)++;
    while (*pos < len && s[*pos] >= '0' && s[*pos] <= '9') v = v * 10u + (uint32_t)(s[(*pos)++] - '0');
    return v;
}

static NOINLINE uint32_t env_u(const char* key, uint32_t key_len, uint32_t def) {
    char v[16];
    int32_t n = k32_env_get(key, key_len, v, sizeof v);
    uint32_t pos = 0;
    return n > 0 ? parse_u(v, (uint32_t)n, &pos) : def;
}

static NOINLINE void set_url(uint32_t n) {
    uint32_t p = url_base_len;
    const char* q = "?n=";
    while (*q) url[p++] = *q++;
    char tmp[12];
    int t = 0;
    do { tmp[t++] = (char)('0' + n % 10u); n /= 10u; } while (n);
    while (t) url[p++] = tmp[--t];
    url[p] = 0;
    url_len = p;
}

static NOINLINE uint32_t expected_sum(uint32_t n) {
    uint32_t h = XFER_SEED;
    for (uint32_t i = 0; i < n / 4u - 1u; i++) h = xfer_step(h, xfer_word(n, i));
    return h;
}

static NOINLINE uint32_t received_sum(uint32_t n) {
    uint32_t h = XFER_SEED;
    for (uint32_t i = 0; i < n / 4u - 1u; i++) h = xfer_step(h, body[i]);
    return h;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char s[64];
    int32_t n = k32_env_get("URL", 3, url, sizeof url - 16);
    if (n <= 0) {
        const char* d = "http://xfer-server/bytes";
        n = 0;
        while (d[n]) { url[n] = d[n]; n++; }
    }
    url_base_len = (uint32_t)n;
    n = k32_env_get("SIZES", 5, s, sizeof s);
    if (n <= 0) {
        sizes[0] = 4096; sizes[1] = 8192; sizes[2] = 16384; n_sizes = 3;
    } else {
        uint32_t pos = 0;
        while (pos < (uint32_t)n && n_sizes < MAX_SIZES) {
            uint32_t v = parse_u(s, (uint32_t)n, &pos);
            if (v >= 8 && v <= XFER_MAX && !(v & 3u)) sizes[n_sizes++] = v;
        }
    }
    calls = env_u("CALLS", 5, MAX_CALLS);
    if (calls == 0 || calls > MAX_CALLS) calls = MAX_CALLS;
    timeout_ms = env_u("TIMEOUT_MS", 10, 3000);
    put("xfer start sizes="); putn(n_sizes); put(" calls="); putn(calls); put(" timeout_ms="); putn(timeout_ms);
    flush();
    return 0;
}

static NOINLINE void begin_size(void) {
    uint32_t n = sizes[cur];
    set_url(n);
    expect_sum = expected_sum(n);
    done_calls = warm = ok = logged_fail = 0;
    for (int i = 0; i < 8; i++) f_rc[i] = 0;
    f_status = f_size = f_sum = 0;
    t_start = started = 0;
}

static NOINLINE void sort16(uint16_t* a, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        uint16_t v = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > v) { a[j] = a[j - 1]; j--; }
        a[j] = v;
    }
}

static NOINLINE void report_size(void) {
    uint32_t wall = now_ms() - t_start;
    sort16(lat, done_calls);
    uint32_t n = done_calls;
    put("xfer n="); putn(sizes[cur]);
    put(" calls="); putn(n); put(" ok="); putn(ok);
    put(" wall_ms="); putn(wall);
    /* calls a second with two decimals */
    uint32_t cps100 = wall > 0 ? n * 100000u / wall : 0;
    put(" calls_per_s="); putn(cps100 / 100); put("."); if (cps100 % 100 < 10) put("0"); putn(cps100 % 100);
    put(" call_ms p50="); putn(lat[n / 2]);
    put(" p95="); putn(lat[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1]);
    put(" max="); putn(lat[n - 1]);
    put(" min="); putn(lat[0]);
    uint32_t slow = 0, mean = 0;
    for (uint32_t i = 0; i < n; i++) { mean += lat[i]; slow += lat[i] >= 500; }
    put(" mean="); putn(mean / n); put(" over_500ms="); putn(slow);
    put(" fail:");
    for (int i = 0; i < 8; i++) { put(" rc_"); put(rc_name[i]); put("="); putn(f_rc[i]); }
    put(" status="); putn(f_status); put(" size="); putn(f_size); put(" sum="); putn(f_sum);
    flush();
}

static NOINLINE void log_fail(const char* kind, int32_t rc, uint32_t got, uint32_t ms) {
    if (logged_fail >= 8) return;
    logged_fail++;
    put("xfer fail n="); putn(sizes[cur]); put(" call="); putn(done_calls); put(" kind="); put(kind);
    put(" rc="); putn(rc); put(" status="); putn(k32_http_status()); put(" len="); putn(got); put(" ms="); putn(ms);
    flush();
}

/* One call; returns 1 when it counted */
static NOINLINE int one_call(void) {
    uint32_t n = sizes[cur], got = 0;
    uint32_t t0 = now_ms();
    int32_t rc = k32_http_call("GET", 3, url, url_len, "", 0, 0, 0,
                               (uint8_t*)body, XFER_MAX, &got, timeout_ms);
    uint32_t ms = now_ms() - t0;
    if (warm < WARMUP) { warm++; return 0; }
    if (!started) { t_start = t0; started = 1; }
    lat[done_calls] = (uint16_t)(ms > 65535 ? 65535 : ms);
    if (rc < 0) {
        int k = 7;
        for (int i = 0; i < 7; i++) if (rc_code[i] == rc) k = i;
        f_rc[k]++;
        log_fail(rc_name[k], rc, got, ms);
    } else if (rc != 200) {
        f_status++;
        log_fail("status", rc, got, ms);
    } else if (got != n) {
        f_size++;
        log_fail("size", rc, got, ms);
    } else if (body[n / 4u - 1u] != expect_sum || received_sum(n) != expect_sum) {
        f_sum++;
        log_fail("sum", rc, got, ms);
    } else {
        ok++;
    }
    done_calls++;
    return 1;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (finished) return 0;
    if (cur >= n_sizes) {
        put("xfer done"); flush();
        finished = 1;
        return 0;
    }
    if (done_calls == 0 && warm == 0) begin_size();
    uint32_t t0 = now_ms();
    while (done_calls < calls && now_ms() - t0 < SLICE_MS) one_call();
    if (done_calls >= calls) {
        report_size();
        cur++;
        done_calls = 0;
        warm = 0;
    }
    return 1;
}
