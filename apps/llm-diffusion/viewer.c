/* llm-diffusion-viewer: shows a generation of llm-diffusion forming on the
 * Cardputer's screen.
 *
 * The pod starts a generation at the diffusion pod's Service, reads
 * /state?from=<step> while it runs, and draws the window's text after every
 * step: revealed tokens in white, each masked position as a grey block, and
 * a title line with the step and its time. When the generation is done the
 * title shows the time a step and the tokens a second; PAUSE ms later the
 * next generation starts with a fresh seed, its step count the next one of
 * STEPS.
 *
 * Keys: 1, 2 and 3 start a generation of 8, 16 or 32 steps when none runs;
 * any other key starts the next one of STEPS.
 *
 * Environment: URL of the diffusion pod (http://llm-diffusion), STEPS, a
 * comma list (8,16,32), TEMP, the temperature x 100 (80; 0 is greedy),
 * PAUSE (10000), PROMPT (none).
 *
 *   GET /   the viewer's state: run, step, frames drawn, generations */
#include "../../include/runtime/k32_app_abi.h"

#define NOINLINE __attribute__((noinline))
#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

/* screen: 240 x 135, a 6 x 8 font: 40 columns, 12 rows of text below the title */
#define COLS 40
#define ROWS 12
#define TEXT_Y 12
#define ROW_H 9
#define WHITE 0xFFFFu
#define GREY 0x8410u
#define BLACK 0x0000u
#define TITLE_BG 0x0010u
#define ACCENT 0xFD20u
#define MASK_CH '_'

enum { START, RUN };

static char g_url[64] = "http://llm-diffusion";
static uint32_t g_url_len = 20;
static char g_prompt[96];
static uint32_t g_prompt_len;
static uint32_t g_steps_list[8] = {8, 16, 32}, g_steps_n = 3, g_next_steps;
static uint32_t g_temp, g_pause;

static uint32_t g_mode = START, g_next_at, g_poll_at, g_seen, g_run, g_steps, g_frames, g_runs, g_rows_drawn;
static int32_t g_want = -1;   /* steps a key asked for, or -1 */

static uint8_t g_resp[3072];

/* ---- text out ------------------------------------------------------------- */
static char g_out[256];
static uint32_t g_out_len;
static NOINLINE void put(const char* s) {
    while (*s && g_out_len < sizeof g_out) g_out[g_out_len++] = *s++;
}
static NOINLINE void put_n(const char* s, uint32_t n) {
    for (uint32_t i = 0; i < n && g_out_len < sizeof g_out; i++) g_out[g_out_len++] = s[i];
}
static NOINLINE void put_u(uint32_t v) {
    char d[12];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k && g_out_len < sizeof g_out) g_out[g_out_len++] = d[--k];
}
/* v / 100 with two decimals */
static NOINLINE void put_x100(uint32_t v) {
    put_u(v / 100u);
    put(".");
    put_u(v / 10u % 10u);
    put_u(v % 10u);
}
static NOINLINE void say(void) {
    k32_log(g_out, g_out_len);
    g_out_len = 0;
}
static uint32_t now_ms(void) { return (uint32_t)k32_time_ms(); }
static NOINLINE uint32_t atou(const char* s, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n && s[i] >= '0' && s[i] <= '9'; i++) v = v * 10u + (uint32_t)(s[i] - '0');
    return v;
}
static NOINLINE uint32_t env_u(const char* key, uint32_t klen, uint32_t dflt) {
    char v[12];
    const int32_t n = k32_env_get(key, klen, v, sizeof v);
    return n > 0 ? atou(v, (uint32_t)n) : dflt;
}

