/*
 * tessera-hud: the display-node pod. Runs as an app_main loop (no HTTP), uses
 * the capability-gated display, keyboard and speaker imports, and talks to the
 * rest of the app through two cluster channels:
 *
 *   tessera.events (subscribe): issue/comment events from tessera-core and the
 *                                "stats" lines the ingress publishes every second
 *   tessera.input  (publish):   one small JSON object per key press
 *
 * Memory: one 64 KiB Wasm page. No heap.
 */
#include "../../../include/runtime/k32_app_abi.h"

#define W 240
#define H 135
#define C_BG     0x10A2u
#define C_PANEL  0x194Bu
#define C_TEXT   0xFFFFu
#define C_DIM    0x8410u
#define C_CYAN   0x07FFu
#define C_GREEN  0x07E0u
#define C_AMBER  0xFD20u
#define C_RED    0xF800u

static int ev_handle = -1;
static int in_handle = -1;
static unsigned char rx[256];
static char line[48];      /* text typed on the keyboard, mirrored to browsers */
static unsigned line_len;
static unsigned long issues_open, issues_total, created_here, events_seen;
static unsigned rps, viewers, heap_kb;
static unsigned long last_draw_ms, last_event_ms;
static char last_key_text[32];

static unsigned slen(const char* s) { unsigned n = 0; while (s[n]) n++; return n; }
static void text(int x, int y, const char* s, unsigned fg, unsigned bg) { k32_display_text(x, y, s, slen(s), fg, bg); }

static unsigned utoa10(unsigned long v, char* out) {
    char tmp[12]; unsigned t = 0, n = 0;
    do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (t) out[n++] = tmp[--t];
    out[n] = 0;
    return n;
}

/* Finds "key":<digits> in a JSON-ish buffer; returns 1 and the value when found */
static int json_uint(const unsigned char* s, unsigned len, const char* key, unsigned long* out) {
    unsigned kl = slen(key);
    for (unsigned i = 0; i + kl + 2 < len; i++) {
        if (s[i] != '"') continue;
        unsigned j = 0;
        while (j < kl && i + 1 + j < len && s[i + 1 + j] == (unsigned char)key[j]) j++;
        if (j != kl || i + 1 + kl >= len || s[i + 1 + kl] != '"') continue;
        unsigned p = i + 2 + kl;
        while (p < len && (s[p] == ':' || s[p] == ' ')) p++;
        unsigned long v = 0; int any = 0;
        while (p < len && s[p] >= '0' && s[p] <= '9') { v = v * 10 + (s[p] - '0'); p++; any = 1; }
        if (!any) return 0;
        *out = v;
        return 1;
    }
    return 0;
}

static int json_str(const unsigned char* s, unsigned len, const char* key, char* out, unsigned cap) {
    unsigned kl = slen(key);
    for (unsigned i = 0; i + kl + 3 < len; i++) {
        if (s[i] != '"') continue;
        unsigned j = 0;
        while (j < kl && s[i + 1 + j] == (unsigned char)key[j]) j++;
        if (j != kl || s[i + 1 + kl] != '"') continue;
        unsigned p = i + 2 + kl;
        while (p < len && (s[p] == ':' || s[p] == ' ')) p++;
        if (p >= len || s[p] != '"') return 0;
        p++;
        unsigned o = 0;
        while (p < len && s[p] != '"' && o + 1 < cap) {
            if (s[p] == '\\' && p + 1 < len) p++;
            out[o++] = (char)s[p++];
        }
        out[o] = 0;
        return 1;
    }
    return 0;
}

static void draw_frame(void) {
    k32_display_rect(0, 0, W, H, C_BG);
    k32_display_rect(0, 0, W, 18, C_PANEL);
    char title[32];
    int n = k32_env_get("HUD_TITLE", 9, title, sizeof title - 1);
    if (n <= 0) { title[0] = 'T'; title[1] = 'E'; title[2] = 'S'; title[3] = 'S'; title[4] = 'E'; title[5] = 'R'; title[6] = 'A'; title[7] = 0; }
    else title[n < 31 ? n : 31] = 0;
    text(6, 5, title, C_CYAN, C_PANEL);
    text(120, 5, "esp32-node-01", C_DIM, C_PANEL);
    text(6, 24, "open", C_DIM, C_BG);
    text(86, 24, "req/s", C_DIM, C_BG);
    text(166, 24, "viewers", C_DIM, C_BG);
    text(6, 66, "heap", C_DIM, C_BG);
    text(6, 96, "type on keyboard, Enter posts a comment", C_DIM, C_BG);
}

