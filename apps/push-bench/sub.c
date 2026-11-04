/* push-sub: reassembles the frames push-pub publishes (push.h) and logs,
 * per run, how many arrive whole.
 *
 * Chunks arrive in k32_event. The pod reassembles up to WINDOW frames at a
 * time (default 1, at most 4): a chunk of a newer frame when every slot is
 * taken evicts the oldest frame, which counts as partial. A chunk of a frame
 * older than every slot counts as stale. Every chunk's bytes are checked
 * against push_byte. A run ends with the first chunk of the next run, or
 * after 3 s without chunks. Per run it logs:
 *
 *   push recv run=1 size=4600 fps=6 count=25 window=1 frames=360 whole=358
 *        partial=2 missing=0 whole_fps=5.96 chunk_loss_ppm=223 ok=.. dup=..
 *        stale=.. bad=.. reasm_ms p50=.. p95=.. max=.. e2e_ms p50=.. p95=..
 *        max=.. gap_ms max=..
 *
 * frames counts the frame numbers from the first to the last seen;
 * whole_fps is whole * fps / frames. reasm_ms runs from a frame's first
 * chunk to its last on this node's clock; e2e_ms from the publisher's wall
 * clock when the frame went out to this node's wall clock when it was
 * whole, so it includes the offset between the two clocks. gap_ms is the
 * longest time between two whole frames. A frame sent whole (k32_publish)
 * arrives as one event. Runs in 24 KiB of linear memory.
 *
 * Environment: WINDOW, CHECK (0: the bytes are not checked). */
#include "../../include/runtime/k32_app_abi.h"
#include "push.h"

#define NOINLINE __attribute__((noinline))
#define MAX_WINDOW 4
#define MAX_SAMPLES 640
#define WORDS ((PUSH_MAX_CHUNKS + 31) / 32)

static unsigned char block[PUSH_HDR + 12800 + 80];   /* topic + a whole frame */
static char line[400];
static uint32_t line_len;

struct slot {
    uint32_t used, frame, got, count, t_first, t_sent;
    uint32_t bits[WORDS];
};
static struct slot slots[MAX_WINDOW];
static uint32_t window = 1, check = 1;

/* run statistics */
static uint32_t run, size, fps, count, seen, first, last, t_last_chunk, t_last_whole;
static uint32_t ok, dup, stale, bad, whole, partial, gap_max, n_reasm;
static uint16_t reasm[MAX_SAMPLES];
static int16_t e2e[MAX_SAMPLES];

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t n) {
    return n <= sizeof block ? (uint32_t)(uintptr_t)block : 0;
}

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

K32_EXPORT("k32_init") int32_t k32_init(void) {
    char v[8];
    int32_t n = k32_env_get("WINDOW", 6, v, sizeof v);
    if (n > 0 && v[0] >= '1' && v[0] <= '0' + MAX_WINDOW) window = (uint32_t)(v[0] - '0');
    n = k32_env_get("CHECK", 5, v, sizeof v);
    if (n > 0 && v[0] == '0') check = 0;
    put("push sub window="); putn((int32_t)window); put(" check="); putn((int32_t)check);
    flush();
    return 0;
}

static NOINLINE void sort_u16(uint16_t* a, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        uint16_t x = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > x) { a[j] = a[j - 1]; j--; }
        a[j] = x;
    }
}
static NOINLINE void sort_i16(int16_t* a, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        int16_t x = a[i];
        uint32_t j = i;
        while (j > 0 && a[j - 1] > x) { a[j] = a[j - 1]; j--; }
        a[j] = x;
    }
}

static NOINLINE void put_pcts(const char* name, int32_t p50, int32_t p95, int32_t max) {
    put(" "); put(name); put(" p50="); putn(p50); put(" p95="); putn(p95); put(" max="); putn(max);
}