/* ---- the JSON of /state, read by key -------------------------------------- */
/* the offset after the first (or the last) occurrence of key in b[from..n), or 0 */
static NOINLINE uint32_t find(const uint8_t* b, uint32_t from, uint32_t n, const char* key, uint32_t klen, int last) {
    uint32_t hit = 0;
    for (uint32_t i = from; i + klen <= n; i++) {
        uint32_t k = 0;
        while (k < klen && b[i + k] == (uint8_t)key[k]) k++;
        if (k == klen) {
            hit = i + klen;
            if (!last) return hit;
        }
    }
    return hit;
}
static NOINLINE uint32_t num_at(const uint8_t* b, uint32_t at, uint32_t n) {
    return at ? atou((const char*)b + at, n - at) : 0;
}

/* ---- the screen ------------------------------------------------------------ */
static NOINLINE void title(uint32_t color) {
    if (g_out_len < COLS) {
        for (uint32_t i = g_out_len; i < COLS; i++) g_out[i] = ' ';
    }
    k32_display_text(0, 1, g_out, COLS, color, TITLE_BG);
    g_out_len = 0;
}

/* one row: the characters in white on black, a masked position as a grey block */
static NOINLINE void draw_row(uint32_t row, const char* s, uint32_t n) {
    char line[COLS];
    for (uint32_t i = 0; i < COLS; i++) line[i] = i < n && s[i] != MASK_CH ? s[i] : ' ';
    const int32_t y = TEXT_Y + (int32_t)(row * ROW_H);
    k32_display_text(0, y, line, COLS, WHITE, BLACK);
    for (uint32_t i = 0; i < n; i++)
        if (s[i] == MASK_CH) k32_display_rect((int32_t)(6 * i), y, 5, 8, GREY);
}

/* the window's text, wrapped at spaces into rows of COLS */
static NOINLINE void draw_text(const char* t, uint32_t n) {
    uint32_t row = 0, i = 0;
    while (i < n && row < ROWS) {
        uint32_t len = 0;
        while (len < COLS && i + len < n && t[i + len] != '\n') len++;
        uint32_t next = i + len;
        if (next < n && t[next] == '\n') next++;
        else if (len == COLS && next < n && t[next] != ' ') {
            uint32_t sp = len;
            while (sp > COLS / 2 && t[i + sp - 1] != ' ') sp--;
            if (sp > COLS / 2) { len = sp; next = i + sp; }
        } else if (next < n && t[next] == ' ') next++;
        draw_row(row++, t + i, len);
        i = next;
    }
    if (row < g_rows_drawn)
        k32_display_rect(0, TEXT_Y + (int32_t)(row * ROW_H), 240, (int32_t)((g_rows_drawn - row) * ROW_H), BLACK);
    g_rows_drawn = row;
}

/* a JSON string at b[at..) unescaped into out: ASCII, a newline as '\n' */
static NOINLINE uint32_t json_text(const uint8_t* b, uint32_t at, uint32_t n, char* out, uint32_t cap) {
    uint32_t o = 0;
    for (uint32_t i = at; i < n && b[i] != '"' && o < cap; i++) {
        uint8_t c = b[i];
        if (c == '\\' && i + 1 < n) {
            c = b[++i];
            if (c == 'n') c = '\n';
        }
        out[o++] = c >= 0x20 && c < 0x7f ? (char)c : c == '\n' ? '\n' : '?';
    }
    return o;
}

/* ---- the diffusion pod ------------------------------------------------------- */
static NOINLINE int32_t call(const char* path, uint32_t plen, uint32_t* got) {
    char url[256];
    uint32_t u = 0;
    for (uint32_t i = 0; i < g_url_len && u < sizeof url; i++) url[u++] = g_url[i];
    for (uint32_t i = 0; i < plen && u < sizeof url; i++) url[u++] = path[i];
    *got = 0;
    return k32_http_call(LIT("GET"), url, u, "", 0, (const uint8_t*)"", 0, g_resp, sizeof g_resp, got, 3000);
}

