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
 * The Cardputer keyboard reports presses, not releases: a key is released
 * with the next tick (the sim keeps it down for a few tics at least). w a
 * s d move and turn, space uses, f fires, enter and escape pass through.
 *
 * Along the bottom it draws the status bar's numbers the sim appends to
 * each draw list: the ammo of the weapon up, health, armor, and the keys.
 * In the hunt (port/hunt.c) a record after them names pods: a line along the
 * top, and a tag over each demon in sight. */
#include <stdint.h>
#include "runtime/k32_app_abi.h"
#include "drawlist.h"

#define VIEW_W 240
#define BAND_ROWS 27            /* 5 bands of a 135-row view */
#define LIST_BYTES 12288        /* the sim's cap at 240 wide (Makefile: LIST_BYTES) */
#define NAMES_BYTES 256        /* the hunt's record (port/hunt.h: HUNT_RECORD_MAX) */
#define HEAD_BYTES 4096

/* the tiny libc clang expects for a freestanding module */
void* memcpy(void* d, const void* s, unsigned long n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }
void* memset(void* d, int c, unsigned long n) { uint8_t* a = d; while (n--) *a++ = (uint8_t)c; return d; }
static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }

static uint8_t list[LIST_BYTES + NAMES_BYTES + 64];   /* the topic comes first, then the message */
static uint8_t band[VIEW_W * BAND_ROWS];
static uint8_t head[HEAD_BYTES];
static uint16_t pal[256];
static dl_ctx ctx;
static char keys_topic[48];
static int32_t blob = -1;
static uint32_t frames, bytes, draw_errors, key_errors, keys_sent, draw_ms, blit_ms;   /* ms spent in the last 100 frames */
static int ready, draw = 1;   /* DOOM_VIEW_DRAW=0: receive and count only (a transport measurement) */

/* keys: pressed this tick, released with the next */
static uint8_t release[16];
static uint32_t last_draw_ms, last_blit_ms;
static unsigned nrelease;

static void logf(const char* s) { k32_log(s, slen(s)); }

/* The status bar's numbers, which the sim appends to each draw list past its entries
 * (port/drawlist_emit.c): ammo, health and armor in a strip along the bottom, and the keys */
#define HUD_Y 123               /* the strip: rows 123 .. 134 */
#define HUD_ROWS 12
static const uint16_t font[] = {   /* 3 x 5, a row of 3 bits each, top row first */
    075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,   /* 0 .. 9 */
    051245,                                                                                /* % */
};
enum { C_BACK, C_AMMO, C_HEALTH, C_ARMOR1, C_ARMOR2, C_DIM, C_BLUE, C_YELLOW, C_RED, C_WHITE, C_ORANGE, C_COLORS };
static uint8_t hud_color[C_COLORS];
static const uint8_t hud_rgb[C_COLORS][3] = {
    {0, 0, 0}, {255, 215, 0}, {220, 40, 40}, {60, 200, 60}, {80, 120, 255}, {150, 150, 150}, {40, 80, 255}, {255, 230, 0}, {230, 0, 0},
    {255, 255, 255}, {255, 140, 0},
};
static uint8_t hud[12];
static int hud_valid;

