/*
 * Work counters for representative requests (native build, see k32_prof in
 * k32lib.h): comparator calls, dictionary string compares, stopword
 * compares and posting binary-search steps. Seed sizes come from the
 * environment like everywhere else (SEED_ISSUES / SEED_COMMENTS / SEED_CHUNK).
 */
#include "harness.h"
#include <stdlib.h>
#include <time.h>

static int64_t now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000ll + ts.tv_nsec / 1000; }
static char cookie[64];

static void one(const char* label, const char* method, const char* path, const char* body) {
    k32_prof_t before = k32_prof;
    int64_t t0 = now_us();
    int st = hx_request(method, path, cookie, body);
    int64_t dt = now_us() - t0;
    printf("  %-34s %3d %6u B %6u cmp %5u term %5u stop %5u post | %6u putc %5u bulk %6u bulkB %6u scanB %4u key %4u int %3u flush %6lld us\n", label, st, shim.body_len,
           k32_prof.cmp_calls - before.cmp_calls, k32_prof.term_cmps - before.term_cmps,
           k32_prof.stop_cmps - before.stop_cmps, k32_prof.posting_steps - before.posting_steps,
           k32_prof.jw_putc - before.jw_putc, k32_prof.jw_writes - before.jw_writes, k32_prof.jw_write_bytes - before.jw_write_bytes,
           k32_prof.jw_scan_bytes - before.jw_scan_bytes, k32_prof.jw_keys - before.jw_keys, k32_prof.jw_ints - before.jw_ints,
           k32_prof.resp_writes - before.resp_writes, (long long)dt);
}

int main(void) {
    shim.quiet = 1;
    srand(3);
    int64_t t0 = now_us();
    if (k32_init() != 0) { fprintf(stderr, "init failed\n"); return 1; }
    int64_t t_init = now_us() - t0;
    k32_prof_t s0 = k32_prof;
    t0 = now_us();
    int ticks = 0, max_tick_us = 0, last_tick_us = 0;
    for (;;) {
        int64_t a = now_us();
        int rc = k32_tick(0);
        int us = (int)(now_us() - a);
        ticks++;
        if (us > max_tick_us) max_tick_us = us;
        last_tick_us = us;
        if (rc <= 0) break;
    }
    int64_t t_seed = now_us() - t0;
    tessera_layout L; tessera_layout_get(&L);
    printf("tessera-core work profile (SEED_ISSUES=%s SEED_COMMENTS=%s)\n", getenv("SEED_ISSUES") ? getenv("SEED_ISSUES") : "3000", getenv("SEED_COMMENTS") ? getenv("SEED_COMMENTS") : "6000");
    printf("  k32_init %lld us; seeding %d ticks in %lld us (max tick %d us, final tick %d us), %u compactions\n",
           (long long)t_init, ticks, (long long)t_seed, max_tick_us, last_tick_us, k32_prof.compactions - s0.compactions);
    printf("  seeding work: %u term cmps, %u stop cmps, %u posting steps; arena used %u, postings %u B, leaked %u\n",
           k32_prof.term_cmps - s0.term_cmps, k32_prof.stop_cmps - s0.stop_cmps, k32_prof.posting_steps - s0.posting_steps, L.arena_used, L.postings, L.postings_leaked);
    hx_request("POST", "/api/auth/login", 0, "{\"username\":\"mara\",\"password\":\"demo\"}");
    snprintf(cookie, sizeof cookie, "tsid=%.16s", shim.headers[0].value + 5);
    /* real keys of two old issues for the mutation requests */
    char k1[24] = "TES-1", k3[24] = "TES-3";
    hx_request("GET", "/api/issues?sort=key&order=asc&limit=3", cookie, 0);
    { const char* p = strstr(shim.body, "\"key\":\""); if (p) sscanf(p + 7, "%15[^\"]", k1); p = p ? strstr(p + 7, "\"key\":\"") : 0; p = p ? strstr(p + 7, "\"key\":\"") : 0; if (p) sscanf(p + 7, "%15[^\"]", k3); }
    char p_comment[64], p_title[64], p_status[64];
    snprintf(p_comment, sizeof p_comment, "/api/issues/%s/comments", k1);
    snprintf(p_title, sizeof p_title, "/api/issues/%s", k1);
    snprintf(p_status, sizeof p_status, "/api/issues/%s", k3);
    printf("  %-34s %3s %8s %10s %10s %10s %10s | per-byte writer work: putc = single-byte writes, bulk = memcpy runs, scanB = bytes through the escape scanner\n", "request", "st", "bytes", "cmp", "term", "stop", "post");
    one("GET /api/issues?limit=1", "GET", "/api/issues?limit=1", 0);
    one("GET /api/issues?limit=50", "GET", "/api/issues?limit=50", 0);
    one("GET /api/issues?limit=50&page=10", "GET", "/api/issues?limit=50&page=10", 0);
    one("GET /api/issues?sort=key&order=desc", "GET", "/api/issues?sort=key&order=desc&limit=25", 0);
    one("GET /api/issues?status=done&sort=priority", "GET", "/api/issues?status=done&sort=priority&order=desc", 0);
    one("GET /api/issues?q=wifi&limit=25", "GET", "/api/issues?q=wifi&limit=25", 0);
    one("GET /api/board?project=1", "GET", "/api/board?project=1", 0);
    one("GET /api/board (all)", "GET", "/api/board", 0);
    one("GET /api/issues/<k1> (full)", "GET", p_title, 0);
    one("GET /api/search?q=wifi", "GET", "/api/search?q=wifi", 0);
    one("GET /api/search?q=wi (prefix)", "GET", "/api/search?q=wi", 0);
    one("GET /api/search?q=lease+timer", "GET", "/api/search?q=lease+timer", 0);
    one("POST /api/issues (create)", "POST", "/api/issues", "{\"title\":\"Bench issue about the wifi driver timeout\",\"description\":\"Observed on esp32-node-02: the scheduler stalls under load. Logs show retries piling up.\",\"project\":1,\"priority\":2,\"labels\":[0,4]}");
    one("POST .../comments (old issue)", "POST", p_comment, "{\"body\":\"Reproduced on the ci rig, a crc mismatch after reconnect.\"}");
    one("PATCH ... (title, old issue)", "PATCH", p_title, "{\"title\":\"Renamed wifi driver issue under load\"}");
    one("PATCH ... (status only)", "PATCH", p_status, "{\"status\":\"done\"}");
    one("GET /api/analytics", "GET", "/api/analytics", 0);
    one("GET /api/stats", "GET", "/api/stats", 0);
    return 0;
}