static NOINLINE void start(uint32_t now) {
    uint32_t steps = g_want > 0 ? (uint32_t)g_want : g_steps_list[g_next_steps++ % g_steps_n];
    g_want = -1;
    const uint32_t seed = (k32_random() & 0x7fffffffu) | 1u;
    put("/generate?steps="); put_u(steps); put("&seed="); put_u(seed); put("&temp="); put_u(g_temp);
    if (g_prompt_len) { put("&prompt="); put_n(g_prompt, g_prompt_len); }
    char path[200];
    uint32_t pl = g_out_len < sizeof path ? g_out_len : sizeof path;
    for (uint32_t i = 0; i < pl; i++) path[i] = g_out[i];
    g_out_len = 0;
    uint32_t got;
    const int32_t rc = call(path, pl, &got);
    if (rc == 200 || rc == 409) {
        /* 409: a generation is running already; show that one */
        g_mode = RUN;
        g_seen = 0;
        g_run = rc == 200 ? num_at(g_resp, find(g_resp, 0, got, LIT("\"run\":"), 0), got) : 0;
        g_steps = rc == 200 ? steps : 0;
        g_poll_at = now;
        g_runs++;
        put("mdlm  starting ");
        put_u(steps);
        put(" steps");
        title(ACCENT);
        put("llm-diffusion-viewer: run "); put_u(g_run); put(" steps "); put_u(steps); put(" seed "); put_u(seed);
        put(rc == 409 ? " (one was running)" : "");
        say();
    } else {
        put("mdlm  waiting for llm-diffusion ("); put(rc < 0 ? "-" : ""); put_u((uint32_t)(rc < 0 ? -rc : rc)); put(")");
        title(GREY);
        g_next_at = now + 3000;
    }
}

static NOINLINE void poll(uint32_t now) {
    char path[48];
    g_out_len = 0;
    put("/state?from="); put_u(g_seen);
    const uint32_t pl = g_out_len;
    for (uint32_t i = 0; i < pl; i++) path[i] = g_out[i];
    g_out_len = 0;
    uint32_t got;
    const int32_t rc = call(path, pl, &got);
    g_poll_at = now + 400;
    if (rc == K32_ERR_TOOBIG) {
        /* several steps behind: read only the newest next time */
        const uint32_t step = num_at(g_resp, find(g_resp, 0, got, LIT("\"step\":"), 0), got);
        if (step > g_seen + 1) g_seen = step - 1;
        return;
    }
    if (rc != 200) return;
    const uint32_t run = num_at(g_resp, find(g_resp, 0, got, LIT("\"run\":"), 0), got);
    const uint32_t st = find(g_resp, 0, got, LIT("\"state\":\""), 0);
    const int done = st && g_resp[st] == 'd', idle = st && g_resp[st] == 'i';
    if (run != g_run) { g_run = run; g_seen = 0; }
    g_steps = num_at(g_resp, find(g_resp, 0, got, LIT("\"steps\":"), 0), got);
    const uint32_t e = find(g_resp, 0, got, LIT("{\"step\":"), 1);
    if (e) {
        const uint32_t step = num_at(g_resp, e, got);
        const uint32_t us = num_at(g_resp, find(g_resp, e, got, LIT("\"us\":"), 0), got);
        const uint32_t tx = find(g_resp, e, got, LIT("\"text\":\""), 0);
        char text[COLS * ROWS];
        const uint32_t tn = tx ? json_text(g_resp, tx, got, text, sizeof text) : 0;
        g_seen = step + 1;
        g_frames++;
        put("mdlm  step "); put_u(step + 1); put("/"); put_u(g_steps); put("  "); put_u(us / 1000u); put(" ms");
        title(WHITE);
        draw_text(text, tn);
        put("llm-diffusion-viewer: run "); put_u(g_run); put(" step "); put_u(step + 1); put("/"); put_u(g_steps);
        put(" "); put_u(us / 1000u); put(" ms, frames "); put_u(g_frames);
        say();
    }
    if (done && (g_seen >= g_steps || !e)) {
        const uint32_t per = num_at(g_resp, find(g_resp, 0, got, LIT("\"us_per_step\":"), 0), got);
        const uint32_t tps = num_at(g_resp, find(g_resp, 0, got, LIT("\"tokens_per_s_x100\":"), 0), got);
        const uint32_t gen = num_at(g_resp, find(g_resp, 0, got, LIT("\"generated\":"), 0), got);
        put("mdlm  "); put_u(g_steps); put(" steps  "); put_x100(per / 10000u); put(" s/step  "); put_x100(tps); put(" tok/s");
        title(ACCENT);
        put("llm-diffusion-viewer: run "); put_u(g_run); put(" done, "); put_u(g_steps); put(" steps, "); put_u(gen);
        put(" tokens, "); put_u(per / 1000u); put(" ms a step, tokens/s x100 "); put_u(tps);
        say();
        g_mode = START;
        g_next_at = now + g_pause;
    } else if (idle) {
        g_mode = START;
        g_next_at = now + 3000;
    }
}

