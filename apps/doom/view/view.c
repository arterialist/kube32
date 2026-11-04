// SPDX-License-Identifier: GPL-2.0-or-later
/* doom-view: the Cardputer side of the split DOOM. Every tick it sends the
 * keys the keyboard produced to the sim pod, fetches the frame's draw list
 * (GET <DOOM_SIM_URL>/drawlist?keys=...), draws it band by band with the
 * k32_raster_* kernels from the render pack blob (the textures never enter
 * this pod's memory: the kernels read them in place), and puts each band on
 * the screen with k32_display_blit_pal.
 *
 *   env DOOM_SIM_URL    the sim pod's draw list, default http://doom-sim/drawlist
 *   env DOOM_PACK_BLOB  the render pack blob (wad.py pack), default doom-pack
 *   GET /stats          frames, bytes and errors as JSON
 *
 * The Cardputer keyboard reports presses, not releases: a key is held for
 * the frame it arrives in and released with the next request. w a s d move
 * and turn, space uses, f or ctrl fires, enter and escape pass through. */
#include <stdint.h>
#include "runtime/k32_app_abi.h"
#include "drawlist.h"

#define VIEW_W 240
#define BAND_ROWS 27            /* 5 bands of a 135-row view */
#define LIST_BYTES 16384
#define HEAD_BYTES 4096

/* the tiny libc clang expects for a freestanding module */
void* memcpy(void* d, const void* s, unsigned long n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)c; return d; }
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

static uint8_t list[LIST_BYTES];
static uint8_t band[VIEW_W * BAND_ROWS];
static uint8_t head[HEAD_BYTES];
static uint16_t pal[256];
static dl_ctx ctx;
static char url[128];
static char req_url[192];
static int32_t blob = -1;
static uint32_t frames, bytes, fetch_errors, draw_errors, last_status;
static int ready;

/* keys: pressed this frame, released with the next request */
static uint8_t pressed[16];
static unsigned npressed;
static uint8_t release[16];
static unsigned nrelease;

static void logf(const char* s) { k32_log(s, slen(s)); }

int32_t dl_host_columns(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, const void* records, uint32_t n, uint32_t colormap,
                        const uint8_t* translation, int32_t centery) {
    return k32_raster_columns(dst, stride, y0, rows, records, n, (const uint8_t*)(uintptr_t)colormap, translation, centery);
}
int32_t dl_host_spans(uint8_t* dst, uint32_t stride, int32_t y0, uint32_t rows, const void* records, uint32_t n, uint32_t colormap) {
    return k32_raster_spans(dst, stride, y0, rows, records, n, (const uint8_t*)(uintptr_t)colormap);
}

static uint32_t env(const char* key, char* out, uint32_t cap, const char* def) {
    int32_t n = k32_env_get(key, slen(key), out, cap - 1);
    if (n <= 0 || n >= (int32_t)cap) { n = (int32_t)slen(def); memcpy(out, def, n); }
    out[n] = 0;
    return (uint32_t)n;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char name[48];
    env("DOOM_SIM_URL", url, sizeof url, "http://doom-sim/drawlist");
    env("DOOM_PACK_BLOB", name, sizeof name, "doom-pack");
    blob = k32_blob_open(name, slen(name));
    if (blob < 0) { logf("doom-view: the render pack blob is not open"); return -1; }
    if (k32_blob_mapped(blob) != 1) { logf("doom-view: the render pack must be a flash blob (mapped)"); return -1; }
    if (k32_blob_read(blob, 0, head, 28) != 28) return -1;
    uint32_t hl = dl_head_len(head);
    if (hl > HEAD_BYTES || k32_blob_read(blob, 0, head, hl) != (int32_t)hl) { logf("doom-view: the pack's tables do not fit"); return -1; }
    if (dl_init(&ctx, (uint32_t)(uintptr_t)K32_BLOB_ADDR(blob, 0), head, hl)) { logf("doom-view: not a version 2 render pack"); return -1; }
    uint8_t rgb[768];
    if (k32_blob_read(blob, ctx.palette, rgb, 768) != 768) return -1;
    for (int i = 0; i < 256; i++) pal[i] = (uint16_t)(((rgb[3 * i] >> 3) << 11) | ((rgb[3 * i + 1] >> 2) << 5) | (rgb[3 * i + 2] >> 3));
    ready = 1;
    return 0;
}

