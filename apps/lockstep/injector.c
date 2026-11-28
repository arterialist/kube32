/* lockstep-injector: a fault button for one lane of apps/lockstep, and an
 * LED that shows whether the voter (apps/voter) counts that lane with the
 * majority.
 *
 * A press of the button (the boot button of an ESP32-S31 board by default)
 * calls FAULT_URL, the lane's fault-injection endpoint: the lane flips one
 * bit of its state, and the voter outvotes it at its next checkpoint. Every
 * POLL_MS the pod asks the voter for the lane's status and sets the board's
 * RGB LED: green while the lane agrees with the majority, red while it is
 * outvoted, amber while no vote covers it yet (a lane that starts, catches up
 * or has no majority to compare with), blue while the voter does not answer.
 *
 *   GET /press    the same as a press of the button
 *   GET /status   {"lane":..,"status":..,"presses":..,"last_rc":..}
 *
 * Environment:
 *   LANE        the lane, as the voter lists it (default b)
 *   FAULT_URL   default http://lockstep-<LANE>/fault, the lane's Service
 *   STATUS_URL  default http://voter/lane?name=<LANE>
 *   FAULT_BIT   the bit to flip (0..255); without it the lane's own FAULT_BIT
 *   BUTTON_PIN  the button's GPIO (default 61, the ESP32-S31's boot button);
 *               none for no button
 *   ACTIVE      low (default) or high: the level of a pressed button
 *   LED         on (default) or off: without the LED the pod needs no grant
 *               of it and asks the voter nothing
 *   POLL_MS     time between two status reads, 200..10000 (default 500)
 *
 * Grants: esp32.io/gpio-input-<BUTTON_PIN> for the button,
 * hardware.kube32.io/led for the LED, the SERVICE_CALL capability for both
 * calls. */
#include "../../include/runtime/k32_app_abi.h"

#define NOINLINE __attribute__((noinline))

enum { ST_NONE, ST_AGREE, ST_OUT, ST_WAIT, ST_UNREACHABLE };
static const uint32_t colors[] = {0x000000, 0x00300a, 0x400000, 0x302000, 0x000040};
static const char* const names[] = {"none", "agree", "outvoted", "waiting", "unreachable"};

static char lane[16], fault_url[192], status_url[192];
static uint32_t lane_len, fault_url_len, status_url_len;
static int32_t pin = 61;
static uint32_t active_high, led_on = 1, poll_ms;
static uint32_t level_prev, pressed, press_pending, presses, last_poll, shown = 0xFF, state = ST_NONE;
static int32_t last_rc;
static char io[512];
static uint32_t io_len;
static char answer[384];   /* the lane's /fault reply and the voter's verdict */
static char req[1024];

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static uint32_t env(const char* key, char* out, uint32_t cap) {
    const int32_t n = k32_env_get(key, slen(key), out, cap);
    return n <= 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap;
}

