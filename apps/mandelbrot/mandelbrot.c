/* mandelbrot: one image, rendered a few rows at a time by a pod that moves
 * between nodes (docs/migration.md).
 *
 * The image lives in the pod's linear memory: a 4-bit shade a pixel, and for
 * each row the node that rendered it. Every TICK_MS of pod time a tick
 * renders ROWS_PER_TICK rows. A move freezes the pod between two ticks, and on
 * the next node k32_restored runs in place of k32_init, so the render goes on
 * at the next row and the image gathers rows from every node the pod visits.
 * The rows of a node take the hue of its platform, <os>/<arch> as its Node
 * reports them in kubernetes.io/os and kubernetes.io/arch: orange for
 * freertos/xtensa (ESP32-S3), green for freertos/riscv32 (ESP32-S31), blue
 * for linux/arm64, violet for darwin/arm64, and a hue from the name's hash
 * for any other. The arithmetic is 32-bit fixed point, so every architecture
 * computes the same shades.
 *
 * k32_init and k32_restored ask the API server where the pod runs: its Pod as
 * a Table (the NODE column), then its Node's labels. The answer holds until
 * the next restore. A lookup that fails is tried again from a tick, after
 * 1 s three times and then every 10 s; the rows wait for the first three
 * tries, then render as node "unknown".
 *
 *   GET /           (and any other path) a page with the image, reloaded once
 *                   a second, and the rows each node rendered
 *   GET /image.bmp  the image, an 8-bit BMP with a palette of 16 shades for
 *                   each platform
 *   GET /status     {"rows_done":..,"rows_total":..,"node":..,"os":..,
 *                   "arch":..,"moves":..,"nodes":[..],"rows":".."}; "rows"
 *                   has a character a row: the index of its node in "nodes"
 *                   (0-9, a-z), "." while it waits
 *   GET /restart    starts the image again and answers /status
 *
 * Requests match on the path's last segment, so the pod answers the same
 * under an Ingress prefix (/mandelbrot/image.bmp).
 *
 * Environment:
 *   API_URL        the API server, e.g. http://$(KUBERNETES_SERVICE_HOST):$(KUBERNETES_SERVICE_PORT)
 *   API_TOKEN      bearer token for it, when it has one
 *   POD_NAME       the pod's name (fieldRef metadata.name)
 *   POD_NAMESPACE  its namespace (default default)
 *   WIDTH          image width, 16..1024 (default 256)
 *   HEIGHT         image height, 16..1024 (default 192); fewer rows when the
 *                  memory limit holds fewer
 *   ROWS_PER_TICK  rows a render step draws, 1..64 (default 1)
 *   TICK_MS        pod time between render steps, 10..60000 (default 300)
 *   MAX_ITER       iterations a pixel, 8..1024 (default 128)
 *
 * Needs SERVICE_CALL for the lookup and no hardware, so it moves to any node.
 * The image takes the memory between the module's data and the pod's memory
 * limit: 256x192 needs 24.2 KiB of it, and the pod runs with a 32Ki limit. */
#include "../../include/runtime/k32_app_abi.h"

#define MAX_NODES 12        /* nodes the image remembers; later ones share the last entry */
#define MAX_SLOTS 15        /* platform hues in the palette, after the background slot */
#define FIELD_CAP 24
#define IO_CAP 1024         /* requests, responses, API answers */
#define FRAC 12             /* fixed point: 4.12 */
#define NONE 0xFFu
#define GREY 0xFFFFu

extern unsigned char __heap_base;

/* A node: its name, os, arch and board label, in that order */
typedef struct {
    char field[4][FIELD_CAP + 16];
    uint8_t len[4];
    uint8_t slot;           /* palette slot of its platform, 1..MAX_SLOTS */
    uint16_t rows;
} node_t;
enum { F_NAME, F_OS, F_ARCH, F_BOARD };

static node_t nodes[MAX_NODES], nobody;
static uint32_t node_count;
static uint32_t cur = NONE;           /* the node the pod runs on */
static uint32_t last_node = NONE;     /* the node the last lookup found */
static uint16_t slot_hue[MAX_SLOTS + 1];
static uint32_t slot_count;

