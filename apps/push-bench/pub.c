/* push-pub: publishes frames of a given size at a given rate on a channel
 * topic, each frame split into chunks of at most CHUNK bytes (push.h).
 *
 * After DELAY seconds it runs every size in SIZES at every rate in RATES,
 * SECONDS seconds a run with PAUSE seconds between runs, then logs "push
 * done" and stays idle. A frame's chunks go out PER_TICK at a time, one
 * batch every 10 ms tick (0: the whole frame in one tick). A frame that
 * comes due while the previous one is still going out waits for it; frames
 * more than one interval behind are skipped and counted. Per run it logs:
 *
 *   push sent run=1 size=4600 fps=6 chunk=200 count=25 frames=360 chunks=9000
 *        skipped=0 emit_err=0 (first rc=0) frame_send_ms mean=.. max=..
 *
 * With PEAK and PEAK_EVERY, every PEAK_EVERY-th whole frame is PEAK bytes.
 * Messages on the topics of the pod's kube32.io/event-topics (the
 * subscriber's input messages) are counted: inputs= in each line.
 *
 * Environment: TOPIC (default doom.draw), SIZES (default 4600,12600), RATES
 * (default 6,8,10), CHUNK (default 200, the event limit; 17 to 200; 0 sends
 * each frame whole with k32_publish, at most 12,800 bytes),
 * PER_TICK (default 0), SECONDS (default 60), PAUSE (default 5), DELAY
 * (default 10). Runs in 32 KiB of linear memory. */
#include "../../include/runtime/k32_app_abi.h"
#include "push.h"

#define NOINLINE __attribute__((noinline))
#define MAX_LIST 6

static uint8_t ev[PUSH_HDR + 12800];   /* a whole frame (CHUNK=0) or one chunk */
static char topic[64];
static uint32_t topic_len;
static char line[256];
static uint32_t line_len;

static uint32_t sizes[MAX_LIST], n_sizes, rates[MAX_LIST], n_rates;
static uint32_t chunk = 200, per_tick, seconds = 60, pause_s = 5, delay_s = 10, peak, peak_every, base_size, inputs;
static unsigned char in_block[96];

/* run state; times are 32-bit milliseconds */
static uint32_t phase, run, t_begin, t_run, t_pause, t_frame, frame, idx, count, per, size, fps, sending;
static uint32_t frames, chunks, skipped, emit_err, first_rc, send_ms_sum, send_ms_max, t_sent_unix;

static NOINLINE uint32_t now_ms(void) { return (uint32_t)k32_time_ms(); }

