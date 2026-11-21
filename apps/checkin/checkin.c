/* checkin: a check-in job that follows its hardware from node to node
 * (docs/migration.md#hardware-following-pods, examples/checkin.yaml).
 *
 * One record, four steps. The weight and the label are globals in linear
 * memory, so they travel with the pod when it moves:
 *
 *   1 weigh    no hardware. Reads the scale's Service (SCALE_URL, the number
 *              in field SCALE_FIELD) with k32_http_call: a reading is one
 *              small request, so the pod stays where it is. It asks twice a
 *              second, and goes on without a weight after 20 s.
 *   2 label    LABEL_NEEDS (display and keyboard). Shows the weight and takes
 *              the label typed on the keyboard (Enter saves it, Backspace
 *              deletes), or LABEL when it is set.
 *   3 confirm  CONFIRM_NEEDS (speaker and LED). Plays one tone per digit of
 *              the weight at VOLUME percent of the speaker's ceiling and
 *              flashes the LED green, then turns it off.
 *   4 log      no hardware. Logs the record where the pod is.
 *
 * Each step asks k32_need for its devices on every tick. When the node lacks
 * them, the call returns K32_NEED_MOVING and the control plane moves the pod
 * to a node that has them; the step runs there after k32_restored.
 *
 *   GET /        {"step":"<step>","node":"<node>","weight_g":<g>,"label":"<label>",
 *                 "trail":["<node of each step>",...],"moves":<n>,
 *                 "need":<k32_need>,"display":<rc>,"led":<rc>,"tone":<rc>}
 *                display, led and tone: what k32_display_info (at the start
 *                on each node), k32_led_set and k32_tone last returned
 *   POST /again  a new record from step 1, once the last one is logged
 *
 * Built into images/sd/checkin.wasm (not embedded); examples/checkin.yaml
 * deploys it inline. */
#include "../../include/runtime/k32_app_abi.h"

#define NOINLINE __attribute__((noinline))

enum { WEIGH = 1, LABEL = 2, CONFIRM = 3, LOG = 4, DONE = 5 };
static const char* const step_names[] = {"", "weigh", "label", "confirm", "log", "done"};

#define LABEL_MAX 24
#define NODE_MAX 32
#define WEIGH_WAIT_MS 20000   /* an on-demand driver starts in a few seconds */
#define WEIGH_EVERY_MS 500

/* The record and where the job is: they move with the pod */
static int32_t step;
static int32_t weight_g;
static int64_t weigh_since, weigh_last;   /* k32_time_ms: it continues across moves */
static char label[LABEL_MAX + 1];
static uint32_t label_len;
static char trail[4][NODE_MAX];
static int32_t moves;
static int32_t drawn;        /* the step whose screen this node shows */
static int32_t waiting;      /* the step that logged it waits for a node */
static int32_t need_rc, display_rc, led_rc, tone_rc;
static int32_t screen_w, screen_h;

static unsigned char req_buf[256];
static char resp[384];
static char env_buf[96];
static char out[512];
static uint32_t out_len;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