/* The image: H bytes of row owners (0 waiting, else node index + 1), then
 * W * H / 2 bytes of shades, two pixels a byte */
static uint8_t* row_node;
static uint8_t* pix;
static uint32_t W, H, max_iter, rows_per_tick, tick_ms;
static int32_t x0, y0, step;

static uint32_t rows_done, renders, moves, restores;
static uint32_t next_due, started_ms, finished_ms;
static int due_set;

static int ident_ok;
static uint32_t ident_fails, ident_retry_at;

static char api[96], auth[160], pod[64], ns[64], url[224];
static uint32_t api_len, auth_len, pod_len, ns_len, url_len;

static char io[IO_CAP];
static uint32_t io_len;
static char line[160];
static uint32_t line_len;

/* ---- text ------------------------------------------------------------------ */
static uint32_t slen(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }

static uint32_t copy(char* dst, uint32_t cap, const char* src, uint32_t n) {
    if (n > cap) n = cap;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    return n;
}

static int same(const char* a, uint32_t an, const char* b) {
    for (uint32_t i = 0; i < an; i++) if (a[i] != b[i]) return 0;
    return b[an] == 0;
}

static uint32_t env(const char* key, char* out, uint32_t cap) {
    const int32_t n = k32_env_get(key, slen(key), out, cap);
    return n <= 0 ? 0 : (uint32_t)n < cap ? (uint32_t)n : cap;
}

static uint32_t env_uint(const char* key, uint32_t dflt, uint32_t lo, uint32_t hi) {
    char b[12];
    const uint32_t n = env(key, b, sizeof b);
    uint32_t v = 0;
    if (!n) return dflt;
    for (uint32_t i = 0; i < n; i++) {
        if (b[i] < '0' || b[i] > '9') return dflt;
        v = v * 10 + (uint32_t)(b[i] - '0');
    }
    return v < lo ? lo : v > hi ? hi : v;
}

typedef void (*sink_t)(const char*, uint32_t);

/* `f` with each %u the next number, %d the next signed one, %x the next as
 * six hex digits and %s the next string (pointer, length) */
static void fmt(sink_t out, const char* f, const uint32_t* a) {
    static const char hex[] = "0123456789abcdef";
    while (*f) {
        uint32_t n = 0;
        while (f[n] && f[n] != '%') n++;
        if (n) { out(f, n); f += n; continue; }
        const char c = f[1];
        f += 2;
        if (c == 's') { out((const char*)(uintptr_t)a[0], a[1]); a += 2; continue; }
        char t[11];
        uint32_t v = *a++, k = 0;
        if (c == 'x') {
            for (int s = 20; s >= 0; s -= 4) t[k++] = hex[v >> s & 15];
        } else {
            if (c == 'd' && (int32_t)v < 0) { out("-", 1); v = 0u - v; }
            do { t[10 - k++] = (char)('0' + v % 10); v /= 10; } while (v);
            out(t + 11 - k, k);
            continue;
        }
        out(t, k);
    }
}

static uint32_t* sarg(uint32_t* a, const char* s, uint32_t n) {
    a[0] = (uint32_t)(uintptr_t)s;
    a[1] = n;
    return a + 2;
}
static uint32_t* bool_arg(uint32_t* a, int v) { return sarg(a, v ? "true" : "false", v ? 4 : 5); }
static uint32_t* node_args(uint32_t* a, const node_t* n) {
    for (int i = 0; i < 4; i++) a = sarg(a, n->field[i], n->len[i]);
    return a;
}

static void lput(const char* s, uint32_t n) { line_len += copy(line + line_len, sizeof line - line_len, s, n); }
static void log_fmt(const char* f, const uint32_t* a) { fmt(lput, f, a); }
static void log_node(uint32_t i) {
    uint32_t a[8];
    if (i != NONE) node_args(a, &nodes[i]), log_fmt(" on %s (%s/%s, %s)", a);
}
static void log_line(void) { k32_log(line, line_len); line_len = 0; }