static NOINLINE void put(const char* s) {
    while (*s && line_len < sizeof line - 1) line[line_len++] = *s++;
}
static NOINLINE void putn(int32_t v) {
    char tmp[12];
    int t = 0;
    uint32_t u = v < 0 ? (uint32_t)-v : (uint32_t)v;
    if (v < 0) put("-");
    do { tmp[t++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (t && line_len < sizeof line - 1) line[line_len++] = tmp[--t];
}
static NOINLINE void flush(void) {
    k32_log(line, line_len);
    line_len = 0;
}

static NOINLINE uint32_t parse_list(const char* key, uint32_t key_len, uint32_t* out, const uint32_t* def, uint32_t n_def) {
    char s[64];
    int32_t n = k32_env_get(key, key_len, s, sizeof s);
    if (n <= 0) {
        for (uint32_t i = 0; i < n_def; i++) out[i] = def[i];
        return n_def;
    }
    uint32_t pos = 0, k = 0;
    while (pos < (uint32_t)n && k < MAX_LIST) {
        while (pos < (uint32_t)n && (s[pos] < '0' || s[pos] > '9')) pos++;
        if (pos >= (uint32_t)n) break;
        uint32_t v = 0;
        while (pos < (uint32_t)n && s[pos] >= '0' && s[pos] <= '9') v = v * 10u + (uint32_t)(s[pos++] - '0');
        out[k++] = v;
    }
    return k;
}

static NOINLINE uint32_t env_u(const char* key, uint32_t key_len, uint32_t def) {
    uint32_t v[MAX_LIST];
    return parse_list(key, key_len, v, &def, 1) ? v[0] : def;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    static const uint32_t def_sizes[2] = {4600, 12600}, def_rates[3] = {6, 8, 10};
    int32_t n = k32_env_get("TOPIC", 5, topic, sizeof topic);
    if (n <= 0) {
        const char* d = "doom.draw";
        n = 0;
        while (d[n]) { topic[n] = d[n]; n++; }
    }
    topic_len = (uint32_t)n;
    n_sizes = parse_list("SIZES", 5, sizes, def_sizes, 2);
    n_rates = parse_list("RATES", 5, rates, def_rates, 3);
    chunk = env_u("CHUNK", 5, 200);
    if (chunk != 0 && (chunk < PUSH_HDR + 1 || chunk > K32_MAX_EVENT_BYTES)) chunk = K32_MAX_EVENT_BYTES;
    per_tick = env_u("PER_TICK", 8, 0);
    peak = env_u("PEAK", 4, 0);
    if (peak > sizeof ev - PUSH_HDR) peak = sizeof ev - PUSH_HDR;
    peak_every = env_u("PEAK_EVERY", 10, 0);
    seconds = env_u("SECONDS", 7, 60);
    pause_s = env_u("PAUSE", 5, 5);
    delay_s = env_u("DELAY", 5, 10);
    t_begin = now_ms();
    put("push start sizes="); putn((int32_t)n_sizes); put(" rates="); putn((int32_t)n_rates);
    put(" chunk="); putn((int32_t)chunk); put(" per_tick="); putn((int32_t)per_tick);
    put(" seconds="); putn((int32_t)seconds);
    flush();
    return 0;
}

static NOINLINE void begin_run(uint32_t now) {
    uint32_t r = run;   /* runs are numbered from 1; run r covers size (r-1)/n_rates, rate (r-1)%n_rates */
    size = base_size = sizes[(r - 1) / n_rates];
    fps = rates[(r - 1) % n_rates] ? rates[(r - 1) % n_rates] : 1;
    if (size > sizeof ev - PUSH_HDR) size = sizeof ev - PUSH_HDR;
    if (chunk == 0) {   /* the whole frame in one k32_publish */
        per = 0;
        count = 1;
    } else {
        per = chunk - PUSH_HDR;
        count = (size + per - 1) / per;
    }
    if (count > PUSH_MAX_CHUNKS) {
        per = (size + PUSH_MAX_CHUNKS - 1) / PUSH_MAX_CHUNKS;
        count = (size + per - 1) / per;
    }
    t_run = now;
    frame = 0;
    sending = 0;
    frames = chunks = skipped = emit_err = first_rc = send_ms_sum = send_ms_max = 0;
}

static NOINLINE void end_run(void) {
    put("push sent run="); putn((int32_t)run); put(" size="); putn((int32_t)size); put(" fps="); putn((int32_t)fps);
    put(" chunk="); putn((int32_t)(per ? per + PUSH_HDR : PUSH_HDR + size)); put(" count="); putn((int32_t)count);
    put(" frames="); putn((int32_t)frames); put(" chunks="); putn((int32_t)chunks);
    put(" skipped="); putn((int32_t)skipped); put(" emit_err="); putn((int32_t)emit_err);
    put(" (first rc="); putn((int32_t)first_rc); put(")");
    put(" frame_send_ms mean="); putn(frames ? (int32_t)(send_ms_sum / frames) : 0);
    put(" max="); putn((int32_t)send_ms_max);
    put(" inputs="); putn((int32_t)inputs);
    inputs = 0;
    flush();
}

static NOINLINE void emit_chunk(void) {
    uint32_t off = idx * per, len = per == 0 ? size : size - off < per ? size - off : per;
    ev[0] = 'D';
    ev[1] = (uint8_t)run;
    ev[2] = (uint8_t)idx;
    ev[3] = (uint8_t)count;
    push_wr32(ev + 4, frame);
    push_wr32(ev + 8, t_sent_unix);
    ev[12] = (uint8_t)size;
    ev[13] = (uint8_t)(size >> 8);
    ev[14] = (uint8_t)fps;
    ev[15] = (uint8_t)per;
    for (uint32_t i = 0; i < len; i++) ev[PUSH_HDR + i] = push_byte(frame, off + i);
    int32_t rc = per == 0 ? k32_publish(topic, topic_len, ev, PUSH_HDR + len) : k32_event_emit(topic, topic_len, ev, PUSH_HDR + len);
    if (rc < 0) {
        if (!emit_err) first_rc = (uint32_t)rc;
        emit_err++;
    }
    chunks++;
    idx++;
}

/* Due time of frame f in this run */
static NOINLINE uint32_t due(uint32_t f) { return t_run + f * 1000u / fps; }

static NOINLINE void step(uint32_t now) {
    if (sending) {
        uint32_t n = per_tick ? per_tick : count;
        while (n-- && idx < count) emit_chunk();
        if (idx >= count) {
            uint32_t ms = now_ms() - t_frame;
            send_ms_sum += ms;
            if (ms > send_ms_max) send_ms_max = ms;
            sending = 0;
            frames++;
            frame++;
        }
        return;
    }
    if (now - t_run >= seconds * 1000u) {
        end_run();
        t_pause = now;
        phase = 2;
        return;
    }
    if ((int32_t)(now - due(frame)) < 0) return;
    while ((int32_t)(now - due(frame + 1)) >= 0) { frame++; skipped++; }
    t_frame = now;
    /* whole frames: every PEAK_EVERY-th frame is PEAK bytes */
    if (per == 0) size = peak && peak_every && frame % peak_every == 0 ? peak : base_size;
    t_sent_unix = (uint32_t)k32_unix_time_ms();
    idx = 0;
    sending = 1;
    step(now);
}

/* Messages on the input topic (event-topics): counted per run */
K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t n) {
    return n <= sizeof in_block ? (uint32_t)(uintptr_t)in_block : 0;
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t tp, uint32_t tl, uint32_t ptr, uint32_t len) {
    (void)tp; (void)tl; (void)ptr;
    if (len > 0) inputs++;
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    uint32_t now = now_ms();
    switch (phase) {
    case 0:   /* the subscriber gets DELAY seconds to start */
        if (now - t_begin < delay_s * 1000u) return 1;
        run = 1;
        begin_run(now);
        phase = 1;
        return 1;
    case 1:
        step(now);
        return 1;
    case 2:
        if (now - t_pause < pause_s * 1000u) return 1;
        if (++run > n_sizes * n_rates) {
            put("push done"); flush();
            phase = 3;
            return 0;
        }
        begin_run(now);
        phase = 1;
        return 1;
    default:
        return 0;
    }
}