static int same(const char* a, uint32_t an, const char* b) {
    for (uint32_t i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return b[an] == 0;
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

static uint32_t cat(char* out, uint32_t at, uint32_t cap, const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n && at < cap; i++) out[at++] = s[i];
    return at;
}

static void put(const char* s, uint32_t n) { io_len = cat(io, io_len, sizeof io, s, n); }
static void puts_(const char* s) { put(s, slen(s)); }
static void put_d(int32_t v) {
    char t[11];
    uint32_t k = 0, u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    do { t[10 - k++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) put("-", 1);
    put(t + 11 - k, k);
}

static void log_io(void) { k32_log(io, io_len); }

static void show(uint32_t st) {
    state = st;
    if (!led_on || st == shown) return;
    shown = st;
    k32_led_set(0, colors[st]);
}

static NOINLINE void press(void) {
    presses++;
    uint32_t got = 0;
    last_rc = k32_http_call("GET", 3, fault_url, fault_url_len, 0, 0, 0, 0, (uint8_t*)answer, sizeof answer, &got, 3000);
    io_len = 0;
    puts_("lockstep-injector: lane ");
    put(lane, lane_len);
    puts_(": fault injection, ");
    put(fault_url, fault_url_len);
    puts_(" answered ");
    put_d(last_rc);
    log_io();
}

static NOINLINE void poll(void) {
    uint32_t got = 0;
    const int32_t rc = k32_http_call("GET", 3, status_url, status_url_len, 0, 0, 0, 0, (uint8_t*)answer, sizeof answer, &got, 2000);
    uint32_t n = 0;
    while (rc == 200 && n < got && answer[n] >= 'a' && answer[n] <= 'z') n++;
    show(rc != 200 ? ST_UNREACHABLE : same(answer, n, "agree") ? ST_AGREE : same(answer, n, "outvoted") ? ST_OUT : ST_WAIT);
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    lane_len = env("LANE", lane, sizeof lane);
    if (!lane_len) lane[lane_len++] = 'b';
    char bit[8];
    const uint32_t bit_len = env("FAULT_BIT", bit, sizeof bit);
    fault_url_len = env("FAULT_URL", fault_url, sizeof fault_url - 16);
    if (!fault_url_len) {
        fault_url_len = cat(fault_url, 0, sizeof fault_url - 16, "http://lockstep-", 16);
        fault_url_len = cat(fault_url, fault_url_len, sizeof fault_url - 16, lane, lane_len);
        fault_url_len = cat(fault_url, fault_url_len, sizeof fault_url - 16, "/fault", 6);
    }
    if (bit_len && parse_uint(bit, bit_len, 256) < 256) {
        fault_url_len = cat(fault_url, fault_url_len, sizeof fault_url, "?bit=", 5);
        fault_url_len = cat(fault_url, fault_url_len, sizeof fault_url, bit, bit_len);
    }
    status_url_len = env("STATUS_URL", status_url, sizeof status_url);
    if (!status_url_len) {
        status_url_len = cat(status_url, 0, sizeof status_url, "http://voter/lane?name=", 23);
        status_url_len = cat(status_url, status_url_len, sizeof status_url, lane, lane_len);
    }
    char v[8];
    uint32_t n = env("BUTTON_PIN", v, sizeof v);
    pin = same(v, n, "none") ? -1 : (int32_t)parse_uint(v, n, 61);
    n = env("ACTIVE", v, sizeof v);
    active_high = same(v, n, "high");
    n = env("LED", v, sizeof v);
    led_on = !same(v, n, "off");
    n = env("POLL_MS", v, sizeof v);
    poll_ms = parse_uint(v, n, 500);
    poll_ms = poll_ms < 200 ? 200 : poll_ms > 10000 ? 10000 : poll_ms;
    if (pin >= 0) {
        const int32_t rc = esp32_gpio_set_mode((uint32_t)pin, active_high ? K32_GPIO_INPUT_PULLDOWN : K32_GPIO_INPUT_PULLUP);
        if (rc < 0) {
            io_len = 0;
            puts_("lockstep-injector: GPIO ");
            put_d(pin);
            puts_(" as the button: ");
            put_d(rc);
            log_io();
            pin = -1;
        }
    }
    level_prev = !active_high;
    show(ST_WAIT);
    io_len = 0;
    puts_("lockstep-injector: lane ");
    put(lane, lane_len);
    puts_(", button GPIO ");
    put_d(pin);
    puts_(", fault ");
    put(fault_url, fault_url_len);
    log_io();
    return 0;
}

K32_EXPORT("k32_restored") int32_t k32_restored(void) {
    shown = 0xFF;   /* the LED of this node is not set yet */
    return k32_init();
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    if (pin >= 0) {
        /* A press is the pressed level read on two ticks in a row */
        const int32_t level = esp32_gpio_read((uint32_t)pin);
        if (level >= 0) {
            const uint32_t down = (uint32_t)level == active_high;
            if (down && level_prev == active_high && !pressed) {
                pressed = 1;
                press_pending = 1;
            }
            if (!down) pressed = 0;
            level_prev = (uint32_t)level;
        }
    }
    if (press_pending) {
        press_pending = 0;
        press();
        last_poll = now - poll_ms + 300;   /* the voter's verdict after its next checkpoint */
    }
    if (led_on && now - last_poll >= poll_ms) {
        last_poll = now;
        poll();
    }
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    uint32_t i = 0;
    while (i < len && m[i] != ' ') i++;
    uint32_t seg = ++i, end = i;
    for (; end < len && m[end] != '\n' && m[end] != ' ' && m[end] != '?'; end++)
        if (m[end] == '/') seg = end + 1;
    const int is_press = same(m + seg, end - seg, "press");
    if (is_press) press_pending = 1;
    io_len = 0;
    puts_("{\"lane\":\"");
    put(lane, lane_len);
    puts_("\",\"status\":\"");
    puts_(names[state]);
    puts_("\",\"presses\":");
    put_d((int32_t)presses);
    puts_(",\"last_rc\":");
    put_d(last_rc);
    puts_(",\"button\":");
    put_d(pin);
    puts_(is_press ? ",\"pressed\":true}" : "}");
    k32_resp_begin(is_press ? 202 : 200, "application/json", 16);
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    k32_resp_write(io, io_len);
    k32_resp_end();
    return is_press ? 202 : 200;
}