/* ---- the API server ----------------------------------------------------------- */
/* After the first `pat` in [p, end), or 0 */
static const char* find(const char* p, const char* end, const char* pat) {
    const uint32_t n = slen(pat);
    for (; p + n <= end; p++) {
        uint32_t i = 0;
        while (i < n && p[i] == pat[i]) i++;
        if (i == n) return p + n;
    }
    return 0;
}

/* The end of the JSON string that starts at p, after its opening quote */
static const char* str_end(const char* p, const char* end) {
    while (p < end && *p != '"') p += *p == '\\' ? 2 : 1;
    return p < end ? p : 0;
}

static void url_put(const char* s, uint32_t n) { url_len += copy(url + url_len, sizeof url - url_len, s, n); }

/* GET <API_URL><path>[<namespace>/pods/]<name>, a Table for a pod */
static int32_t api_get(const char* path, const char* name, uint32_t name_len, int pod_table) {
    static const char accept[] = "Accept: application/json;as=Table;v=v1;g=meta.k8s.io\r\n";
    char hdr[sizeof accept + sizeof auth];
    uint32_t hdr_len = pod_table ? copy(hdr, sizeof hdr, accept, sizeof accept - 1) : 0;
    hdr_len += copy(hdr + hdr_len, sizeof hdr - hdr_len, auth, auth_len);
    url_len = 0;
    url_put(api, api_len);
    url_put(path, slen(path));
    if (pod_table) { url_put(ns, ns_len); url_put("/pods/", 6); }
    url_put(name, name_len);
    io_len = 0;
    const int32_t rc = k32_http_call("GET", 3, url, url_len, hdr, hdr_len, 0, 0, (uint8_t*)io, sizeof io, &io_len, 2000);
    /* Only the head of a long answer arrives; what is read here is in it */
    return rc == K32_ERR_TOOBIG ? k32_http_status() : rc;
}

/* The NODE cell of a Pod Table, as [*out, return), or 0 when the pod has none */
static const char* table_node(const char* p, const char* end, const char** out) {
    const char* defs = find(p, end, "\"columnDefinitions\":[");
    const char* rows = defs ? find(defs, end, "\"cells\":[") : 0;
    if (!rows) return 0;
    /* The column named NODE (Node in Kubernetes) */
    uint32_t col = 0;
    for (const char* q = defs;; col++) {
        q = find(q, rows, "\"name\":\"");
        if (!q) return 0;
        uint32_t i = 0;
        while (i < 5 && (q[i] | 32) == "node\""[i]) i++;
        if (i == 5) break;
    }
    for (const char* c = rows;; col--) {
        if (c >= end || *c != '"') return 0;   /* every cell of a Pod Table up to NODE is a string */
        const char* e = str_end(c + 1, end);
        if (!e) return 0;
        if (!col) { *out = c + 1; return c[1] == '<' ? 0 : e; }   /* <none>: not bound */
        c = e + 2;
    }
}

/* The value of label `key` (quoted, with its colon) in a Node's labels into f */
static uint8_t node_label(const char* p, const char* end, const char* key, char* f) {
    const char* l = find(p, end, "\"labels\":{");
    const char* e = l;
    while (e && e < end && *e != '}') e++;
    const char* v = l ? find(l, e, key) : 0;
    const char* ve = v ? str_end(v, e) : 0;
    return ve ? (uint8_t)copy(f, FIELD_CAP + 15, v, (uint32_t)(ve - v)) : 0;
}

