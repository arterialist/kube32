/* xfer-server: answers GET /bytes?n=<bytes> with an n-byte body in the
 * format of xfer.h (n a multiple of 4, 8 to 16384; anything else is a 400).
 * The checksum also goes out as the header X-Checksum (hex) for clients
 * that read headers. Runs in 16 KiB of linear memory. */
#include "../../include/runtime/k32_app_abi.h"
#include "xfer.h"

#define NOINLINE __attribute__((noinline))
#define CHUNK 2048u

static unsigned char req_buf[512];
static uint32_t chunk[CHUNK / 4];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static NOINLINE void reply_text(int32_t status, const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    k32_resp_begin(status, "text/plain", 10);
    k32_resp_write(s, n);
    k32_resp_end();
}

/* n from "...n=<digits>..." in [q, q + len), or 0 */
static NOINLINE uint32_t param_n(const char* q, uint32_t len) {
    for (uint32_t i = 0; i + 1 < len; i++) {
        if (q[i] == 'n' && q[i + 1] == '=' && (i == 0 || q[i - 1] == '&' || q[i - 1] == '?')) {
            uint32_t v = 0;
            for (uint32_t k = i + 2; k < len && q[k] >= '0' && q[k] <= '9' && v < 1000000u; k++) v = v * 10u + (uint32_t)(q[k] - '0');
            return v;
        }
    }
    return 0;
}

static NOINLINE uint32_t checksum(uint32_t n) {
    uint32_t h = XFER_SEED;
    for (uint32_t i = 0; i < n / 4u - 1u; i++) h = xfer_step(h, xfer_word(n, i));
    return h;
}

static NOINLINE void send_body(uint32_t n, uint32_t sum) {
    uint32_t words = n / 4u - 1u, i = 0;
    while (i < words) {
        uint32_t k = 0;
        for (; k < CHUNK / 4u && i < words; k++, i++) chunk[k] = xfer_word(n, i);
        k32_resp_write(chunk, k * 4u);
    }
    k32_resp_write(&sum, 4);
}

static NOINLINE void hex32(char* out, uint32_t v) {
    for (int i = 7; i >= 0; i--, v >>= 4) out[i] = "0123456789abcdef"[v & 15u];
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    /* "METHOD SP target LF headers LF LF body" */
    uint32_t sp = 0;
    while (sp < len && m[sp] != ' ') sp++;
    uint32_t le = sp + 1;
    while (le < len && m[le] != '\n') le++;
    const char* t = m + sp + 1;
    uint32_t tlen = le > sp + 1 ? le - sp - 1 : 0;
    if (tlen < 6 || t[0] != '/' || t[1] != 'b' || t[2] != 'y' || t[3] != 't' || t[4] != 'e' || t[5] != 's') {
        reply_text(404, "GET /bytes?n=<bytes>\n");
        return 404;
    }
    uint32_t n = param_n(t, tlen);
    if (n < 8u || n > XFER_MAX || (n & 3u)) {
        reply_text(400, "n: a multiple of 4 from 8 to 16384\n");
        return 400;
    }
    uint32_t sum = checksum(n);
    char hex[8];
    hex32(hex, sum);
    k32_resp_begin(200, "application/octet-stream", 24);
    k32_resp_header("X-Checksum", 10, hex, 8);
    send_body(n, sum);
    k32_resp_end();
    return 200;
}