static void hud_fill(int32_t y0, uint32_t rows, int x, int y, int w, int h, uint8_t color) {
    for (int yy = y; yy < y + h; yy++) {
        if (yy < y0 || yy >= y0 + (int32_t)rows) continue;
        for (int xx = x; xx < x + w && xx < VIEW_W; xx++) if (xx >= 0) band[(yy - y0) * VIEW_W + xx] = color;
    }
}
/* a glyph at twice its size: 6 x 10 pixels */
static void hud_glyph(int32_t y0, uint32_t rows, int x, int g, uint8_t color) {
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (font[g] >> (3 * (4 - r) + (2 - c)) & 1) hud_fill(y0, rows, x + 2 * c, HUD_Y + 1 + 2 * r, 2, 2, color);
}
/* a number right-aligned to x_end, with a box behind it; percent adds a % */
static void hud_number(int32_t y0, uint32_t rows, int x_end, int v, int percent, uint8_t color) {
    int digits[4], n = 0;
    if (v < 0) v = 0;
    do { digits[n++] = v % 10; v /= 10; } while (v && n < 4);
    const int chars = n + percent, x0 = x_end - 8 * chars;
    hud_fill(y0, rows, x0 - 2, HUD_Y, 8 * chars + 2, HUD_ROWS, hud_color[C_BACK]);
    for (int i = 0; i < n; i++) hud_glyph(y0, rows, x0 + 8 * i, digits[n - 1 - i], color);
    if (percent) hud_glyph(y0, rows, x0 + 8 * n, 10, color);
}
static void hud_draw(int32_t y0, uint32_t rows) {
    if (!hud_valid || y0 + (int32_t)rows <= HUD_Y) return;
    const int health = (int16_t)(hud[2] | hud[3] << 8), armor = (int16_t)(hud[4] | hud[5] << 8), ammo = (int16_t)(hud[6] | hud[7] << 8);
    if (ammo >= 0) hud_number(y0, rows, 34, ammo, 0, hud_color[C_AMMO]);
    hud_number(y0, rows, 90, health, 1, hud_color[C_HEALTH]);
    hud_number(y0, rows, 146, armor, 1, hud_color[!armor ? C_DIM : hud[9] >= 2 ? C_ARMOR2 : C_ARMOR1]);
    const uint8_t keys = hud[10];
    if (keys & 077) hud_fill(y0, rows, 202, HUD_Y, 36, HUD_ROWS, hud_color[C_BACK]);
    for (int k = 0; k < 3; k++)   /* blue, yellow, red: a card or a skull key */
        if (keys >> k & 011) hud_fill(y0, rows, 205 + 11 * k, HUD_Y + 2, 8, 8, hud_color[C_BLUE + k]);
}

/* ---- the hunt's names ---- */
static const char text_chars[] = " -.@0123456789abcdefghijklmnopqrstuvwxyzDEIKLNTW";
static const uint8_t text_font[][5] = {   /* 5 columns of 7 rows, a bit a row, top row in bit 0 */
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x08,0x08,0x08,0x00}, {0x00,0x00,0x40,0x00,0x00}, {0x3e,0x41,0x5d,0x55,0x1e}, {0x3e,0x51,0x49,0x45,0x3e}, {0x00,0x42,0x7f,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x22,0x41,0x49,0x49,0x36}, {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3c,0x4a,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1e}, {0x20,0x54,0x54,0x54,0x78}, {0x7f,0x44,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x44,0x7f},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7e,0x09,0x01,0x02}, {0x0c,0x52,0x52,0x52,0x3e}, {0x7f,0x04,0x04,0x04,0x78}, {0x00,0x44,0x7d,0x40,0x00}, {0x20,0x40,0x44,0x3d,0x00},
    {0x7f,0x10,0x28,0x44,0x00}, {0x00,0x41,0x7f,0x40,0x00}, {0x7c,0x04,0x18,0x04,0x78}, {0x7c,0x04,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7e,0x12,0x12,0x12,0x0c},
    {0x0c,0x12,0x12,0x12,0x7e}, {0x7c,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x24}, {0x04,0x3f,0x44,0x40,0x20}, {0x3c,0x40,0x40,0x20,0x7c}, {0x1c,0x20,0x40,0x20,0x1c},
    {0x3c,0x40,0x30,0x40,0x3c}, {0x44,0x28,0x10,0x28,0x44}, {0x0e,0x50,0x50,0x50,0x3e}, {0x44,0x64,0x54,0x4c,0x44}, {0x7f,0x41,0x41,0x22,0x1c}, {0x7f,0x49,0x49,0x49,0x41},
    {0x00,0x41,0x7f,0x41,0x00}, {0x7f,0x08,0x14,0x22,0x41}, {0x7f,0x40,0x40,0x40,0x40}, {0x7f,0x02,0x04,0x08,0x7f}, {0x01,0x01,0x7f,0x01,0x01}, {0x7f,0x20,0x18,0x20,0x7f},
};
#define TEXT_W 6                /* a glyph and the gap after it */
#define TEXT_H 7
#define LINE_ROWS 9             /* the line: rows 0 .. 8 */
static const uint8_t text_color[] = {C_WHITE, C_ORANGE, C_RED, C_ARMOR1};
static const uint8_t* names;    /* the record in the frame being drawn, or 0 */
static uint32_t names_len;