static uint32_t fnv(const char* s, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

/* The hue of a platform: fixed for the common ones, from the name otherwise */
static uint16_t platform_hue(const node_t* n) {
    static const char keys[] = "freertos/xtensa\0freertos/riscv32\0linux/arm64\0darwin/arm64\0linux/amd64\0";
    static const uint16_t hues[] = {28, 140, 205, 285, 52};
    char k[2 * FIELD_CAP + 1];
    if (!n->len[F_ARCH]) return GREY;
    uint32_t kn = copy(k, FIELD_CAP, n->field[F_OS], n->len[F_OS]);
    k[kn++] = '/';
    kn += copy(k + kn, FIELD_CAP, n->field[F_ARCH], n->len[F_ARCH]);
    const char* key = keys;
    for (uint32_t i = 0; *key; i++, key += slen(key) + 1)
        if (same(k, kn, key)) return hues[i];
    return (uint16_t)(fnv(k, kn) % 360);
}

/* A node's palette slot: the slot of another node of its platform, or a new one */
static void assign_slot(uint32_t idx) {
    node_t* n = &nodes[idx];
    const uint16_t hue = platform_hue(n);
    for (uint32_t s = 1; s <= slot_count; s++) {
        if (slot_hue[s] == hue) { n->slot = (uint8_t)s; return; }
    }
    if (slot_count == MAX_SLOTS) { n->slot = (uint8_t)(1 + hue % MAX_SLOTS); return; }
    n->slot = (uint8_t)++slot_count;
    slot_hue[n->slot] = hue;
}

static uint32_t node_entry(const char* name, uint32_t len) {
    for (uint32_t i = 0; i < node_count; i++)
        if (nodes[i].len[F_NAME] == len && same(name, len, nodes[i].field[F_NAME])) return i;
    if (node_count == MAX_NODES) return MAX_NODES - 1;
    nodes[node_count].len[F_NAME] = (uint8_t)copy(nodes[node_count].field[F_NAME], FIELD_CAP + 15, name, len);
    return node_count++;
}

/* Where the pod runs: its Pod's NODE, then that Node's labels. 1 when known */
static int identify(void) {
    const char *name = 0, *name_end = 0;
    url_len = 0;
    int32_t rc = api_len && pod_len ? api_get("/api/v1/namespaces/", pod, pod_len, 1) : K32_ERR_INVAL;
    if (rc == 200 && !(name_end = table_node(io, io + io_len, &name))) rc = K32_ERR_NOENT;
    char found[FIELD_CAP + 16];
    const uint32_t found_len = name_end ? copy(found, sizeof found, name, (uint32_t)(name_end - name)) : 0;
    if (rc == 200) rc = api_get("/api/v1/nodes/", found, found_len, 0);
    if (rc != 200) {
        uint32_t a[3];
        sarg(a, url, url_len);
        a[2] = (uint32_t)rc;
        log_fmt("mandelbrot: no node: GET %s: %d", a);
        log_line();
        ident_ok = 0;
        ident_retry_at = (uint32_t)k32_time_ms() + (++ident_fails < 3 ? 1000 : 10000);
        if (ident_fails >= 3) {
            cur = node_entry("unknown", 7);
            if (!nodes[cur].slot) assign_slot(cur);
        }
        return 0;
    }
    const uint32_t idx = node_entry(found, found_len);
    node_t* n = &nodes[idx];
    static const char* const keys[] = {"\"kubernetes.io/os\":\"", "\"kubernetes.io/arch\":\"", "\"kube32.io/board\":\""};
    for (int i = 0; i < 3; i++) n->len[F_OS + i] = node_label(io, io + io_len, keys[i], n->field[F_OS + i]);
    if (!n->slot) assign_slot(idx);
    if (last_node != NONE && idx != last_node) moves++;
    cur = last_node = idx;
    ident_ok = 1;
    ident_fails = 0;
    return 1;
}

static void read_api(void) {
    api_len = env("API_URL", api, sizeof api);
    while (api_len && api[api_len - 1] == '/') api_len--;
    char token[128];
    const uint32_t tn = env("API_TOKEN", token, sizeof token);
    auth_len = 0;
    if (tn) {
        auth_len = copy(auth, sizeof auth, "Authorization: Bearer ", 22);
        auth_len += copy(auth + auth_len, sizeof auth - 2 - auth_len, token, tn);
        auth_len += copy(auth + auth_len, 2, "\r\n", 2);
    }
    pod_len = env("POD_NAME", pod, sizeof pod);
    ns_len = env("POD_NAMESPACE", ns, sizeof ns);
    if (!ns_len) ns_len = copy(ns, sizeof ns, "default", 7);
}

/* ---- the image ---------------------------------------------------------------- */
/* 0 inside the set, else 1..15 by the iterations to escape */
static uint32_t shade(int32_t cr, int32_t ci) {
    int32_t x = 0, y = 0;
    uint32_t i = 0;
    for (; i < max_iter; i++) {
        const int32_t xx = (x * x) >> FRAC, yy = (y * y) >> FRAC;
        if (xx + yy > (4 << FRAC)) break;
        y = ((x * y) >> (FRAC - 1)) + ci;
        x = xx - yy + cr;
    }
    if (i >= max_iter) return 0;
    const uint32_t n = i * 224 / max_iter;
    uint32_t r = 1;
    while (r * r <= n) r++;
    return r;
}

static void render_row(uint32_t row) {
    const int32_t ci = y0 + (int32_t)row * step;
    uint8_t* out = pix + row * (W / 2);
    int32_t cr = x0;
    for (uint32_t x = 0; x < W / 2; x++) {
        const uint32_t a = shade(cr, ci);
        out[x] = (uint8_t)(a << 4 | shade(cr + step, ci));
        cr += 2 * step;
    }
    row_node[row] = (uint8_t)(cur + 1);
    nodes[cur].rows++;
    renders++;
}

/* Clears the image and the rows of each node */
static void reset(void) {
    for (uint32_t i = 0; i < H * (1 + W / 2); i++) row_node[i] = 0;
    for (uint32_t i = 0; i < node_count; i++) nodes[i].rows = 0;
    rows_done = renders = 0;
    due_set = 0;
    finished_ms = 0;
}

/* Palette entry slot * 16 + level as 0xRRGGBB: slot 0 the rows still to come,
 * level 0 the set itself, levels 1..15 the platform's hue from dark to light */
static uint32_t color(uint32_t slot, uint32_t level) {
    if (!slot) return 0x16181cu;
    if (!level) return 0;
    const uint32_t h = slot_hue[slot] % 360;
    const uint32_t v = 64 + level * 191 / 15;
    const uint32_t s = slot_hue[slot] == GREY ? 0 : level <= 10 ? 255 : 255 - (level - 10) * 40;
    /* HSV to RGB: the channels at v, at v less s, and the one between */
    const uint32_t f = (h % 60) * 255 / 60, lo = v * (255 - s) / 255;
    const uint32_t up = lo + (v - lo) * f / 255, down = v - (v - lo) * f / 255;
    static const uint8_t order[6][3] = {{0, 2, 1}, {3, 0, 1}, {1, 0, 2}, {1, 3, 0}, {2, 1, 0}, {0, 1, 3}};
    const uint32_t c[4] = {v, lo, up, down};
    const uint8_t* o = order[h / 60];
    return c[o[0]] << 16 | c[o[1]] << 8 | c[o[2]];
}

/* ---- responses ------------------------------------------------------------------- */
static void flush(void) {
    if (io_len) k32_resp_write(io, io_len);
    io_len = 0;
}
static void put(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (io_len == IO_CAP) flush();
        io[io_len++] = s[i];
    }
}
static void put_le(uint32_t v, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i++) { const char c = (char)(v >> (8 * i)); put(&c, 1); }
}

