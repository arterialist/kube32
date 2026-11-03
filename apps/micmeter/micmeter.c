/* micmeter: the microphone's level and a test sound through the speaker
 * (docs/peripherals.md#speaker-and-microphone).
 *
 *   GET /record?ms=<n>[&hz=<f>] records n ms (at most 10000) and answers the
 *                               mean (DC offset), RMS and peak around the mean
 *                               of every 250 ms window and of the whole take;
 *                               with hz, also each window's amplitude at f Hz
 *                               (a Goertzel filter), which picks a test tone
 *                               out of broadband room noise
 *   GET /play?hz=<f>&ms=<n>&vol=<pct>
 *                               a sine of f Hz for n ms (at most 2000) at pct
 *                               of the speaker's ceiling, streamed as PCM a
 *                               buffer at a time
 *
 * Needs hardware.kube32.io/microphone and hardware.kube32.io/speaker. */
#include "../../include/runtime/k32_app_abi.h"

#define NOINLINE __attribute__((noinline))

#define RATE 16000
#define WINDOW 4000          /* 250 ms */
#define CHUNK 800            /* 50 ms */

static int16_t pcm[CHUNK];
static unsigned char req_buf[512];
static char out[2048];
static unsigned out_len;

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req_buf ? (uint32_t)(uintptr_t)req_buf : 0;
}

static NOINLINE void put(const char* s) { while (*s && out_len < sizeof out) out[out_len++] = *s++; }
static NOINLINE void put_int(int64_t v) {
    if (v < 0) { put("-"); v = -v; }
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && out_len < sizeof out) out[out_len++] = d[--k];
}
static NOINLINE void reply(int32_t status) {
    k32_log(out, out_len < 200 ? out_len : 200);
    k32_resp_begin(status, "application/json", 16);
    k32_resp_write(out, out_len);
    k32_resp_end();
}

/* Integer square root of a 64-bit value */
static NOINLINE uint32_t isqrt(uint64_t v) {
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return (uint32_t)r;
}

struct stats { int64_t sum, sumsq; int32_t min, max; uint32_t n; float s1, s2; };

static float g_coeff;     /* 2 cos(2 pi f / RATE), 0 without hz */

static NOINLINE void stats_add(struct stats* s, int32_t v) {
    s->sum += v;
    s->sumsq += (int64_t)v * v;
    if (g_coeff != 0.0f) {
        const float s0 = (float)v + g_coeff * s->s1 - s->s2;
        s->s2 = s->s1;
        s->s1 = s0;
    }
    if (v < s->min) s->min = v;
    if (v > s->max) s->max = v;
    s->n++;
}

static NOINLINE void stats_put(const struct stats* s) {
    const int64_t mean = s->n ? s->sum / s->n : 0;
    const int64_t var = s->n ? s->sumsq / s->n - mean * mean : 0;
    const int64_t peak = s->max - mean > mean - s->min ? s->max - mean : mean - s->min;
    put("{\"mean\":"); put_int(mean);
    put(",\"rms\":"); put_int(var > 0 ? isqrt((uint64_t)var) : 0);
    put(",\"peak\":"); put_int(peak);
    if (g_coeff != 0.0f && s->n) {
        const float p = s->s1 * s->s1 + s->s2 * s->s2 - g_coeff * s->s1 * s->s2;
        put(",\"tone\":"); put_int((int64_t)(2u * isqrt(p > 0.0f ? (uint64_t)p : 0) / s->n));
    }
    put("}");
}

static NOINLINE void stats_reset(struct stats* s) { s->sum = s->sumsq = 0; s->min = 32767; s->max = -32768; s->n = 0; s->s1 = s->s2 = 0.0f; }

