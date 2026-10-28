/* sdnote: proves pod storage on the microSD card.
 *
 * On k32_init it appends one line to boot.log in its volume
 * (/sd/kube32/volumes/<claim>/boot.log). It serves:
 *   GET /log   the file, as text/plain
 *   GET /ls    the volume's directory listing
 *   DELETE /log removes the file
 * Build: make -C apps/sdnote  -> images/sd/sdnote.wasm; copy that file to the
 * card as /kube32/images/sdnote.wasm and apply examples/sdnote.yaml. */
#include "../../include/runtime/k32_app_abi.h"

static unsigned char scratch[4096];   /* request block from the host */
static unsigned char iobuf[1024];     /* one k32_fs_read / k32_fs_list chunk */
static unsigned boots;

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

static unsigned utoa10(unsigned long v, char* out) {
    char tmp[20]; unsigned t = 0, n = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) out[n++] = tmp[--t];
    return n;
}

static unsigned itoa10(long v, char* out) {
    unsigned n = 0;
    if (v < 0) { out[n++] = '-'; v = -v; }
    return n + utoa10((unsigned long)v, out + n);
}

static void log_rc(const char* what, long rc) {
    char line[64]; unsigned n = slen(what);
    for (unsigned i = 0; i < n; i++) line[i] = what[i];
    n += itoa10(rc, line + n);
    k32_log(line, n);
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    int32_t fd = k32_fs_open("boot.log", 8, K32_FS_O_WRONLY | K32_FS_O_CREAT | K32_FS_O_APPEND);
    if (fd < 0) {
        log_rc("sdnote: open boot.log failed rc=", fd);
        return fd;   /* an honest start failure: the pod exists to use its volume */
    }
    char line[64];
    const char* p = "boot at uptime_ms=";
    unsigned n = slen(p);
    for (unsigned i = 0; i < n; i++) line[i] = p[i];
    n += utoa10((unsigned long)k32_time_ms(), line + n);
    line[n++] = '\n';
    int32_t w = k32_fs_write(fd, line, n);
    k32_fs_close(fd);
    if (w != (int32_t)n) {
        log_rc("sdnote: write boot.log failed rc=", w);
        return w < 0 ? w : K32_ERR_IO;
    }
    boots++;
    log_rc("sdnote: appended to boot.log, bytes=", w);
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    if (size > sizeof scratch) return 0;
    return (uint32_t)(uintptr_t)scratch;
}

static int path_is(const unsigned char* req, uint32_t len, const char* method, const char* path) {
    unsigned ml = slen(method), pl = slen(path);
    if (len < ml + 1 + pl) return 0;
    for (unsigned i = 0; i < ml; i++) if (req[i] != (unsigned char)method[i]) return 0;
    if (req[ml] != ' ') return 0;
    for (unsigned i = 0; i < pl; i++) if (req[ml + 1 + i] != (unsigned char)path[i]) return 0;
    unsigned char end = (len > ml + 1 + pl) ? req[ml + 1 + pl] : '\n';
    return end == '\n' || end == '?' || end == ' ';
}

static int32_t respond_error(int32_t status, const char* what, int32_t rc) {
    char body[96]; unsigned n = slen(what);
    for (unsigned i = 0; i < n; i++) body[i] = what[i];
    n += itoa10(rc, body + n);
    body[n++] = '\n';
    k32_resp_begin(status, "text/plain", 10);
    k32_resp_write(body, n);
    k32_resp_end();
    return status;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const unsigned char* req = (const unsigned char*)(uintptr_t)ptr;

    if (path_is(req, len, "GET", "/log")) {
        int32_t fd = k32_fs_open("boot.log", 8, K32_FS_O_RDONLY);
        if (fd == K32_ERR_NOENT) return respond_error(404, "no boot.log yet rc=", fd);
        if (fd < 0) return respond_error(500, "open failed rc=", fd);
        int32_t size = k32_fs_size(fd);
        k32_resp_begin(200, "text/plain", 10);
        char hdr[16]; unsigned hn = itoa10(size, hdr);
        k32_resp_header("X-File-Size", 11, hdr, hn);
        for (;;) {
            int32_t n = k32_fs_read(fd, iobuf, sizeof iobuf);
            if (n <= 0) break;
            k32_resp_write(iobuf, (uint32_t)n);
        }
        k32_fs_close(fd);
        k32_resp_end();
        return 200;
    }
    if (path_is(req, len, "GET", "/ls")) {
        int32_t n = k32_fs_list("", 0, (char*)iobuf, sizeof iobuf);
        if (n < 0) return respond_error(500, "list failed rc=", n);
        k32_resp_begin(200, "text/plain", 10);
        k32_resp_write(iobuf, (uint32_t)n);
        k32_resp_end();
        return 200;
    }
    if (path_is(req, len, "DELETE", "/log")) {
        int32_t rc = k32_fs_remove("boot.log", 8);
        if (rc < 0) return respond_error(rc == K32_ERR_NOENT ? 404 : 500, "remove failed rc=", rc);
        k32_resp_begin(200, "text/plain", 10);
        k32_resp_write("removed boot.log\n", 17);
        k32_resp_end();
        return 200;
    }
    const char* usage = "sdnote: GET /log, GET /ls, DELETE /log\n";
    k32_resp_begin(404, "text/plain", 10);
    k32_resp_write(usage, slen(usage));
    k32_resp_end();
    return 404;
}