static void begin(const char* type) {
    k32_resp_begin(200, type, slen(type));
    k32_resp_header("Cache-Control", 13, "no-store", 8);
    io_len = 0;
}
static void end_resp(void) {
    flush();
    k32_resp_end();
}

static void send_bmp(void) {
    const uint32_t stride = (W + 3) & ~3u, data = stride * H;
    /* BITMAPFILEHEADER and BITMAPINFOHEADER: the file's size, the offset of the
     * pixels (1078), top-down rows (negative height), 8 bits a pixel, 256
     * colours; the sizes are set below */
    static const char head[] = "BM\0\0\0\0\0\0\0\0\x36\x04\0\0\x28\0\0\0\0\0\0\0\0\0\0\0\1\0\x08\0\0\0\0\0\0\0\0\0"
                             "\x13\x0b\0\0\x13\x0b\0\0\0\x01\0\0\0\0\0";
    begin("image/bmp");
    put(head, 54);
    const uint32_t sizes[] = {2, 1078 + data, 18, W, 22, (uint32_t)-(int32_t)H, 34, data};
    for (uint32_t i = 0; i < 8; i += 2)
        for (uint32_t b = 0; b < 4; b++) io[sizes[i] + b] = (char)(sizes[i + 1] >> (8 * b));
    for (uint32_t i = 0; i < 256; i++) {
        const uint32_t rgb = color(i >> 4, i & 15);
        put_le((rgb & 0xFF) << 16 | (rgb & 0xFF00) | rgb >> 16, 4);   /* B, G, R, 0 */
    }
    for (uint32_t y = 0; y < H; y++) {
        if (io_len + stride > IO_CAP) flush();
        const uint32_t base = row_node[y] ? (uint32_t)nodes[row_node[y] - 1].slot << 4 : 0;
        const uint8_t* src = pix + y * (W / 2);
        char* dst = io + io_len;
        for (uint32_t x = 0; x < stride; x++) dst[x] = x < W ? (char)(base | (x & 1 ? src[x / 2] & 15 : src[x / 2] >> 4)) : 0;
        io_len += stride;
    }
    end_resp();
}