/* cos(x) for x in [0, pi]: a Taylor series, close enough for a filter coefficient */
static NOINLINE float cosine(float x) {
    const float x2 = x * x;
    float term = 1.0f, sum = 1.0f;
    for (int k = 1; k <= 6; k++) {
        term = -term * x2 / (float)((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}

static NOINLINE void record(uint32_t ms, uint32_t hz) {
    g_coeff = hz > 0 && hz < RATE / 2 ? 2.0f * cosine(6.2831853f * (float)hz / (float)RATE) : 0.0f;
    if (ms > 10000) ms = 10000;
    const uint32_t total = ms * (RATE / 1000);
    struct stats all, win;
    stats_reset(&all);
    stats_reset(&win);
    out_len = 0;
    put("{\"windows\":[");
    k32_mic_read(pcm, CHUNK, 200);   /* the decimator settles first: not counted */
    uint32_t got = 0, windows = 0;
    while (got < total) {
        const int32_t n = k32_mic_read(pcm, total - got < CHUNK ? total - got : CHUNK, 200);
        if (n <= 0) {
            out_len = 0;
            put("{\"error\":\"k32_mic_read\",\"code\":"); put_int(n); put("}");
            reply(500);
            return;
        }
        for (int32_t i = 0; i < n; i++) {
            stats_add(&all, pcm[i]);
            stats_add(&win, pcm[i]);
            if (win.n == WINDOW) {
                if (windows++) put(",");
                stats_put(&win);
                stats_reset(&win);
            }
        }
        got += (uint32_t)n;
    }
    k32_mic_read(pcm, 0, 0);   /* stop the microphone */
    put("],\"samples\":"); put_int(got); put(",\"all\":"); stats_put(&all); put("}");
    reply(200);
}

/* sin(2 pi t) for t in [0, 1) in Q15: a parabola per half period, close enough for a test tone */
static NOINLINE int32_t sine_q15(uint32_t phase) {
    const int32_t x = (int32_t)(phase >> 16);            /* 0..65535 for one period */
    const int32_t h = x < 32768 ? x : x - 32768;          /* position in the half period */
    const int32_t y = (int32_t)(((int64_t)4 * h * (32768 - h)) >> 15);   /* 0..32768 */
    return x < 32768 ? (y > 32767 ? 32767 : y) : -y;
}

static NOINLINE void play(uint32_t hz, uint32_t ms, uint32_t vol) {
    if (hz < 50 || hz > 8000) hz = 440;
    if (ms > 2000) ms = 2000;
    const uint32_t total = ms * (RATE / 1000);
    const uint32_t step = (uint32_t)(((uint64_t)hz << 32) / RATE);
    uint32_t phase = 0, done = 0;
    while (done < total) {
        const uint32_t n = total - done < CHUNK ? total - done : CHUNK;
        for (uint32_t i = 0; i < n; i++) {
            pcm[i] = (int16_t)sine_q15(phase);
            phase += step;
        }
        const int32_t rc = k32_audio_play(pcm, n, RATE, vol);
        if (rc < 0) {
            out_len = 0;
            put("{\"error\":\"k32_audio_play\",\"code\":"); put_int(rc); put("}");
            reply(500);
            return;
        }
        done += n;
    }
    out_len = 0;
    put("{\"played_ms\":"); put_int(ms); put(",\"hz\":"); put_int(hz); put(",\"vol\":"); put_int(vol); put("}");
    reply(200);
}

static NOINLINE uint32_t query_uint(const char* q, uint32_t len, const char* key, uint32_t dflt) {
    uint32_t klen = 0;
    while (key[klen]) klen++;
    for (uint32_t i = 0; i + klen < len; i++) {
        if ((i == 0 || q[i - 1] == '&') && q[i + klen] == '=') {
            uint32_t j = 0;
            while (j < klen && q[i + j] == key[j]) j++;
            if (j < klen) continue;
            uint32_t v = 0;
            for (uint32_t e = i + klen + 1; e < len && q[e] >= '0' && q[e] <= '9'; e++) v = v * 10u + (uint32_t)(q[e] - '0');
            return v;
        }
    }
    return dflt;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    const char* r = (const char*)(uintptr_t)ptr;
    uint32_t i = 0;
    while (i < len && r[i] != ' ') i++;
    const char* path = r + i + 1;
    uint32_t plen = 0;
    while (i + 1 + plen < len && path[plen] != ' ' && path[plen] != '\n') plen++;
    const char* q = path;
    uint32_t qlen = 0;
    for (uint32_t k = 0; k < plen; k++) {
        if (path[k] == '?') { q = path + k + 1; qlen = plen - k - 1; plen = k; break; }
    }
    if (plen == 5 && path[1] == 'p') play(query_uint(q, qlen, "hz", 440), query_uint(q, qlen, "ms", 300), query_uint(q, qlen, "vol", 10));
    else record(query_uint(q, qlen, "ms", 2000), query_uint(q, qlen, "hz", 0));
    return 200;
}
