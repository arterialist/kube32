/* boardinfo: the test pod of tests/test_board_api.cpp. GET /info answers with
 * what k32_display_info and k32_touch_read return.
 *
 * Build: clang --target=wasm32 -Os -nostdlib -ffreestanding -mcpu=mvp -c boardinfo.c -o boardinfo.o
 *        rust-lld -flavor wasm --no-entry --export-dynamic --allow-undefined --stack-first \
 *                 -z stack-size=4096 --initial-memory=65536 --max-memory=65536 -o boardinfo.wasm boardinfo.o */
#include "../../include/runtime/k32_app_abi.h"

static unsigned char req_buf[2048];
static char out[256];
static unsigned out_len;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) { return 0; }

static void put(const char* s) {
    while (*s && out_len < sizeof out) out[out_len++] = *s++;
}

static void put_i(int32_t value) {
    uint32_t v = value < 0 ? (uint32_t)-value : (uint32_t)value;
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    if (value < 0) put("-");
    while (n && out_len < sizeof out) out[out_len++] = d[--n];
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr;
    (void)len;
    int32_t w = 0, h = 0, f = 0;
    const int32_t info = k32_display_info(&w, &h, &f);
    k32_touch_point_t p[2];
    const int32_t touches = k32_touch_read(p, 2);
    out_len = 0;
    put("{\"info\":"); put_i(info);
    put(",\"w\":"); put_i(w);
    put(",\"h\":"); put_i(h);
    put(",\"f\":"); put_i(f);
    put(",\"touches\":"); put_i(touches);
    if (touches > 0) { put(",\"x\":"); put_i(p[0].x); put(",\"y\":"); put_i(p[0].y); put(",\"id\":"); put_i((int32_t)p[0].id); }
    put("}");
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
    return 0;
}