static void draw_stats(void) {
    char buf[16];
    k32_display_rect(6, 34, 228, 22, C_BG);
    utoa10(issues_open, buf); text(6, 36, buf, C_TEXT, C_BG);
    utoa10(rps, buf);         text(86, 36, buf, C_GREEN, C_BG);
    utoa10(viewers, buf);     text(166, 36, buf, C_AMBER, C_BG);
    /* heap bar: 0..200 KB */
    unsigned pct = heap_kb > 200 ? 100 : (heap_kb * 100) / 200;
    k32_display_bar(40, 66, 150, 8, pct, C_CYAN, C_PANEL);
    utoa10(heap_kb, buf); unsigned l = slen(buf); buf[l] = 'K'; buf[l + 1] = 0;
    k32_display_rect(196, 64, 44, 12, C_BG);
    text(196, 66, buf, C_TEXT, C_BG);
    k32_display_rect(6, 78, 228, 12, C_BG);
    text(6, 80, "events", C_DIM, C_BG);
    utoa10(events_seen, buf); text(50, 80, buf, C_TEXT, C_BG);
    text(110, 80, "created here", C_DIM, C_BG);
    utoa10(created_here, buf); text(190, 80, buf, C_TEXT, C_BG);
}

static void draw_line(void) {
    k32_display_rect(0, 108, W, 27, C_PANEL);
    char shown[41];
    unsigned start = line_len > 38 ? line_len - 38 : 0, o = 0;
    shown[o++] = '>';
    shown[o++] = ' ';
    for (unsigned i = start; i < line_len && o < 40; i++) shown[o++] = line[i];
    shown[o++] = '_';
    shown[o] = 0;
    text(6, 117, shown, C_TEXT, C_PANEL);
}

static void emit_key(char c) {
    char msg[40];
    unsigned o = 0;
    const char* head = "{\"t\":\"key\",\"c\":\"";
    for (unsigned i = 0; head[i]; i++) msg[o++] = head[i];
    if (c == '\n') { msg[o++] = '\\'; msg[o++] = 'n'; }
    else if (c == '\b') { msg[o++] = '\\'; msg[o++] = 'b'; }
    else if (c == '"' || c == '\\') { msg[o++] = '\\'; msg[o++] = c; }
    else msg[o++] = c;
    msg[o++] = '"'; msg[o++] = '}';
    k32_event_emit("tessera.input", 13, msg, o);
}

static void handle_event(const unsigned char* data, unsigned len) {
    char t[12];
    events_seen++;
    if (json_str(data, len, "t", t, sizeof t)) {
        if (t[0] == 's' && t[1] == 't') {            /* "stats" from the ingress */
            unsigned long v;
            if (json_uint(data, len, "open", &v)) issues_open = v;
            if (json_uint(data, len, "total", &v)) issues_total = v;
            if (json_uint(data, len, "rps", &v)) rps = (unsigned)v;
            if (json_uint(data, len, "viewers", &v)) viewers = (unsigned)v;
            if (json_uint(data, len, "heap_kb", &v)) heap_kb = (unsigned)v;
            return;
        }
        if (t[0] == 'h' && t[1] == 'u') {            /* "hud": core echoes the live note */
            return;
        }
    }
    char op[12];
    if (json_str(data, len, "op", op, sizeof op)) {
        if (op[0] == 'c' && op[1] == 'r') {
            k32_tone(1320, 40, 35);
            unsigned long v;
            if (json_uint(data, len, "actor", &v) && v == 0) created_here++;
            issues_open++;
        } else if (op[0] == 'd') {
            k32_tone(440, 60, 25);
        } else {
            k32_tone(880, 15, 15);
        }
    }
    last_event_ms = (unsigned long)k32_time_ms();
}

K32_EXPORT("app_main") int app_main(void) {
    ev_handle = kube32_channel_open("tessera.events", 0);
    (void)in_handle;
    draw_frame();
    draw_stats();
    draw_line();
    k32_tone(880, 30, 20);
    k32_tone(1320, 50, 20);
    for (;;) {
        /* 1. Keyboard */
        int k;
        int typed = 0;
        while ((k = k32_key_read()) >= 0) {
            char c = (char)k;
            typed = 1;
            if (c == '\b') { if (line_len) line_len--; }
            else if (c == '\n') { line_len = 0; }
            else if (c >= 32 && c < 127 && line_len < sizeof line - 1) { line[line_len++] = c; }
            emit_key(c);
            k32_tone(2200, 6, 12);
        }
        if (typed) draw_line();

        /* 2. Cluster events (50 ms poll keeps the loop responsive to keys) */
        int n = kube32_channel_recv(ev_handle, rx, sizeof rx, 50);
        if (n > 0) handle_event(rx, (unsigned)n);

        /* 3. Redraw counters once per second */
        unsigned long now = (unsigned long)k32_time_ms();
        if (now - last_draw_ms >= 1000) {
            last_draw_ms = now;
            draw_stats();
        }
    }
    return 0;
}
