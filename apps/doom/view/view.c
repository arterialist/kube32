// SPDX-License-Identifier: GPL-2.0-or-later
/* doom-view: the Cardputer side of the split DOOM. The sim pod publishes a
 * frame's draw list whole on a channel topic (doom.draw); this pod receives
 * it in k32_event, draws it band by band with the k32_raster_* kernels from
 * the render pack blob (the textures never enter this pod's memory: the
 * kernels read them in place), and puts each band on the screen with
 * k32_display_blit_pal. Every tick it sends the keys the keyboard produced
 * as an event on doom.keys ("d<code>,u<code>,...").
 *
 *   annotation kube32.io/event-topics: doom.draw
 *   env DOOM_KEYS_TOPIC  where the keys go, default doom.keys
 *   env DOOM_PACK_BLOB   the render pack blob (wad.py pack), default doom-pack
 *   GET /stats           frames, bytes and errors as JSON
 *
 * The Cardputer keyboard reports presses, not releases: a key is held for
 * the frame it arrives in and released with the next tick. w a s d move
 * and turn, space uses, f fires, enter and escape pass through. */
#include <stdint.h>
#include "runtime/k32_app_abi.h"
#include "drawlist.h"

#define VIEW_W 240
#define BAND_ROWS 27            /* 5 bands of a 135-row view */
#define LIST_BYTES 12288        /* the sim's cap at 240 wide (Makefile: LIST_BYTES) */
#define HEAD_BYTES 4096

/* the tiny libc clang expects for a freestanding module */
void* memcpy(void* d, const void* s, unsigned long n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)c; return d; }
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

static uint8_t list[LIST_BYTES + 64];   /* the topic comes first, then the message */
static uint8_t band[VIEW_W * BAND_ROWS];
static uint8_t head[HEAD_BYTES];
static uint16_t pal[256];
static dl_ctx ctx;
static char keys_topic[48];
static int32_t blob = -1;
static uint32_t frames, bytes, draw_errors, key_errors, keys_sent, draw_ms, blit_ms;   /* ms spent in the last 100 frames */
static int ready;

/* keys: pressed this tick, released with the next */
static uint8_t release[16];
static uint32_t last_draw_ms, last_blit_ms;
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
    env("DOOM_KEYS_TOPIC", keys_topic, sizeof keys_topic, "doom.keys");
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

/* a draw list arrives whole; the buffer also serves the requests */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof list ? (uint32_t)(uintptr_t)list : 0;
}

static unsigned put_num(char* p, unsigned v);
K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic, uint32_t topic_len, uint32_t ptr, uint32_t len) {
    (void)topic; (void)topic_len;
    const uint8_t* msg = (const uint8_t*)(uintptr_t)ptr;
    if (!ready || len < DL_HEADER_BYTES || msg[0] != 'D') return 0;
    bytes += len;
    const uint32_t vh = msg[8];
    for (uint32_t y0 = 0; y0 < vh; y0 += BAND_ROWS) {
        const uint32_t rows = vh - y0 < BAND_ROWS ? vh - y0 : BAND_ROWS;
        const int64_t t0 = k32_time_ms();
        int32_t rc = dl_draw(&ctx, msg, len, band, VIEW_W, (int32_t)y0, rows);
        const int64_t t1 = k32_time_ms();
        if (rc) { draw_errors++; break; }
        k32_display_blit_pal(0, (int32_t)y0, VIEW_W, (int32_t)rows, band, pal);
        draw_ms += (uint32_t)(t1 - t0);
        blit_ms += (uint32_t)(k32_time_ms() - t1);
    }
    frames++;
    if (frames % 100 == 0) {   /* a line every 100 frames: kubectl logs shows the flow */
        char line[128];
        unsigned n = 0;
        const char* s = "doom-view: frames ";
        while (*s) line[n++] = *s++;
        n += put_num(line + n, frames);
        s = " bytes "; while (*s) line[n++] = *s++;
        n += put_num(line + n, bytes);
        s = " draw_errors "; while (*s) line[n++] = *s++;
        n += put_num(line + n, draw_errors);
        s = " keys_sent "; while (*s) line[n++] = *s++;
        n += put_num(line + n, keys_sent);
        s = " ms/frame draw "; while (*s) line[n++] = *s++;
        n += put_num(line + n, draw_ms / 100);
        s = " blit "; while (*s) line[n++] = *s++;
        n += put_num(line + n, blit_ms / 100);
        k32_log(line, n);
        draw_ms = blit_ms = 0;
    }
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
    /* releases of the last tick's presses, then this tick's presses */
    uint8_t pressed[16];
    unsigned npressed = 0;
    for (int32_t k; (k = k32_key_read()) >= 0 && npressed < sizeof pressed;) {
        uint8_t c = doom_key(k);
        if (c) pressed[npressed++] = c;
    }
    if (nrelease || npressed) {
        char msg[160];
        unsigned n = 0;
        for (unsigned i = 0; i < nrelease; i++) { msg[n++] = 'u'; n += put_num(msg + n, release[i]); msg[n++] = ','; }
        for (unsigned i = 0; i < npressed; i++) { msg[n++] = 'd'; n += put_num(msg + n, pressed[i]); msg[n++] = ','; }
        if (k32_event_emit(keys_topic, slen(keys_topic), msg, n - 1) < 0) key_errors++; else keys_sent += nrelease + npressed;
    }
    memcpy(release, pressed, npressed);
    nrelease = npressed;
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

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr; (void)len;
    out_len = 0;
    put("{");
    put_kv("frames", frames);
    put_kv("bytes", bytes);
    put_kv("draw_errors", draw_errors);
    put_kv("keys_sent", keys_sent);
    put_kv("key_errors", key_errors);
    put_kv("unsupported", ctx.unsupported);
    put_kv("draw_ms_100", draw_ms);
    put_kv("blit_ms_100", blit_ms);
    put("}");
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
    return 200;
}
