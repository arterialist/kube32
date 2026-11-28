/* mosaico-verify: end-to-end Wasm verification pod for ESP-Mosaico CoreBoard +
 * left-slot Camera Module + right-slot Interaction Module.
 *
 * On k32_init() it runs a non-destructive hardware check across:
 *   - 480x480 AMOLED (k32_display_info + status banner)
 *   - Interaction Module: 6x WS2812 RGB LEDs via k32_led_set(), PIR motion,
 *     LDR light level, 2 capacitive/GPIO buttons, 38 kHz NEC IR TX
 *   - Camera Module: k32_camera_info(), real JPEG capture into linear memory
 *     (verifying 0xFF 0xD8 SOI header), and RGB565 preview blit to AMOLED
 *   - Die temperature (esp32_chip_temp_read)
 *
 * HTTP endpoints (on pod port 7000):
 *   GET  /              JSON status of CoreBoard + Interaction + Camera
 *   GET  /snapshot.jpg  Live hardware-encoded JPEG from left-slot Camera
 *   POST /preview       Blit one live camera frame to the 480x480 AMOLED
 *   POST /led           Cycle/set 6x WS2812 LEDs (?rgb=00ff00 or default chase)
 *   POST /ir            Send 38 kHz NEC IR test frame (addr=0x00, cmd=0x10)
 */
#include "../../include/runtime/k32_app_abi.h"

#define JPEG_CAP (192u * 1024u)
static uint8_t jpeg_buf[JPEG_CAP];
static uint8_t req_buf[1024];
static char out_buf[768];
static uint32_t out_len;

static int32_t last_jpeg_size;
static uint8_t last_jpeg_soi_ok;
static int32_t last_preview_rc;
static int32_t last_ir_rc;
static int32_t last_led_rc;
static int32_t disp_w, disp_h, disp_fmt;

static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void put_n(const char* s, uint32_t n) { while (n-- && out_len < sizeof out_buf) out_buf[out_len++] = *s++; }
static void put(const char* s) { put_n(s, slen(s)); }
static void put_uint(uint32_t v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) put_n(&t[--n], 1);
}
static void put_int(int32_t v) {
    if (v < 0) put("-");
    put_uint(v < 0 ? 0u - (uint32_t)v : (uint32_t)v);
}

static uint32_t parse_hex6(const char* s, const char* end, uint32_t def) {
    uint32_t v = 0, d = 0;
    while (s < end && d < 6) {
        char c = *s++;
        uint32_t n;
        if (c >= '0' && c <= '9') n = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') n = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') n = (uint32_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | n;
        d++;
    }
    return d == 6 ? v : def;
}