static const char status_head[] =
    "{\"width\":%u,\"height\":%u,\"max_iter\":%u,\"rows_done\":%u,\"rows_total\":%u,\"elapsed_ms\":%u,"
    "\"moves\":%u,\"restores\":%u,\"renders\":%u,\"done\":%s,\"identified\":%s,"
    "\"node\":\"%s\",\"os\":\"%s\",\"arch\":\"%s\",\"board\":\"%s\",\"nodes\":[";
static const char status_node[] = "%s{\"name\":\"%s\",\"os\":\"%s\",\"arch\":\"%s\",\"board\":\"%s\",\"rows\":%u,\"color\":\"#%x\"}";

static void send_status(void) {
    uint32_t a[24];
    const uint32_t vals[] = {W, H, max_iter, rows_done, H, !due_set ? 0 : (finished_ms ? finished_ms : (uint32_t)k32_time_ms()) - started_ms,
                             moves, restores, renders};
    for (uint32_t i = 0; i < 9; i++) a[i] = vals[i];
    node_args(bool_arg(bool_arg(a + 9, rows_done >= H), ident_ok), cur == NONE ? &nobody : &nodes[cur]);
    begin("application/json");
    fmt(put, status_head, a);
    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t* p = node_args(sarg(a, ",", i ? 1 : 0), &nodes[i]);
        p[0] = nodes[i].rows;
        p[1] = color(nodes[i].slot, 12);
        fmt(put, status_node, a);
    }
    put("],\"rows\":\"", 10);
    for (uint32_t y = 0; y < H; y++) {
        const uint32_t k = row_node[y];
        const char ch = !k ? '.' : k <= 10 ? (char)('0' + k - 1) : (char)('a' + k - 11);
        put(&ch, 1);
    }
    put("\"}", 2);
    end_resp();
}

static const char page_head[] =
    "<!doctype html><meta name=viewport content=\"width=device-width\"><title>mandelbrot</title><base href=\"";
static const char page_tail[] =
    "\"><style>body{background:#111;color:#ccc;font:14px monospace}img{width:min(100%,768px);image-rendering:pixelated}"
    "b{display:inline-block;width:1em;height:1em}td{padding-right:9px}</style>"
    "<img id=i><p id=p></p><table id=t></table><button onclick=\"fetch('restart')\">restart</button>"
    "<script>let l=-1;(function u(){fetch('status').then(r=>r.json()).then(s=>{"
    "p.textContent=`${s.rows_done} of ${s.rows_total} rows${s.done?`, done in ${s.elapsed_ms/1e3} s`:''}; on ${s.node} (${s.os}/${s.arch}); ${s.moves} moves`;"
    "t.innerHTML=s.nodes.map(n=>`<tr><td><b style=background:${n.color}></b><td>${n.name}<td>${n.os}/${n.arch}<td>${n.board}<td>${n.rows} rows`).join('');"
    "if(s.rows_done!=l)i.src='image.bmp?'+(l=s.rows_done)}).catch(_=>0).finally(_=>setTimeout(u,1e3))})()</script>";

