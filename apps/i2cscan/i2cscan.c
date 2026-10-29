/* I2C bus scanner: probes every 7-bit address from 0x08 to 0x77 with a
 * zero-length write and logs the ones a device answers on.
 *
 * Environment:
 *   I2C_BUS        bus to scan, 0 or 1 (default 0); the pod must claim it
 *                  (esp32.io/i2c-bus-N)
 *   SCAN_EVERY_MS  scan again after this many milliseconds; unset or 0 scans
 *                  once and exits
 *
 * Exits 0 after a scan, 1 when the bus cannot be used (not claimed, I/O
 * error). examples/i2cscan.yaml runs it. */
#include "../../include/runtime/k32_app_abi.h"

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

struct line { char buf[96]; uint32_t len; };

static void put(struct line* l, const char* s) {
    while (*s && l->len < sizeof l->buf) l->buf[l->len++] = *s++;
}

static void put_uint(struct line* l, uint32_t v) {
    char tmp[12]; int t = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t && l->len < sizeof l->buf) l->buf[l->len++] = tmp[--t];
}

static void put_int(struct line* l, int32_t v) {
    if (v < 0) { put(l, "-"); put_uint(l, (uint32_t)(-v)); } else put_uint(l, (uint32_t)v);
}

static void put_hex8(struct line* l, uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    put(l, "0x");
    if (l->len + 2 <= sizeof l->buf) { l->buf[l->len++] = hex[(v >> 4) & 15]; l->buf[l->len++] = hex[v & 15]; }
}

static uint32_t env_uint(const char* key, uint32_t def) {
    char v[12];
    int32_t n = k32_env_get(key, slen(key), v, sizeof v - 1);
    if (n <= 0) return def;
    uint32_t r = 0;
    for (int32_t i = 0; i < n && i < (int32_t)sizeof v - 1; i++) {
        if (v[i] < '0' || v[i] > '9') return def;
        r = r * 10 + (uint32_t)(v[i] - '0');
    }
    return r;
}

/* One pass over the bus: the number of devices found, or a negative error */
static int32_t scan(uint32_t bus) {
    int32_t found = 0;
    for (uint32_t addr = 0x08; addr <= 0x77; addr++) {
        int32_t r = esp32_i2c_write(bus, addr, 0, 0);
        if (r == K32_ERR_NODEV) continue;   /* nobody acknowledged */
        struct line l = {{0}, 0};
        if (r < 0) {
            put(&l, "i2cscan: bus "); put_uint(&l, bus); put(&l, ": error "); put_int(&l, r);
            put(&l, " probing "); put_hex8(&l, addr);
            put(&l, r == K32_ERR_PERM ? " (bus not claimed by this pod)" : "");
            k32_log(l.buf, l.len);
            return r;
        }
        found++;
        put(&l, "i2cscan: bus "); put_uint(&l, bus); put(&l, ": device at "); put_hex8(&l, addr);
        k32_log(l.buf, l.len);
    }
    return found;
}

K32_EXPORT("app_main") int32_t app_main(void) {
    const uint32_t bus = env_uint("I2C_BUS", 0);
    const uint32_t every_ms = env_uint("SCAN_EVERY_MS", 0);
    for (;;) {
        int64_t t0 = k32_time_ms();
        int32_t n = scan(bus);
        if (n < 0) return 1;
        struct line l = {{0}, 0};
        put(&l, "i2cscan: bus "); put_uint(&l, bus); put(&l, ": scan complete, ");
        put_uint(&l, (uint32_t)n); put(&l, n == 1 ? " device in " : " devices in ");
        put_uint(&l, (uint32_t)(k32_time_ms() - t0)); put(&l, " ms");
        k32_log(l.buf, l.len);
        if (every_ms == 0) return 0;
        esp32_delay_ms(every_ms);
    }
}