static void text_put(int32_t y0, uint32_t rows, int x, int y, const uint8_t* s, unsigned n, uint8_t color) {
    if (y + TEXT_H <= y0 || y >= y0 + (int32_t)rows) return;
    for (unsigned i = 0; i < n; i++, x += TEXT_W) {
        unsigned g = 0;
        while (text_chars[g] && text_chars[g] != (char)s[i]) g++;
        if (!text_chars[g] || x < 0 || x + 5 > VIEW_W) continue;
        for (int c = 0; c < 5; c++) {
            const uint8_t bits = text_font[g][c];
            for (int r = 0; bits >> r; r++) {
                const int yy = y + r;
                if (bits >> r & 1 && yy >= y0 && yy < y0 + (int32_t)rows) band[(yy - y0) * VIEW_W + x + c] = color;
            }
        }
    }
}
/* A tag, centred over the sprite: its text with a dark edge, or white on a red box for the sim */
static void tag_put(int32_t y0, uint32_t rows, int x, int y, const uint8_t* s, unsigned n, unsigned color) {
    x -= (int)(n * TEXT_W) / 2;
    if (x < 1) x = 1;
    if (x + (int)(n * TEXT_W) > VIEW_W) x = VIEW_W - (int)(n * TEXT_W);
    y -= TEXT_H + 2;
    if (y < LINE_ROWS + 1) y = LINE_ROWS + 1;
    if (color == 2) {
        hud_fill(y0, rows, x - 1, y - 1, (int)(n * TEXT_W) + 1, TEXT_H + 2, hud_color[C_RED]);
        text_put(y0, rows, x, y, s, n, hud_color[C_WHITE]);
        return;
    }
    text_put(y0, rows, x + 1, y + 1, s, n, hud_color[C_BACK]);
    text_put(y0, rows, x, y, s, n, hud_color[text_color[color]]);
}
static void names_draw(int32_t y0, uint32_t rows) {
    if (!names) return;
    const uint8_t* p = names + 2;
    const uint8_t* end = names + names_len;
    const unsigned color = p[0] < sizeof text_color ? p[0] : 0, n = p[1];
    p += 2;
    if (p + n >= end) return;
    if (n && y0 < LINE_ROWS) {
        hud_fill(y0, rows, 0, 0, VIEW_W, LINE_ROWS, hud_color[color == 2 ? C_RED : C_BACK]);
        text_put(y0, rows, 2, 1, p, n, hud_color[color == 2 ? C_WHITE : text_color[color]]);
    }
    p += n;
    unsigned tags = *p++;
    while (tags-- && p + 4 <= end && p + 4 + p[3] <= end) {
        tag_put(y0, rows, p[0], p[1], p + 4, p[3], p[2] < sizeof text_color ? p[2] : 0);
        p += 4 + p[3];
    }
}

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
    { char v[4]; env("DOOM_VIEW_DRAW", v, sizeof v, "1"); draw = v[0] != '0'; }
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
    for (int c = 0; c < C_COLORS; c++) {   /* the status bar's colours: the palette's nearest */
        uint32_t best = 0xffffffffu;
        for (int i = 0; i < 256; i++) {
            const int dr = rgb[3 * i] - hud_rgb[c][0], dg = rgb[3 * i + 1] - hud_rgb[c][1], db = rgb[3 * i + 2] - hud_rgb[c][2];
            const uint32_t d = (uint32_t)(dr * dr + dg * dg + db * db);
            if (d < best) { best = d; hud_color[c] = (uint8_t)i; }
        }
    }
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
    const uint32_t vh = msg[8], entries = (uint32_t)(msg[4] | msg[5] << 8);
    hud_valid = entries + sizeof hud <= len && msg[entries] == 'H' && msg[entries + 1] >= sizeof hud;
    if (hud_valid) memcpy(hud, msg + entries, sizeof hud);
    names = 0;
    if (hud_valid && entries + hud[1] + 4 <= len && msg[entries + hud[1]] == 'P') {
        names = msg + entries + hud[1];
        names_len = names[1];
        if (entries + hud[1] + names_len > len) names = 0;
    }
    for (uint32_t y0 = 0; draw && y0 < vh; y0 += BAND_ROWS) {
        const uint32_t rows = vh - y0 < BAND_ROWS ? vh - y0 : BAND_ROWS;
        const int64_t t0 = k32_time_ms();
        int32_t rc = dl_draw(&ctx, msg, len, band, VIEW_W, (int32_t)y0, rows);
        const int64_t t1 = k32_time_ms();
        if (rc) { draw_errors++; break; }
        if (ctx.detailshift)   /* low detail: each column two pixels wide */
            for (uint32_t r = 0; r < rows; r++)
                for (uint32_t x = VIEW_W / 2; x-- > 0;) band[r * VIEW_W + 2 * x] = band[r * VIEW_W + 2 * x + 1] = band[r * VIEW_W + x];
        hud_draw((int32_t)y0, rows);
        names_draw((int32_t)y0, rows);
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