/* ---- exports ---------------------------------------------------------------- */
static unsigned char g_block[256];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    int32_t n = k32_env_get(LIT("URL"), g_url, sizeof g_url);
    if (n > 0) g_url_len = (uint32_t)n;
    char v[48];
    n = k32_env_get(LIT("STEPS"), v, sizeof v);
    if (n > 0) {
        g_steps_n = 0;
        for (int32_t i = 0; i < n && g_steps_n < 8;) {
            const uint32_t s = atou(v + i, (uint32_t)(n - i));
            if (s >= 1 && s <= 64) g_steps_list[g_steps_n++] = s;
            while (i < n && v[i] != ',') i++;
            i++;
        }
        if (!g_steps_n) { g_steps_list[0] = 32; g_steps_n = 1; }
    }
    g_temp = env_u(LIT("TEMP"), 80);
    g_pause = env_u(LIT("PAUSE"), 10000);
    /* the prompt, URL-encoded */
    char p[48];
    n = k32_env_get(LIT("PROMPT"), p, sizeof p);
    for (int32_t i = 0; i < n && g_prompt_len + 3 <= sizeof g_prompt; i++) {
        const char c = p[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) g_prompt[g_prompt_len++] = c;
        else {
            static const char hx[] = "0123456789ABCDEF";
            g_prompt[g_prompt_len++] = '%';
            g_prompt[g_prompt_len++] = hx[(uint8_t)c >> 4];
            g_prompt[g_prompt_len++] = hx[(uint8_t)c & 15];
        }
    }
    k32_display_rect(0, 0, 240, 135, BLACK);
    put("mdlm  masked diffusion LM");
    title(GREY);
    k32_display_text(0, 127, LIT("keys 1/2/3: 8/16/32 steps"), GREY, BLACK);
    g_next_at = now_ms() + 1000;
    put("llm-diffusion-viewer: started, url "); put_n(g_url, g_url_len);
    say();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t tick_ms) {
    (void)tick_ms;
    const uint32_t now = now_ms();
    for (int32_t k; (k = k32_key_read()) >= 0;) {
        if (g_mode != START) continue;
        g_want = k == '1' ? 8 : k == '2' ? 16 : k == '3' ? 32 : 0;
        g_next_at = now;
    }
    if (g_mode == START && (int32_t)(now - g_next_at) >= 0) start(now);
    else if (g_mode == RUN && (int32_t)(now - g_poll_at) >= 0) poll(now);
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr;
    (void)len;
    g_out_len = 0;
    put("{\"run\":"); put_u(g_run); put(",\"state\":\""); put(g_mode == RUN ? "running" : "waiting");
    put("\",\"steps\":"); put_u(g_steps); put(",\"step\":"); put_u(g_seen); put(",\"frames\":"); put_u(g_frames);
    put(",\"generations\":"); put_u(g_runs); put("}");
    k32_resp_begin(200, LIT("application/json"));
    k32_resp_write(g_out, g_out_len);
    g_out_len = 0;
    k32_resp_end();
    return 200;
}
