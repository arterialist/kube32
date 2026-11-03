/* wordsketch-listen: the microphone, the keyword spotter and the screen of
 * wordsketch, as one pod for a board without PSRAM.
 *
 * k32_tick reads the microphone every 10 ms and turns each 20 ms of audio
 * into a frame of 40 log-mel values (models/kws/frontend.c), kept as the
 * network's int8 input; the last 49 frames are the spotter's second. A
 * frame's mean log energy against the room's floor marks where a word
 * starts and ends; a sound that stays loud for 0.9 s is no word and becomes
 * the new floor. When the word sits in the middle of the window, or a
 * quarter second after its end, the spotter (models/kws/kws_infer.c)
 * classifies the window, at the level of the training clips: as if the
 * audio were scaled so that the word's peak is 16,000, which adds
 * 512 log2(16000 / peak) to every log-mel value. A keyword with at least
 * THRESHOLD percent:
 *
 *   - goes out as an event on wordsketch.word, which the draw pod turns into
 *     a doodle, every step's image on wordsketch.frame;
 *   - starts a story at STORY_URL, "Once upon a time there was a <word>.",
 *     whose tokens come back on wordsketch.text;
 *   - plays a short chime (the microphone stops first: speaker and
 *     microphone share the I2S clock pin).
 *
 * The screen shows the drawing forming from noise at 4x on the left and the
 * story on the right. Once both are done the pod listens again and logs the
 * word's timing, measured on this node's clock from the word's last loud
 * frame.
 *
 * The weights stay in the blob wordsketch-kws (flash, read in place by
 * k32_matvec_i8); the tables, scales and biases come into memory.
 *
 * Environment: STORY_URL (default http://wordsketch-teller/generate),
 * STORY_STEPS (80), STEPS of the drawing, 8 or 16 (8), THRESHOLD (60),
 * VOLUME of the chime in percent of the speaker's ceiling (30; 0 is none),
 * VAD, a word's mean log2 energy above the floor in Q8 (512).
 *
 *   POST /word?w=cat[&seed=7]   as if the word was heard */
#include "../../include/runtime/k32_app_abi.h"
#include "../../models/kws/kws_infer.h"
#include "blobload.h"

#define NOINLINE __attribute__((noinline))
#define LIT(s) (s), (uint32_t)(sizeof(s) - 1)

#define N_WORDS 6
static const char* const kNames[KWS_CLASSES] = {"cat", "dog", "bird", "house", "tree", "bed", "unknown", "silence"};

/* screen: 240 x 135, the picture at 4x on the left, the story on the right */
#define PIC_X 2
#define PIC_Y 12
#define TXT_X 118
#define TXT_Y 12
#define COLS 20
#define ROWS 15
#define WHITE 0xFFFFu
#define GREY 0x8410u
#define BLACK 0x0000u
#define ACCENT 0xFD20u

static char g_url[96] = "http://wordsketch-teller/generate";
static uint32_t g_url_len = 33, g_story_steps, g_steps, g_threshold, g_volume;
static int32_t g_vad;