static NOINLINE void end_run(void) {
    for (uint32_t s = 0; s < window; s++) {
        if (slots[s].used) { partial++; slots[s].used = 0; }
    }
    uint32_t frames = seen ? last - first + 1 : 0;
    uint32_t missing = frames > whole + partial ? frames - whole - partial : 0;
    uint32_t expected = frames * count;
    uint32_t loss_ppm = expected > ok ? (uint32_t)(((uint64_t)(expected - ok) * 1000000u) / expected) : 0;
    uint32_t wfps100 = frames ? whole * fps * 100u / frames : 0;
    put("push recv run="); putn((int32_t)run); put(" size="); putn((int32_t)size); put(" fps="); putn((int32_t)fps);
    put(" count="); putn((int32_t)count); put(" window="); putn((int32_t)window);
    put(" frames="); putn((int32_t)frames); put(" whole="); putn((int32_t)whole);
    put(" partial="); putn((int32_t)partial); put(" missing="); putn((int32_t)missing);
    put(" whole_fps="); putn((int32_t)(wfps100 / 100)); put("."); if (wfps100 % 100 < 10) put("0"); putn((int32_t)(wfps100 % 100));
    put(" chunk_loss_ppm="); putn((int32_t)loss_ppm);
    put(" ok="); putn((int32_t)ok); put(" dup="); putn((int32_t)dup); put(" stale="); putn((int32_t)stale); put(" bad="); putn((int32_t)bad);
    uint32_t n = n_reasm;
    if (n) {
        sort_u16(reasm, n);
        sort_i16(e2e, n);
        put_pcts("reasm_ms", reasm[n / 2], reasm[n * 95 / 100], reasm[n - 1]);
        put_pcts("e2e_ms", e2e[n / 2], e2e[n * 95 / 100], e2e[n - 1]);
    }
    put(" gap_ms max="); putn((int32_t)gap_max);
    flush();
    seen = 0;
}

static NOINLINE void begin_run(const uint8_t* h) {
    run = h[1];
    count = h[3];
    size = h[12] | (uint32_t)h[13] << 8;
    fps = h[14];
    ok = dup = stale = bad = whole = partial = gap_max = n_reasm = 0;
    t_last_whole = 0;
    for (uint32_t s = 0; s < MAX_WINDOW; s++) slots[s].used = 0;
}

static NOINLINE int chunk_bytes_ok(const uint8_t* d, uint32_t len, uint32_t frame, uint32_t off) {
    for (uint32_t i = 0; i < len; i++) if (d[i] != push_byte(frame, off + i)) return 0;
    return 1;
}

static NOINLINE struct slot* slot_for(uint32_t frame, uint32_t now) {
    struct slot* oldest = 0;
    uint32_t newest = 0, any = 0;
    for (uint32_t s = 0; s < window; s++) {
        struct slot* sl = &slots[s];
        if (!sl->used) continue;
        if (sl->frame == frame) return sl;
        if (!oldest || sl->frame < oldest->frame) oldest = sl;
        if (!any || sl->frame > newest) newest = sl->frame;
        any = 1;
    }
    if (any && frame < oldest->frame) return 0;          /* older than the window */
    struct slot* sl = 0;
    for (uint32_t s = 0; s < window && !sl; s++) if (!slots[s].used) sl = &slots[s];
    if (!sl) {
        sl = oldest;                                    /* evicted: partial */
        partial++;
    }
    sl->used = 1;
    sl->frame = frame;
    sl->got = 0;
    sl->t_first = now;
    for (uint32_t w = 0; w < WORDS; w++) sl->bits[w] = 0;
    return sl;
}

static NOINLINE void on_chunk(const uint8_t* h, uint32_t len) {
    uint32_t now = now_ms();
    if (seen && h[1] != run) end_run();
    if (!seen) begin_run(h);
    uint32_t idx = h[2], frame = push_rd32(h + 4), per = h[15];
    if (!seen || frame < first) first = frame;
    if (!seen || frame > last) last = frame;
    seen = 1;
    t_last_chunk = now;
    if (idx >= count || count > PUSH_MAX_CHUNKS || (check && !chunk_bytes_ok(h + PUSH_HDR, len - PUSH_HDR, frame, idx * per))) {
        bad++;
        return;
    }
    struct slot* sl = slot_for(frame, now);
    if (!sl) { stale++; return; }
    uint32_t bit = 1u << (idx & 31u);
    if (sl->bits[idx >> 5] & bit) { dup++; return; }
    sl->bits[idx >> 5] |= bit;
    ok++;
    if (sl->got == 0) sl->t_sent = push_rd32(h + 8);
    if (++sl->got < count) return;
    /* whole */
    sl->used = 0;
    whole++;
    if (t_last_whole && now - t_last_whole > gap_max) gap_max = now - t_last_whole;
    t_last_whole = now;
    if (n_reasm < MAX_SAMPLES) {
        uint32_t r = now - sl->t_first;
        int32_t e = (int32_t)((uint32_t)k32_unix_time_ms() - sl->t_sent);
        reasm[n_reasm] = (uint16_t)(r > 65535u ? 65535u : r);
        e2e[n_reasm] = (int16_t)(e > 32767 ? 32767 : e < -32768 ? -32768 : e);
        n_reasm++;
    }
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t tp, uint32_t tl, uint32_t ptr, uint32_t len) {
    (void)tp; (void)tl;
    const uint8_t* h = (const uint8_t*)(uintptr_t)ptr;
    if (len < PUSH_HDR || h[0] != 'D') return 0;
    on_chunk(h, len);
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    if (seen && now_ms() - t_last_chunk > 3000u) end_run();
    return 0;
}
