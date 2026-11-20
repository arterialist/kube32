/* ledcycle: the board's RGB LED through k32_led_set
 * (docs/peripherals.md#rgb-led).
 *
 * From k32_init on, each tick sets LED 0 to the next color of red, green,
 * blue, white and off, and logs what k32_led_set returned. The LED ends off.
 *
 *   GET /                       {"step":<colors set>,"rc":[<one per color>]}
 *   GET /cycle                  starts the cycle again
 *   GET /set?rgb=<hex>[&i=<n>]  sets LED n (0 by default) to 0xRRGGBB once:
 *                               {"index":n,"rgb":"<hex>","rc":<result>}
 *
 * Colors are at a quarter of full brightness. Needs hardware.kube32.io/led.
 * Built into images/sd/ledcycle.wasm (not embedded); deploy it inline with
 * kube32-cli deploy. */
#include "../../include/runtime/k32_app_abi.h"

#define NOINLINE __attribute__((noinline))

#define COLORS 5

static const char* const names[COLORS] = {"red", "green", "blue", "white", "off"};
static const uint32_t colors[COLORS] = {0x400000u, 0x004000u, 0x000040u, 0x404040u, 0u};

static uint32_t step;
static int32_t rcs[COLORS];
static unsigned char req_buf[512];
static char out[160];
static unsigned out_len;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static NOINLINE void put(const char* s) { while (*s && out_len < sizeof out) out[out_len++] = *s++; }
static NOINLINE void put_int(int32_t v) {
    uint32_t u = (uint32_t)v;
    if (v < 0) { put("-"); u = 0u - u; }
    char d[10];
    int k = 0;
    do { d[k++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (k && out_len < sizeof out) out[out_len++] = d[--k];
}
static NOINLINE void put_hex6(uint32_t v) {
    for (int shift = 20; shift >= 0; shift -= 4) {
        const uint32_t n = (v >> shift) & 0xFu;
        if (out_len < sizeof out) out[out_len++] = (char)(n < 10u ? '0' + n : 'a' + n - 10u);
    }
}
static NOINLINE void reply(void) {
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
}

/* Value of `key` in the query [q, q + len) as a number in `base`; `def` when absent */
static NOINLINE uint32_t query_num(const char* q, uint32_t len, const char* key, uint32_t base, uint32_t def) {
    uint32_t klen = 0;
    while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen < len; i++) {
        if ((i != 0 && q[i - 1] != '&') || q[i + klen] != '=') continue;
        uint32_t j = 0;
        while (j < klen && q[i + j] == key[j]) j++;
        if (j < klen) continue;
        uint32_t n = 0, e = i + klen + 1;
        if (e >= len || q[e] == '&') return def;
        for (; e < len && q[e] != '&'; e++) {
            const char c = q[e];
            const uint32_t d = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
                             : c >= 'a' && c <= 'f' ? (uint32_t)(c - 'a' + 10)
                             : c >= 'A' && c <= 'F' ? (uint32_t)(c - 'A' + 10) : base;
            if (d >= base) return def;
            n = n * base + d;
        }
        return n;
    }
    return def;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    step = 0;
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (step >= COLORS) return 0;
    rcs[step] = k32_led_set(0, colors[step]);
    out_len = 0;
    put("ledcycle: "); put(names[step]);
    put(" 0x"); put_hex6(colors[step]);
    put(" rc "); put_int(rcs[step]);
    k32_log(out, out_len);
    step++;
    return 0;
}

static NOINLINE void status(void) {
    put("{\"step\":"); put_int((int32_t)step);
    put(",\"rc\":[");
    for (uint32_t i = 0; i < step; i++) {
        if (i) put(",");
        put_int(rcs[i]);
    }
    put("]}");
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    out_len = 0;
    /* "METHOD SP target LF ..." */
    uint32_t sp = 0;
    while (sp < len && m[sp] != ' ') sp++;
    const char* t = m + sp + 1;
    uint32_t tlen = 0;
    while (sp + 1 + tlen < len && t[tlen] != '\n' && t[tlen] != ' ') tlen++;
    uint32_t qpos = 0;
    while (qpos < tlen && t[qpos] != '?') qpos++;
    const char* q = qpos < tlen ? t + qpos + 1 : t + tlen;
    const uint32_t qlen = qpos < tlen ? tlen - qpos - 1 : 0;
    if (qpos == 4 && t[1] == 's' && t[2] == 'e' && t[3] == 't') {
        const uint32_t index = query_num(q, qlen, "i", 10, 0);
        const uint32_t rgb = query_num(q, qlen, "rgb", 16, 0) & 0xFFFFFFu;
        const int32_t rc = k32_led_set(index, rgb);
        put("{\"index\":"); put_int((int32_t)index);
        put(",\"rgb\":\""); put_hex6(rgb);
        put("\",\"rc\":"); put_int(rc);
        put("}");
    } else {
        if (qpos == 6 && t[1] == 'c' && t[2] == 'y') {
            step = 0;
            for (uint32_t i = 0; i < COLORS; i++) rcs[i] = 0;
        }
        status();
    }
    reply();
    return 200;
}
