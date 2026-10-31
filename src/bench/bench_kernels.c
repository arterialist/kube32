#include "bench/bench_kernels.h"

#define NOINLINE __attribute__((noinline))

/* fib: recursive Fibonacci, mostly calls and returns */
static NOINLINE uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

/* crc32: bitwise CRC-32 over a 1 KiB buffer, n times; byte loads and shifts */
static NOINLINE uint32_t crc32(uint32_t n, uint8_t* s_bytes) {
    uint32_t seed = 12345u;
    for (uint32_t i = 0; i < 1024; i++) {
        seed = seed * 1103515245u + 12345u;
        s_bytes[i] = (uint8_t)(seed >> 16);
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t r = 0; r < n; r++) {
        for (uint32_t i = 0; i < 1024; i++) {
            crc ^= s_bytes[i];
            for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* sieve: primes below n (at most 4096); byte stores in a loop */
static NOINLINE uint32_t sieve(uint32_t n, uint8_t* s_composite) {
    if (n > BENCH_WORK_BYTES) n = BENCH_WORK_BYTES;
    for (uint32_t i = 0; i < n; i++) s_composite[i] = 0;
    uint32_t count = 0;
    for (uint32_t i = 2; i < n; i++) {
        if (s_composite[i]) continue;
        count++;
        for (uint32_t j = i * i; j < n; j += i) s_composite[j] = 1;
    }
    return count;
}

/* sort: heapsort of n (at most 1024) pseudo-random words; compares, swaps */
static NOINLINE void sift(uint32_t* a, uint32_t root, uint32_t end) {
    for (;;) {
        uint32_t child = 2 * root + 1;
        if (child >= end) return;
        if (child + 1 < end && a[child] < a[child + 1]) child++;
        if (a[root] >= a[child]) return;
        uint32_t t = a[root]; a[root] = a[child]; a[child] = t;
        root = child;
    }
}
static NOINLINE uint32_t sort(uint32_t n, uint32_t* s_words) {
    if (n > 1024) n = 1024;
    uint32_t seed = 42u;
    for (uint32_t i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; s_words[i] = seed; }
    for (uint32_t i = n / 2; i-- > 0;) sift(s_words, i, n);
    for (uint32_t end = n; end > 1; end--) {
        uint32_t t = s_words[0]; s_words[0] = s_words[end - 1]; s_words[end - 1] = t;
        sift(s_words, 0, end - 1);
    }
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) sum = sum * 31u + s_words[i];
    return sum;
}

/* mandel: an n x n Mandelbrot grid, 64 iterations at most; float arithmetic */
static NOINLINE uint32_t mandel(uint32_t n) {
    if (n > 256) n = 256;
    uint32_t total = 0;
    for (uint32_t y = 0; y < n; y++) {
        for (uint32_t x = 0; x < n; x++) {
            float cr = -2.0f + 2.5f * (float)x / (float)n, ci = -1.25f + 2.5f * (float)y / (float)n;
            float zr = 0.0f, zi = 0.0f;
            uint32_t it = 0;
            while (it < 64 && zr * zr + zi * zi <= 4.0f) {
                float t = zr * zr - zi * zi + cr;
                zi = 2.0f * zr * zi + ci;
                zr = t;
                it++;
            }
            total += it;
        }
    }
    return total;
}

/* json: n small JSON records written to a buffer, the text work a pod does
 * when it answers a request */
#define TEXT_BYTES 160u
static NOINLINE uint32_t put_str(char* s_text, uint32_t at, const char* s) { while (*s && at < TEXT_BYTES) s_text[at++] = *s++; return at; }
static NOINLINE uint32_t put_uint(char* s_text, uint32_t at, uint32_t v) {
    char d[10]; int k = 0;
    do { d[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k && at < TEXT_BYTES) s_text[at++] = d[--k];
    return at;
}
static NOINLINE uint32_t json(uint32_t n, char* s_text) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = put_str(s_text, 0, "{\"id\":");
        at = put_uint(s_text, at, i);
        at = put_str(s_text, at, ",\"name\":\"sensor-");
        at = put_uint(s_text, at, i * 7u);
        at = put_str(s_text, at, "\",\"value\":");
        at = put_uint(s_text, at, i * 2654435761u);
        at = put_str(s_text, at, ",\"ok\":true}");
        for (uint32_t j = 0; j < at; j++) sum = sum * 33u + (uint8_t)s_text[j];
    }
    return sum;
}

static const char* const kNames[] = {"fib", "crc32", "sieve", "sort", "mandel", "json"};

int bench_kernel_index(const char* name, uint32_t len) {
    for (int k = 0; k < (int)(sizeof kNames / sizeof kNames[0]); k++) {
        const char* s = kNames[k];
        uint32_t i = 0;
        while (i < len && s[i] && s[i] == name[i]) i++;
        if (i == len && s[i] == 0) return k;
    }
    return -1;
}

const char* bench_kernel_name(int index) {
    return index >= 0 && index < (int)(sizeof kNames / sizeof kNames[0]) ? kNames[index] : "";
}

uint32_t bench_kernel_run(int index, uint32_t n, void* work) {
    switch (index) {
        case 0: return fib(n > 30 ? 30 : n);
        case 1: return crc32(n, (uint8_t*)work);
        case 2: return sieve(n, (uint8_t*)work);
        case 3: return sort(n, (uint32_t*)work);
        case 4: return mandel(n);
        case 5: return json(n, (char*)work);
        default: return 0;
    }
}