/* ---- Text ---------------------------------------------------------------- */
static NOINLINE uint32_t slen(const char* s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static NOINLINE void put(const char* s) { while (*s && out_len < sizeof out) out[out_len++] = *s++; }
static NOINLINE void put_int(int32_t v) {
    uint32_t u = (uint32_t)v;
    if (v < 0) { put("-"); u = 0u - u; }
    char d[10];
    int k = 0;
    do { d[k++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (k && out_len < sizeof out) out[out_len++] = d[--k];
}
static NOINLINE void put_str(const char* s) { put("\""); put(s); put("\""); }
static void log_out(void) { k32_log(out, out_len); }

/* The value of environment variable `key`, or `def` (in env_buf) */
static NOINLINE const char* env(const char* key, const char* def) {
    const int32_t n = k32_env_get(key, slen(key), env_buf, sizeof env_buf - 1);
    if (n < 0) return def;
    env_buf[n < (int32_t)sizeof env_buf - 1 ? n : (int32_t)sizeof env_buf - 1] = 0;
    return env_buf;
}

/* This node's name (NODE_NAME, from spec.nodeName) into trail[i] */
static NOINLINE void note_node(int i) {
    const char* n = env("NODE_NAME", "?");
    uint32_t k = 0;
    for (; n[k] && k < NODE_MAX - 1; k++) trail[i][k] = n[k];
    trail[i][k] = 0;
}

/* k32_need for the devices of environment variable `key` (`def` when unset) */
static NOINLINE int32_t need(const char* key, const char* def) {
    const char* list = key ? env(key, def) : "";
    need_rc = k32_need(list, slen(list));
    if (need_rc == K32_NEED_MOVING && waiting != step) {
        waiting = step;
        out_len = 0;
        put("step "); put(step_names[step]); put(": this node lacks "); put(list); put(", moving");
        log_out();
    }
    return need_rc;
}

static NOINLINE void log_step(void) {
    out_len = 0;
    put("step "); put(step_names[step]); put(" on "); put(trail[step - 1]);
    put(": weight "); put_int(weight_g); put(" g, label \""); put(label); put("\"");
    log_out();
}

/* ---- Step 1: the scale's reading over its Service ------------------------- */
/* The number after "<field>": in resp, rounded to an integer */
static NOINLINE int parse_field(const char* field, uint32_t n, int32_t* value) {
    const uint32_t flen = slen(field);
    for (uint32_t i = 0; i + flen + 3 <= n; i++) {
        if (resp[i] != '"') continue;
        uint32_t j = 0;
        while (j < flen && resp[i + 1 + j] == field[j]) j++;
        if (j < flen || resp[i + 1 + flen] != '"') continue;
        uint32_t p = i + flen + 2;
        while (p < n && (resp[p] == ':' || resp[p] == ' ')) p++;
        int neg = p < n && resp[p] == '-';
        if (neg) p++;
        if (p >= n || resp[p] < '0' || resp[p] > '9') return 0;
        int32_t v = 0;
        while (p < n && resp[p] >= '0' && resp[p] <= '9') v = v * 10 + (resp[p++] - '0');
        if (p + 1 < n && resp[p] == '.' && resp[p + 1] >= '5' && resp[p + 1] <= '9') v++;
        *value = neg ? -v : v;
        return 1;
    }
    return 0;
}

static void weigh(void) {
    if (need(0, 0) < 0) return;
    const int64_t t = k32_time_ms();
    if (!weigh_since) {
        weigh_since = t;
        note_node(0);
    } else if (t - weigh_last < WEIGH_EVERY_MS) {
        return;
    }
    weigh_last = t;
    const char* url = env("SCALE_URL", "http://scale/");
    uint32_t got = 0;
    const int32_t rc = k32_http_call("GET", 3, url, slen(url), "", 0, (const uint8_t*)"", 0, (uint8_t*)resp, sizeof resp, &got, 3000);
    /* A long answer still has the reading near its start */
    const int ok = rc == 200 || (rc == K32_ERR_TOOBIG && k32_http_status() == 200);
    if (ok && parse_field(env("SCALE_FIELD", "weight_g"), got, &weight_g)) {
        step = LABEL;
        log_step();
        return;
    }
    if (t - weigh_since < WEIGH_WAIT_MS) return;
    /* The scale stays away: the record goes on without a weight */
    weight_g = -1;
    out_len = 0;
    put("step weigh: no reading from "); put(env("SCALE_URL", "http://scale/")); put(" (last answer "); put_int(rc); put("), going on without a weight");
    log_out();
    step = LABEL;
}

/* ---- Step 2: the label, on the display and keyboard ------------------------ */
#define WHITE 0xFFFF
#define BLACK 0x0000
#define GREEN 0x07E0
#define AMBER 0xFD20
#define GRAY  0x8410

static NOINLINE void text(int32_t y, const char* s, uint32_t fg) { k32_display_text(8, y, s, slen(s), fg, BLACK); }

static void draw_label_line(int cursor) {
    out_len = 0;
    put("label   "); put(label); if (cursor) put("_");
    while (out_len < 8 + LABEL_MAX + 1) out[out_len++] = ' ';
    k32_display_text(8, 52, out, out_len, WHITE, BLACK);
}

static void size_display(void);

static void draw_label_screen(void) {
    size_display();
    if (display_rc != K32_OK) return;
    k32_display_rect(0, 0, screen_w, screen_h, BLACK);
    k32_display_rect(0, 0, screen_w, 18, GREEN);
    k32_display_text(8, 5, "check-in", 8, BLACK, GREEN);
    out_len = 0;
    put("weight  ");
    if (weight_g < 0) put("unknown"); else { put_int(weight_g); put(" g"); }
    k32_display_text(8, 32, out, out_len, WHITE, BLACK);
    draw_label_line(1);
    text(76, "type the label, Enter saves", GRAY);
}

static int label_char(int32_t c) { return c >= 0x20 && c < 0x7F && c != '"' && c != '\\'; }

static void take_label(void) {
    if (need("LABEL_NEEDS", "tft-display,keyboard") == K32_NEED_MOVING) return;
    if (need_rc < 0) {
        /* No display here and the pod does not move: no label */
        label_len = 0;
        label[0] = 0;
        step = CONFIRM;
        return;
    }
    if (drawn != LABEL) {
        drawn = LABEL;
        note_node(1);
        while (k32_key_read() >= 0) {}   /* keys pressed before the job came */
        draw_label_screen();
        const char* preset = env("LABEL", "");
        if (*preset) {
            for (label_len = 0; preset[label_len] && label_len < LABEL_MAX; label_len++) label[label_len] = label_char(preset[label_len]) ? preset[label_len] : '-';
            label[label_len] = 0;
            draw_label_line(0);
        } else {
            return;
        }
    } else {
        int changed = 0;
        for (int i = 0; i < 8; i++) {
            const int32_t c = k32_key_read();
            if (c < 0) break;
            if (c == '\n' && label_len) {
                step = CONFIRM;   /* saved below */
                break;
            }
            if (c == '\b' && label_len) label[--label_len] = 0, changed = 1;
            else if (label_char(c) && label_len < LABEL_MAX) label[label_len++] = (char)c, label[label_len] = 0, changed = 1;
        }
        if (changed) draw_label_line(1);
        if (step != CONFIRM) return;
        draw_label_line(0);
    }
    /* The screen keeps this once the pod has gone on */
    text(76, "saved: on to the speaker   ", AMBER);
    step = CONFIRM;
    log_step();
}

/* ---- Step 3: the weight's digits as tones, the LED --------------------------- */
static const uint16_t notes[10] = {523, 587, 659, 784, 880, 1047, 1175, 1319, 1568, 1760};

static NOINLINE uint32_t number(const char* s, uint32_t def) {
    uint32_t v = 0;
    if (*s < '0' || *s > '9') return def;
    while (*s >= '0' && *s <= '9') v = v * 10 + (uint32_t)(*s++ - '0');
    return v;
}

static void confirm(void) {
    if (need("CONFIRM_NEEDS", "speaker,led") == K32_NEED_MOVING) return;
    if (need_rc < 0) { step = LOG; return; }
    note_node(2);
    uint32_t volume = number(env("VOLUME", "5"), 5);
    if (volume > 100) volume = 100;
    led_rc = k32_led_set(0, 0x002000);
    /* All in this one call: the notes keep their spacing whatever the tick does */
    char d[10];
    int k = 0;
    uint32_t u = weight_g < 0 ? 0u : (uint32_t)weight_g;
    do { d[k++] = (char)(u % 10u); u /= 10u; } while (u && k < 10);
    while (k) {
        tone_rc = k32_tone(notes[(int)d[--k]], 120, volume);
        esp32_delay_ms(160);
    }
    tone_rc = k32_tone(2093, 200, volume);
    for (int i = 0; i < 3; i++) {
        k32_led_set(0, 0);
        esp32_delay_ms(120);
        k32_led_set(0, 0x004000);
        esp32_delay_ms(120);
    }
    k32_led_set(0, 0);   /* the LED stays as it is left when the pod goes */
    step = LOG;
    log_step();
}

/* ---- Step 4: the record -------------------------------------------------------- */
static void log_record(void) {
    if (need(0, 0) < 0) return;
    note_node(3);
    out_len = 0;
    put("checkin label=\""); put(label); put("\" weight_g="); put_int(weight_g); put(" steps=");
    for (int i = 0; i < 4; i++) { if (i) put(","); put(trail[i]); }
    put(" moves="); put_int(moves);
    log_out();
    step = DONE;
}

static void start(void) {
    step = WEIGH;
    weight_g = -1;
    weigh_since = weigh_last = 0;
    label_len = 0;
    label[0] = 0;
    drawn = 0;
    waiting = 0;
    for (int i = 0; i < 4; i++) trail[i][0] = 0;
}

/* The panel of this node, when the pod holds the display here */
static void size_display(void) {
    int32_t fmt = 0;
    display_rc = k32_display_info(&screen_w, &screen_h, &fmt);
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    start();
    moves = 0;
    size_display();
    return 0;
}

/* On the new node: its screen shows nothing of the job yet, and the next
 * tick asks for the step's devices again */
K32_EXPORT("k32_restored") int32_t k32_restored(void) {
    moves++;
    drawn = 0;
    waiting = 0;
    size_display();
    out_len = 0;
    put("restored on "); put(env("NODE_NAME", "?")); put(" at step "); put(step_names[step]);
    log_out();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t now) {
    (void)now;
    switch (step) {
    case WEIGH: weigh(); break;
    case LABEL: take_label(); break;
    case CONFIRM: confirm(); break;
    case LOG: log_record(); break;
    default: break;
    }
    return 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* req = (const char*)(uintptr_t)ptr;
    if (len >= 11 && req[0] == 'P' && req[6] == 'a' && req[7] == 'g' && step == DONE) start();
    out_len = 0;
    put("{\"step\":"); put_str(step_names[step]);
    put(",\"node\":"); put_str(env("NODE_NAME", "?"));
    put(",\"weight_g\":"); put_int(weight_g);
    put(",\"label\":"); put_str(label);
    put(",\"trail\":[");
    for (int i = 0; i < 4; i++) { if (i) put(","); put_str(trail[i]); }
    put("],\"moves\":"); put_int(moves);
    put(",\"need\":"); put_int(need_rc);
    put(",\"display\":"); put_int(display_rc);
    put(",\"led\":"); put_int(led_rc);
    put(",\"tone\":"); put_int(tone_rc);
    put("}");
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
    return 200;
}