/* ---- entry points ------------------------------------------------------------------- */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof io ? (uint32_t)(uintptr_t)io : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    read_api();
    W = env_uint("WIDTH", 256, 16, 1024) & ~1u;
    H = env_uint("HEIGHT", 192, 16, 1024);
    rows_per_tick = env_uint("ROWS_PER_TICK", 1, 1, 64);
    tick_ms = env_uint("TICK_MS", 300, 10, 60000);
    max_iter = env_uint("MAX_ITER", 128, 8, 1024);
    /* The image takes the memory from the module's data up to the pod's limit */
    uint32_t used = 0, limit = 0;
    k32_mem_stats(&used, &limit);
    const uint32_t top = limit && limit < used ? limit : used;
    const uint32_t base = ((uint32_t)(uintptr_t)&__heap_base + 3) & ~3u;
    const uint32_t fit = top > base ? (top - base) / (1 + W / 2) : 0;
    if (H > fit) H = fit;
    row_node = (uint8_t*)(uintptr_t)base;
    pix = row_node + H;
    /* x from -2.25 to 0.75, y centred on the axis, square pixels */
    step = (int32_t)((3 << FRAC) / W);
    x0 = -(9 << FRAC) / 4 + step / 2;
    y0 = -(int32_t)(step * (int32_t)H) / 2 + step / 2;
    reset();
    identify();
    const uint32_t a[] = {W, H, max_iter, rows_per_tick, tick_ms};
    log_fmt("mandelbrot: %ux%u, %u iterations, %u rows every %u ms", a);
    log_node(cur);
    log_line();
    return H ? 0 : 1;
}

K32_EXPORT("k32_restored") int32_t k32_restored(void) {
    const uint32_t from = last_node;
    read_api();
    restores++;
    /* Rows wait for the node: the one in the snapshot is the node the pod left */
    cur = NONE;
    ident_fails = 0;
    identify();
    uint32_t a[8] = {rows_done, H};
    log_fmt("mandelbrot: restored at row %u of %u", a);
    log_node(cur);
    if (from != NONE && from != cur) node_args(a, &nodes[from]), log_fmt(", from %s", a);
    log_line();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    if (!ident_ok && (int32_t)(now - ident_retry_at) >= 0) {
        identify();
        return 0;
    }
    if (cur == NONE || rows_done >= H) return 0;
    if (!due_set) {
        due_set = 1;
        next_due = started_ms = now;
    }
    if ((int32_t)(now - next_due) < 0) return 0;
    next_due += tick_ms;
    /* A slow node or a long call: go on from now instead of rendering a burst */
    if ((int32_t)(now - next_due) > (int32_t)tick_ms) next_due = now + tick_ms;
    for (uint32_t k = 0; k < rows_per_tick && rows_done < H; k++) render_row(rows_done++);
    if (rows_done >= H) {
        finished_ms = now;
        const uint32_t a[] = {H, now - started_ms};
        log_fmt("mandelbrot: done, %u rows in %u ms of pod time", a);
        log_line();
    }
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* m = (const char*)(uintptr_t)ptr;
    /* "METHOD SP target LF ...": the path's directory and last segment */
    uint32_t i = 0;
    while (i < len && m[i] != ' ') i++;
    char base[96];
    uint32_t base_len = 0, seg = 0;
    for (i++; i < len && m[i] != '\n' && m[i] != ' ' && m[i] != '?' && base_len < sizeof base - 2; i++) {
        const char c = m[i];
        if (c != '"' && c != '<' && c != '\\') base[base_len++] = c;   /* it goes into an attribute */
        if (c == '/') seg = base_len;
    }
    const char* last = base + seg;
    const uint32_t last_len = base_len - seg;
    if (same(last, last_len, "image.bmp")) {
        send_bmp();
        return 200;
    }
    const int restart = same(last, last_len, "restart");
    if (restart) reset();
    if (restart || same(last, last_len, "status")) {
        send_status();
        return 200;
    }
    /* Any other path is the page, whose links are relative to it as a
     * directory: /mandelbrot is /mandelbrot/ */
    if (last_len || !base_len) base[base_len++] = '/';
    begin("text/html; charset=utf-8");
    put(page_head, sizeof page_head - 1);
    put(base, base_len);
    put(page_tail, sizeof page_tail - 1);
    end_resp();
    return 200;
}