/* ---- text out --------------------------------------------------------------- */
static char g_out[200];
static uint32_t g_out_len;
static NOINLINE void put(const char* s) {
    while (*s && g_out_len < sizeof g_out) g_out[g_out_len++] = *s++;
}
static NOINLINE void put_i(int32_t v) {
    char d[12];
    int k = 0;
    if (v < 0) { put("-"); v = -v; }
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && g_out_len < sizeof g_out) g_out[g_out_len++] = d[--k];
}
static NOINLINE void put_n(uint32_t v) {
    put_i((int32_t)v);
    put(" ");
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

/* ---- the model: kws_load's walk over the blob, the weights left in it ------- */
static struct kws_model g_m;
static uint8_t g_arena[4720];

static NOINLINE int32_t load_model(void) {
    const int32_t b = k32_blob_open(LIT("wordsketch-kws"));
    if (b < 0) return b;
    uint32_t h[16];
    if (k32_blob_read(b, 0, h, sizeof h) != sizeof h || h[0] != 0x4B32334Bu || h[1] != 1 || h[2] < 3 || h[2] > KWS_MAX_CONV ||
        h[3] != KWS_CLASSES)
        return K32_ERR_INVAL;
    struct blobload r;
    blobload_begin(&r, b, h[7], g_arena, sizeof g_arena);
    blobload_copy(&r, 64);
    g_m.n_conv = h[2];
    g_m.n_classes = h[3];
    g_m.offset = (int32_t)h[6];
    /* hann, twiddles, the mel filters' starts and lengths, then their weights */
    if (!blobload_copy(&r, 2 * (KWS_WIN + KWS_NFFT + 2 * KWS_MELS))) return K32_ERR_INVAL;
    kws_tables_from_blob(&g_m.fe, g_arena);
    uint32_t mel_w = 0;
    for (uint32_t i = 0; i < KWS_MELS; i++) mel_w += g_m.fe.mel_len[i];
    blobload_copy(&r, 2 * mel_w);
    for (uint32_t i = 0; i < g_m.n_conv; i++) {
        struct kws_conv* c = &g_m.conv[i];
        const uint32_t* d = blobload_copy(&r, 32);
        const float* s = blobload_copy(&r, 8);
        if (!d || !s || d[1] > 64 || d[2] * d[3] * d[0] > 512) return K32_ERR_INVAL;
        c->ci = d[0]; c->co = d[1]; c->kh = d[2]; c->kw = d[3];
        c->sh = d[4]; c->sw = d[5]; c->ph = d[6]; c->pw = d[7];
        c->scale = s[0];
        c->inv = s[1];
        c->ws = blobload_copy(&r, 4 * c->co);
        c->b = blobload_copy(&r, 4 * c->co);
        c->w = blobload_map(&r, c->co * c->kh * c->kw * c->ci);
    }
    const uint32_t* d = blobload_copy(&r, 8);
    if (!d || d[1] != KWS_CLASSES) return K32_ERR_INVAL;
    g_m.fc_ci = d[0];
    g_m.fc_co = d[1];
    g_m.fc_ws = blobload_copy(&r, 4 * KWS_CLASSES);
    g_m.fc_b = blobload_copy(&r, 4 * KWS_CLASSES);
    g_m.fc_w = blobload_map(&r, KWS_CLASSES * g_m.fc_ci);
    return blobload_end(&r) ? K32_ERR_INVAL : (int32_t)r.used;
}

/* ---- the word on the screen and its timing --------------------------------- */
static uint32_t g_id, g_cls, g_p, g_seed;
static uint32_t g_t_end, g_t_word, g_t_noise, g_t_step1, g_t_drawn, g_t_tok0, g_t_tok;
static uint32_t g_frames, g_tokens, g_rate;
static int32_t g_story_rc;

static uint8_t g_pic[784];      /* levels 0..63 as drawn, 255 before the first */
static uint8_t g_part_step[4];  /* the step each band of 7 rows shows, 255 before the first */
static uint32_t g_frames_late;
static char g_line[COLS];
static uint32_t g_col, g_row, g_pcols;   /* g_pcols: the prompt's characters at the line's start */

static NOINLINE void status(const char* s) {
    g_out_len = 0;
    if (g_p) { put(kNames[g_cls]); put(" "); put_i((int32_t)g_p); put("%  "); }
    put(s);
    while (g_out_len < 40) g_out[g_out_len++] = ' ';
    k32_display_text(0, 1, g_out, 40, g_p ? ACCENT : GREY, BLACK);
    g_out_len = 0;
}

static NOINLINE void draw_line(void) {
    char pad[COLS];
    for (uint32_t i = 0; i < COLS; i++) pad[i] = i < g_col ? g_line[i] : ' ';
    const uint32_t pc = g_pcols < g_col ? g_pcols : g_col;
    const int32_t y = TXT_Y + 8 * (int32_t)g_row;
    if (pc) k32_display_text(TXT_X, y, pad, pc, GREY, BLACK);
    k32_display_text(TXT_X + 6 * (int32_t)pc, y, pad + pc, COLS - pc, WHITE, BLACK);
}

static NOINLINE void new_line(void) {
    if (++g_row == ROWS) {
        k32_display_rect(TXT_X, TXT_Y, 240 - TXT_X, 8 * ROWS, BLACK);
        g_row = 0;
    }
    g_col = g_pcols = 0;
}

/* one character of the story, wrapping at the last space of a full line */
static NOINLINE void text_char(char c, int prompt) {
    if (c == '\n') { draw_line(); new_line(); return; }
    if (c == ' ' && !g_col) return;
    if (g_col == COLS) {
        uint32_t cut = COLS;
        while (cut && g_line[cut - 1] != ' ') cut--;
        if (!cut) cut = COLS;
        const uint32_t keep = g_col, pcols = g_pcols;
        g_col = cut;
        draw_line();
        new_line();
        for (uint32_t i = cut; i < keep; i++) g_line[g_col++] = g_line[i];
        g_pcols = pcols > cut ? pcols - cut : 0;
        if (c == ' ' && !g_col) return;
    }
    g_line[g_col++] = (uint8_t)c >= 0x20 && (uint8_t)c < 0x7f ? c : '?';
    if (prompt) g_pcols = g_col;
}

/* ---- listening -------------------------------------------------------------- */
enum { LISTEN, SHOW };
static uint32_t g_state, g_show_since, g_show_last, g_done_at, g_drawn, g_told;

static int16_t g_pcm[KWS_WIN];
static uint32_t g_have;                       /* samples in g_pcm */
static int32_t g_dc;                          /* the decimator's DC offset, Q8 */
static int8_t g_q[KWS_FRAMES * KWS_MELS];     /* the last second, as the network's input */
static uint16_t g_pk[KWS_FRAMES];           /* each frame's new samples' peak */
static uint32_t g_frame, g_in_word, g_onset, g_loud, g_fire_at;
static int32_t g_floor;                       /* the room's mean log2 energy, Q8 */

static NOINLINE void listen_start(void) {
    g_have = g_frame = g_in_word = g_fire_at = 0;
    g_floor = 0x3fffffff;
    __builtin_memset(g_q, 0, sizeof g_q);
    __builtin_memset(g_pk, 0, sizeof g_pk);
    g_state = LISTEN;
    status("listening");
}

static NOINLINE void accept_word(uint32_t cls, uint32_t p, uint32_t seed);

/* 256 log2 x */
static NOINLINE int32_t log2_q8(uint32_t x) {
    const int32_t n = 31 - __builtin_clz(x | 1u);
    const uint32_t frac = n >= 8 ? (x >> (n - 8)) & 255u : (x << (8 - n)) & 255u;
    return 256 * n + (int32_t)frac;
}

static NOINLINE void classify(void) {
    const uint32_t t0 = now_ms();
    /* the word's peak, from its first to its last loud frame */
    const uint32_t n = g_frame;
    uint32_t peak = 0;
    for (uint32_t k = g_onset; k <= g_loud; k++) {
        const uint32_t i = KWS_FRAMES - 1 - (n - k);
        if (n - k < KWS_FRAMES && g_pk[i] > peak) peak = g_pk[i];
    }
    if (peak < 64) peak = 64;   /* at most 250x */
    const float d = (float)(2 * (log2_q8(16000) - log2_q8(peak))) * g_m.conv[0].inv;
    const int32_t dq = (int32_t)(d + (d >= 0.0f ? 0.5f : -0.5f));
    for (uint32_t i = 0; i < sizeof g_q; i++) {
        const int32_t v = g_q[i] + dq;
        g_q[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
    float pr[KWS_CLASSES];
    kws_classify_q(&g_m, g_q, pr);
    k32_softmax_f32(pr, KWS_CLASSES);
    uint32_t best = 0, second = 1;
    for (uint32_t k = 1; k < KWS_CLASSES; k++)
        if (pr[k] > pr[best]) best = k;
    if (best == 1) second = 0;
    for (uint32_t k = 0; k < KWS_CLASSES; k++)
        if (k != best && pr[k] > pr[second]) second = k;
    const uint32_t p = (uint32_t)(pr[best] * 100.0f + 0.5f), t = now_ms();
    put("wordsketch-listen: heard "); put(kNames[best]); put(" "); put_n(p); put(kNames[second]); put(" ");
    put_n((uint32_t)(pr[second] * 100.0f + 0.5f)); put("word_ms "); put_n(20 * (g_loud - g_onset + 1));
    put("end_to_result_ms "); put_n(t - g_t_end); put("network_ms "); put_n(t - t0); put("peak "); put_n(peak);
    put("gain_lsb "); put_i(dq);
    say();
    if (best < N_WORDS && p >= g_threshold) {
        g_t_word = t;
        accept_word(best, p, 0);
    }
}

/* one frame of 480 samples: its features, its energy, the word's start and end */
static NOINLINE void frame(void) {
    int32_t f[KWS_MELS];
    kws_frame_buf(&g_m.fe, g_pcm, f, kws_scratch());   /* the FFT in the network's map */
    __builtin_memmove(g_q, g_q + KWS_MELS, sizeof g_q - KWS_MELS);
    kws_quantize(&g_m, f, KWS_MELS, g_q + sizeof g_q - KWS_MELS);
    __builtin_memmove(g_pk, g_pk + 1, sizeof g_pk - sizeof g_pk[0]);
    int32_t pk = 0;
    for (uint32_t i = KWS_WIN - KWS_HOP; i < KWS_WIN; i++) {
        const int32_t a = g_pcm[i] < 0 ? -g_pcm[i] : g_pcm[i];
        if (a > pk) pk = a;
    }
    g_pk[KWS_FRAMES - 1] = (uint16_t)pk;
    int32_t e = 0;
    for (uint32_t k = 0; k < KWS_MELS; k++) e += f[k];
    e /= KWS_MELS;
    const uint32_t n = ++g_frame;
    if (n < 8) return;   /* the decimator settles */
    /* the floor follows quieter frames in a few frames and louder ones in
     * seconds, so one dropout does not set it */
    if (g_floor == 0x3fffffff) g_floor = e;
    if (e < g_floor) g_floor -= (g_floor - e) / 4;
    if (!g_in_word) {
        g_floor += (e - g_floor) / 64;
        if (n >= 16 && e > g_floor + g_vad) {
            g_in_word = 1;
            g_onset = g_loud = n;
            g_t_end = now_ms();
        }
        return;
    }
    if (e > g_floor + g_vad / 2) {
        g_loud = n;
        g_t_end = now_ms();
    }
    /* the word's middle in the window's middle, at least a quarter second
     * after its end, while the window still holds its start */
    if (!g_fire_at && (n - g_loud >= 8 || n - g_onset >= KWS_FRAMES - 2)) {
        uint32_t at = (g_onset + g_loud) / 2 + KWS_FRAMES / 2;
        if (at < g_loud + 12) at = g_loud + 12;
        if (at > g_onset + KWS_FRAMES - 2) at = g_onset + KWS_FRAMES - 2;
        g_fire_at = at > n ? at : n;
    }
    if (g_fire_at && n >= g_fire_at) {
        g_fire_at = g_in_word = 0;
        if (g_loud - g_onset >= 45) {
            /* loud for 0.9 s or more: a tone or the room, the new floor */
            g_floor = e;
            put("wordsketch-listen: a sustained sound, the floor follows it");
            say();
        } else if (g_loud - g_onset >= 4) {
            classify();   /* 100 ms or more: no clicks */
        }
    }
}

static NOINLINE void hear(void) {
    for (int guard = 0; guard < 8 && g_state == LISTEN; guard++) {
        const int32_t n = k32_mic_read(g_pcm + g_have, KWS_WIN - g_have, 0);
        if (n <= 0) return;
        for (uint32_t i = g_have; i < g_have + (uint32_t)n; i++) {
            const int32_t x = (int32_t)g_pcm[i] * 256;
            g_dc += (x - g_dc) >> 10;
            const int32_t y = (x - g_dc) >> 8;
            g_pcm[i] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        }
        g_have += (uint32_t)n;
        if (g_have < KWS_WIN) continue;
        frame();
        __builtin_memmove(g_pcm, g_pcm + KWS_HOP, sizeof(int16_t) * (KWS_WIN - KWS_HOP));
        g_have = KWS_WIN - KWS_HOP;
    }
}

/* ---- a keyword: the drawing, the story, the chime ------------------------- */
static NOINLINE void accept_word(uint32_t cls, uint32_t p, uint32_t seed) {
    k32_mic_read(g_pcm, 0, 0);   /* the speaker needs the clock pin */
    g_id++;
    if (!seed) seed = ((uint32_t)k32_time_ms() * 2654435761u) >> 1 | 1u;
    g_cls = cls;
    g_p = p;
    g_seed = seed;
    g_state = SHOW;
    g_show_since = g_show_last = now_ms();
    g_t_noise = g_t_step1 = g_t_drawn = g_t_tok0 = g_frames = g_tokens = g_rate = 0;
    g_drawn = g_told = g_done_at = 0;
    /* "W <id> <class> <seed> <steps>" */
    put("W "); put_n(g_id); put_n(cls); put_n(seed); put_i((int32_t)g_steps);
    k32_event_emit(LIT("wordsketch.word"), g_out, g_out_len);
    g_out_len = 0;
    put("{\"prompt\":\"Once upon a time there was a "); put(kNames[cls]); put(".\",\"steps\":"); put_i((int32_t)g_story_steps); put("}");
    char resp[160];   /* the coordinator answers with its state */
    uint32_t got = 0;
    g_story_rc = k32_http_call(LIT("POST"), g_url, g_url_len, "", 0, (const uint8_t*)g_out, g_out_len, (uint8_t*)resp, sizeof resp, &got, 3000);
    g_out_len = 0;
    /* the screen: a frame for the picture, the word, an empty story */
    k32_display_rect(0, 0, 240, 135, BLACK);
    k32_display_rect(PIC_X - 1, PIC_Y - 1, 114, 114, GREY);
    __builtin_memset(g_pic, 255, sizeof g_pic);
    __builtin_memset(g_part_step, 255, sizeof g_part_step);
    g_frames_late = 0;
    g_col = g_row = g_pcols = 0;
    status("drawing");
    if (g_volume) {
        k32_tone(1047, 60, g_volume);
        k32_tone(1568, 100, g_volume);
    }
}

static NOINLINE void report(void) {
    put("wordsketch-listen: word "); put_i((int32_t)g_id); put(" "); put(kNames[g_cls]); put(" "); put_i((int32_t)g_p);
    put("% seed "); put_i((int32_t)g_seed); put(": from its end, result "); put_i((int32_t)(g_t_word - g_t_end));
    put(" ms, noise "); put_i((int32_t)(g_t_noise - g_t_end)); put(" ms, step 1 "); put_i((int32_t)(g_t_step1 - g_t_end));
    put(" ms, drawn "); put_i((int32_t)(g_t_drawn - g_t_end)); put(" ms ("); put_i((int32_t)g_frames); put("/");
    put_i((int32_t)(4 * (g_steps + 1))); put(" frames, "); put_i((int32_t)g_frames_late); put(" late); story "); put_i(g_story_rc); put(", "); put_i((int32_t)g_tokens);
    put(" tokens, first "); put_i((int32_t)(g_t_tok0 - g_t_end)); put(" ms, last "); put_i((int32_t)(g_t_tok - g_t_end));
    put(" ms, "); put_i((int32_t)g_rate); put(" tok/s x100");
    say();
}

/* ---- events: the drawing's frames, the story's tokens ---------------------- */
/* 'F', 'a' + id % 16, 'a' + step, '0' + part, 196 levels '0' + 0..63 */
static NOINLINE void on_frame(const char* d) {
    const uint32_t step = (uint32_t)(d[2] - 'a'), part = (uint32_t)(d[3] - '0');
    if (d[0] != 'F' || (uint32_t)(d[1] - 'a') != (g_id & 15u) || step > g_steps || part > 3) return;
    /* a band shows its newest step: a repeated or older frame is late */
    if (g_part_step[part] != 255 && step <= g_part_step[part]) {
        g_frames_late++;
        return;
    }
    g_part_step[part] = (uint8_t)step;
    const uint32_t t = now_ms();
    g_show_last = t;
    if (!g_frames++) g_t_noise = t;
    if (step == 1 && !g_t_step1) g_t_step1 = t;
    /* runs of changed pixels of one level, one rectangle each */
    for (uint32_t r = 0; r < 7; r++) {
        const uint32_t y = part * 7 + r;
        for (uint32_t x = 0; x < 28;) {
            const uint32_t lv = (uint32_t)(uint8_t)d[4 + r * 28 + x] - 48u;
            const int32_t dl = (int32_t)lv - g_pic[y * 28 + x];
            if (lv > 63 || (dl > -2 && dl < 2)) { x++; continue; }
            uint32_t run = 1;
            while (x + run < 28 && (uint32_t)(uint8_t)d[4 + r * 28 + x + run] - 48u == lv) run++;
            __builtin_memset(g_pic + y * 28 + x, (int)lv, run);
            const uint32_t g = 63u - lv;   /* paper: -1 (level 0) is white */
            k32_display_rect(PIC_X + 4 * (int32_t)x, PIC_Y + 4 * (int32_t)y, 4 * (int32_t)run, 4, ((g >> 1) << 11) | (g << 5) | (g >> 1));
            x += run;
        }
    }
    if (step == g_steps) {
        g_t_drawn = t;
        g_drawn = 1;
    }
    if (part != 3) return;
    put("drawing "); put_i((int32_t)step); put("/"); put_i((int32_t)g_steps);
    char st[16];
    for (uint32_t i = 0; i < 16; i++) st[i] = i < g_out_len ? g_out[i] : 0;
    st[15] = 0;
    status(g_drawn ? (g_told ? "done" : "telling") : st);
}

/* where the value of key=... starts in a query, or n */
static NOINLINE uint32_t field_eq(const char* d, uint32_t n, const char* key) {
    for (uint32_t i = 1; i < n; i++) {
        if (d[i - 1] != '?' && d[i - 1] != '&') continue;
        uint32_t k = 0;
        while (key[k] && i + k < n && d[i + k] == key[k]) k++;
        if (!key[k]) return i + k;
    }
    return n;
}

/* where the value of "key": starts in d, or n */
static NOINLINE uint32_t field(const char* d, uint32_t n, char key) {
    for (uint32_t i = 0; i + 4 < n; i++)
        if (d[i] == '"' && d[i + 1] == key && d[i + 2] == '"' && d[i + 3] == ':') return i + 4;
    return n;
}

/* {"t":"token","s":S,"k":"p"|"g","n":N,"r":R,"x":"..."} and {"t":"done",...} */
static NOINLINE void on_text(const char* d, uint32_t n) {
    const uint32_t t = field(d, n, 't');
    if (t + 2 >= n) return;
    const uint32_t now = now_ms();
    g_show_last = now;
    if (d[t + 1] == 'd') {
        if (g_col) draw_line();
        g_told = 1;
        if (g_drawn) status("done");
        return;
    }
    if (d[t + 1] != 't') return;
    const uint32_t k = field(d, n, 'k');
    const int prompt = k + 1 < n && d[k + 1] == 'p';
    for (uint32_t i = field(d, n, 'x') + 1; i < n && d[i] != '"'; i++) {
        char c = d[i];
        if (c == '\\' && i + 1 < n) {
            c = d[++i];
            if (c == 'n') c = '\n';
            else if (c == 'u') { i += 4; c = '?'; }
        }
        text_char(c, prompt);
    }
    draw_line();
    if (!prompt) {
        if (!g_tokens++) g_t_tok0 = now;
        g_t_tok = now;
        const uint32_t r = field(d, n, 'r');
        g_rate = atou(d + r, n - r);
    }
}

/* ---- exports ---------------------------------------------------------------- */
static unsigned char g_block[256];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof g_block ? (uint32_t)(uintptr_t)g_block : 0;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    const int32_t n = k32_env_get(LIT("STORY_URL"), g_url, sizeof g_url);
    if (n > 0) g_url_len = (uint32_t)n;
    g_story_steps = env_u(LIT("STORY_STEPS"), 80);
    g_steps = env_u(LIT("STEPS"), 8);
    g_threshold = env_u(LIT("THRESHOLD"), 60);
    g_volume = env_u(LIT("VOLUME"), 30);
    g_vad = (int32_t)env_u(LIT("VAD"), 512);
    /* words are numbered from a random start: the draw pod skips a number it
     * just drew, and a restarted pod must not repeat the last one */
    g_id = k32_random() & 0x3fffffffu;
    const int32_t rc = load_model();
    put("wordsketch-listen: model "); put(rc < 0 ? "not loaded: " : "loaded, tables "); put_i(rc); put(rc < 0 ? "" : " B");
    say();
    if (rc < 0) return 1;
    k32_display_rect(0, 0, 240, 135, BLACK);
    listen_start();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t tick_ms) {
    (void)tick_ms;   /* taken before the call waited for the pod: a word accepted in k32_handle meanwhile is newer */
    const uint32_t now = now_ms();
    if (g_state == LISTEN) {
        hear();
        return 1;   /* every 10 ms while listening */
    }
    /* back to listening a moment after the drawing and the story, or after
     * 60 s without progress, or after 15 s without a first frame or token
     * (the word or the story request did not get through); the picture
     * stays until the next word */
    if (g_drawn && g_told && !g_done_at) g_done_at = now;
    if ((g_done_at && now - g_done_at > 1500) || now - g_show_last > 60000 || (!g_frames && !g_tokens && now - g_show_since > 15000)) {
        report();
        listen_start();
        return 1;
    }
    return 0;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t tp, uint32_t tl, uint32_t ptr, uint32_t len) {
    const char* topic = (const char*)(uintptr_t)tp;
    if (g_state != SHOW || tl < 12) return 0;
    if (topic[11] == 'f' && len == 200) on_frame((const char*)(uintptr_t)ptr);   /* wordsketch.frame */
    else if (topic[11] == 't') on_text((const char*)(uintptr_t)ptr, len);        /* wordsketch.text */
    return 0;
}

/* POST /word?w=<word>[&seed=<n>] */
K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    uint32_t line = 0;
    while (line < len && r[line] != '\n') line++;
    const uint32_t w = field_eq(r, line, "w="), sd = field_eq(r, line, "seed=");
    uint32_t cls = 0;
    while (cls < N_WORDS) {
        uint32_t m = 0;
        while (kNames[cls][m] && w + m < line && r[w + m] == kNames[cls][m]) m++;
        if (!kNames[cls][m] && (w + m == line || r[w + m] == '&' || r[w + m] == ' ')) break;
        cls++;
    }
    int32_t status = 400;
    if (r[0] == 'P' && cls < N_WORDS && g_state == LISTEN) {
        status = 200;
        g_t_end = g_t_word = now_ms();
        accept_word(cls, 100, atou(r + sd, line - sd));
    }
    put("{\"word\":"); put_i(status == 200 ? (int32_t)g_id : -1); put(",\"seed\":"); put_i((int32_t)g_seed); put(",\"story\":"); put_i(g_story_rc); put("}");
    k32_resp_begin(status, LIT("application/json"));
    k32_resp_write(g_out, g_out_len);
    g_out_len = 0;
    k32_resp_end();
    return status;
}