static int32_t capture_into_buf(uint32_t quality) {
    int32_t sz = k32_camera_capture_jpeg(jpeg_buf, JPEG_CAP, quality);
    last_jpeg_size = sz;
    last_jpeg_soi_ok = (sz >= 4 && jpeg_buf[0] == 0xFF && jpeg_buf[1] == 0xD8) ? 1 : 0;
    return sz;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    (void)k32_display_info(&disp_w, &disp_h, &disp_fmt);

    /* 1. Interaction Module LED test via standard k32_led_set() */
    static const uint32_t kColors[6] = {0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x0080FF, 0x8000FF};
    last_led_rc = 0;
    for (uint32_t i = 0; i < 6; ++i) {
        int32_t rc = k32_led_set(i, kColors[i]);
        if (rc < 0 && last_led_rc == 0) last_led_rc = rc;
    }
    esp32_delay_ms(120);
    for (uint32_t i = 0; i < 6; ++i) (void)k32_led_set(i, 0);

    /* 2. Interaction Module sensors + 38 kHz NEC IR TX */
    k32_interact_inputs_t in = {0};
    int32_t in_rc = k32_interact_read(&in);
    last_ir_rc = k32_interact_ir_send_nec(0x00, 0x10);

    /* 3. Camera Module JPEG capture + AMOLED preview */
    k32_camera_info_t ci = {0};
    int32_t ci_rc = k32_camera_info(&ci);
    int32_t jpg_sz = capture_into_buf(75);
    (void)k32_camera_info(&ci);
    last_preview_rc = k32_camera_preview();

    int32_t temp_mc = 0;
    int32_t temp_rc = esp32_chip_temp_read(&temp_mc);

    out_len = 0;
    put("mosaico-verify: disp="); put_int(disp_w); put("x"); put_int(disp_h);
    put(" interact_rc="); put_int(in_rc);
    put(" pir="); put_uint(in.motion_detected);
    put(" light="); put_uint(in.light_level);
    put(" btns="); put_uint(in.left_pressed); put(","); put_uint(in.right_pressed);
    put(" led_rc="); put_int(last_led_rc);
    put(" ir_rc="); put_int(last_ir_rc);
    put(" cam_rc="); put_int(ci_rc);
    put(" cam="); put_uint(ci.width); put("x"); put_uint(ci.height);
    put(" jpeg="); put_int(jpg_sz);
    put(" soi="); put_uint(last_jpeg_soi_ok);
    put(" prev_rc="); put_int(last_preview_rc);
    put(" temp_mc="); put_int(temp_rc == 0 ? temp_mc : temp_rc);
    k32_log(out_buf, out_len);
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static int path_eq(const char* p, uint32_t n, const char* lit) {
    uint32_t m = slen(lit);
    if (n != m) return 0;
    for (uint32_t i = 0; i < n; ++i) if (p[i] != lit[i]) return 0;
    return 1;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    const char* end = r + len;
    const char* sp1 = r;
    while (sp1 < end && *sp1 != ' ') sp1++;
    const char* uri = sp1 < end ? sp1 + 1 : end;
    const char* uri_end = uri;
    while (uri_end < end && *uri_end != ' ' && *uri_end != '\n') uri_end++;
    const char* qmark = uri;
    while (qmark < uri_end && *qmark != '?') qmark++;
    uint32_t path_len = (uint32_t)(qmark - uri);

    if (path_eq(uri, path_len, "/snapshot.jpg")) {
        int32_t sz = capture_into_buf(75);
        if (sz <= 0) {
            out_len = 0;
            put("{\"ok\":false,\"error\":\"capture failed\",\"code\":"); put_int(sz); put("}\n");
            k32_resp_begin(503, "application/json", 16);
            k32_resp_write(out_buf, out_len);
            k32_resp_end();
            return 503;
        }
        k32_resp_begin(200, "image/jpeg", 10);
        uint32_t off = 0;
        while (off < (uint32_t)sz) {
            uint32_t chunk = (uint32_t)sz - off;
            if (chunk > 4096u) chunk = 4096u;
            k32_resp_write(jpeg_buf + off, chunk);
            off += chunk;
        }
        k32_resp_end();
        return 200;
    }

    if (path_eq(uri, path_len, "/preview")) {
        last_preview_rc = k32_camera_preview();
        out_len = 0;
        put("{\"ok\":"); put(last_preview_rc == 0 ? "true" : "false");
        put(",\"rc\":"); put_int(last_preview_rc); put("}\n");
        int32_t st = last_preview_rc == 0 ? 200 : 503;
        k32_resp_begin(st, "application/json", 16);
        k32_resp_write(out_buf, out_len);
        k32_resp_end();
        return st;
    }

    if (path_eq(uri, path_len, "/led")) {
        uint32_t rgb = 0x00FF80u;
        if (qmark + 5 < uri_end && qmark[1] == 'r' && qmark[2] == 'g' && qmark[3] == 'b' && qmark[4] == '=') {
            rgb = parse_hex6(qmark + 5, uri_end, rgb);
        }
        int32_t rc = 0;
        for (uint32_t i = 0; i < 6; ++i) {
            int32_t r2 = k32_led_set(i, rgb);
            if (r2 < 0) rc = r2;
        }
        last_led_rc = rc;
        out_len = 0;
        put("{\"ok\":"); put(rc == 0 ? "true" : "false");
        put(",\"rc\":"); put_int(rc); put("}\n");
        int32_t st = rc == 0 ? 200 : 503;
        k32_resp_begin(st, "application/json", 16);
        k32_resp_write(out_buf, out_len);
        k32_resp_end();
        return st;
    }

    if (path_eq(uri, path_len, "/ir")) {
        last_ir_rc = k32_interact_ir_send_nec(0x00, 0x10);
        out_len = 0;
        put("{\"ok\":"); put(last_ir_rc == 0 ? "true" : "false");
        put(",\"rc\":"); put_int(last_ir_rc); put("}\n");
        int32_t st = last_ir_rc == 0 ? 200 : 503;
        k32_resp_begin(st, "application/json", 16);
        k32_resp_write(out_buf, out_len);
        k32_resp_end();
        return st;
    }

    /* GET / : full live status JSON */
    k32_interact_inputs_t in = {0};
    int32_t in_rc = k32_interact_read(&in);
    k32_camera_info_t ci = {0};
    int32_t ci_rc = k32_camera_info(&ci);
    int32_t temp_mc = 0;
    int32_t temp_rc = esp32_chip_temp_read(&temp_mc);

    out_len = 0;
    put("{\"ok\":true,\"display\":{\"w\":"); put_int(disp_w); put(",\"h\":"); put_int(disp_h);
    put("},\"chip_temp_mc\":"); put_int(temp_rc == 0 ? temp_mc : temp_rc);
    put(",\"interact\":{\"rc\":"); put_int(in_rc);
    put(",\"left_pressed\":"); put(in.left_pressed ? "true" : "false");
    put(",\"right_pressed\":"); put(in.right_pressed ? "true" : "false");
    put(",\"motion\":"); put(in.motion_detected ? "true" : "false");
    put(",\"light_pct\":"); put_uint(in.light_level);
    put(",\"light_raw\":"); put_int(in.light_raw);
    put(",\"led_count\":"); put_uint(in.led_count);
    put(",\"led_rc\":"); put_int(last_led_rc);
    put(",\"ir_rc\":"); put_int(last_ir_rc);
    put("},\"camera\":{\"rc\":"); put_int(ci_rc);
    put(",\"width\":"); put_uint(ci.width);
    put(",\"height\":"); put_uint(ci.height);
    put(",\"last_jpeg_bytes\":"); put_int(last_jpeg_size);
    put(",\"soi_ok\":"); put(last_jpeg_soi_ok ? "true" : "false");
    put(",\"preview_rc\":"); put_int(last_preview_rc);
    put("}}\n");
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out_buf, out_len);
    k32_resp_end();
    return 200;
}