static uint8_t doom_key(int32_t k) {
    switch (k) {
    case 'w': case 'W': return 0xad;   /* KEY_UPARROW */
    case 's': case 'S': return 0xaf;   /* KEY_DOWNARROW */
    case 'a': case 'A': return 0xac;   /* KEY_LEFTARROW */
    case 'd': case 'D': return 0xae;   /* KEY_RIGHTARROW */
    case ' ': return 0xa2;             /* KEY_USE */
    case 'f': case 'F': return 0xa3;   /* KEY_FIRE */
    case '\n': case '\r': return 13;
    case 27: return 27;
    default: return (k > 32 && k < 127) ? (uint8_t)k : 0;
    }
}

static unsigned put_num(char* p, unsigned v) {
    char b[8]; unsigned n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (unsigned i = 0; i < n; i++) p[i] = b[n - 1 - i];
    return n;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (!ready) return 0;
    /* the keys: releases of the last frame's presses, then this frame's presses */
    npressed = 0;
    for (int32_t k; (k = k32_key_read()) >= 0 && npressed < sizeof pressed;) {
        uint8_t c = doom_key(k);
        if (c) pressed[npressed++] = c;
    }
    unsigned n = slen(url);
    memcpy(req_url, url, n);
    if (nrelease || npressed) {
        memcpy(req_url + n, "?keys=", 6); n += 6;
        for (unsigned i = 0; i < nrelease; i++) { req_url[n++] = 'u'; n += put_num(req_url + n, release[i]); req_url[n++] = ','; }
        for (unsigned i = 0; i < npressed; i++) { req_url[n++] = 'd'; n += put_num(req_url + n, pressed[i]); req_url[n++] = ','; }
        n--;
    }
    memcpy(release, pressed, npressed);
    nrelease = npressed;
    uint32_t got = 0;
    int32_t st = k32_http_call("GET", 3, req_url, n, "", 0, (const uint8_t*)"", 0, list, sizeof list, &got, 2000);
    last_status = (uint32_t)st;
    if (st != 200 || got < DL_HEADER_BYTES) { fetch_errors++; return 1; }
    bytes += got;
    const uint32_t vh = list[8];
    for (uint32_t y0 = 0; y0 < vh; y0 += BAND_ROWS) {
        const uint32_t rows = vh - y0 < BAND_ROWS ? vh - y0 : BAND_ROWS;
        int32_t rc = dl_draw(&ctx, list, got, band, VIEW_W, (int32_t)y0, rows);
        if (rc) { draw_errors++; break; }
        k32_display_blit_pal(0, (int32_t)y0, VIEW_W, (int32_t)rows, band, pal);
    }
    frames++;
    return 1;
}

static char out[256];
static unsigned out_len;
static void put(const char* s) { while (*s && out_len < sizeof out - 1) out[out_len++] = *s++; }
static void put_kv(const char* k, uint32_t v) {
    if (out_len > 1) put(",");
    put("\""); put(k); put("\":");
    out_len += put_num(out + out_len, v);
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    static uint8_t req[1024];
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}
K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr; (void)len;
    out_len = 0;
    put("{");
    put_kv("frames", frames);
    put_kv("bytes", bytes);
    put_kv("fetch_errors", fetch_errors);
    put_kv("draw_errors", draw_errors);
    put_kv("last_status", last_status);
    put_kv("unsupported", ctx.unsupported);
    put("}");
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
    return 200;
}
