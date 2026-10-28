/*
 * Native benchmark: 10,000 mixed requests against the default seed set,
 * timed per request. Prints p50 / p99 / max per category and overall.
 */
#include "harness.h"
#include <stdlib.h>
#include <time.h>

static int64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec; }
static int cmp_i64(const void* a, const void* b) { int64_t x = *(const int64_t*)a, y = *(const int64_t*)b; return x < y ? -1 : x > y; }

enum { C_LIST, C_SEARCH, C_GET, C_BOARD, C_ANALYTICS, C_ACTIVITY, C_WRITE, C_STATS, C_N };
static const char* names[C_N] = { "list", "search", "get", "board", "analytics", "activity", "write", "stats" };
static int64_t samples[C_N][10000]; static int counts[C_N];
static int64_t all[10000];

static const char* words[] = { "wifi", "driver", "crash", "psram", "lease", "journal", "keyboard", "frame", "timeout", "heap",
                               "espnow", "scheduler", "dma", "brownout", "crc", "display", "search", "session", "cookie", "ota" };

int main(void) {
    shim.quiet = 1;
    srand(7);
    if (k32_init() != 0) { fprintf(stderr, "init failed\n"); return 1; }
    hx_seed_complete();
    hx_request("POST", "/api/auth/login", 0, "{\"username\":\"mara\",\"password\":\"demo\"}");
    char cookie[64]; snprintf(cookie, sizeof cookie, "tsid=%.16s", shim.headers[0].value + 5);
    /* collect some keys */
    char keys[64][16]; int nkeys = 0;
    hx_request("GET", "/api/issues?limit=50&sort=key&order=asc", cookie, 0);
    for (const char* p = shim.body; (p = strstr(p, "\"key\":\"")) && nkeys < 64; p += 7) {
        sscanf(p + 7, "%15[^\"]", keys[nkeys++]);
    }
    const int N = 10000;
    int64_t t_total = now_ns();
    for (int i = 0; i < N; i++) {
        int r = rand() % 100; int cat; char path[256]; int st;
        int64_t t0 = now_ns();
        if (r < 30) {
            cat = C_LIST;
            static const char* sorts[] = { "updated", "created", "priority", "key" };
            int k = rand() % 6;
            if (k == 0) snprintf(path, sizeof path, "/api/issues?page=%d&limit=50", 1 + rand() % 20);
            else if (k == 1) snprintf(path, sizeof path, "/api/issues?status=todo,in_progress&sort=%s", sorts[rand() % 4]);
            else if (k == 2) snprintf(path, sizeof path, "/api/issues?project=%d&assignee=%d", 1 + rand() % 3, 1 + rand() % 8);
            else if (k == 3) snprintf(path, sizeof path, "/api/issues?label=%d&priority=%d&sort=priority", rand() % 12, rand() % 5);
            else if (k == 4) snprintf(path, sizeof path, "/api/issues?q=%s&limit=25", words[rand() % 20]);
            else snprintf(path, sizeof path, "/api/issues?sort=%s&order=asc&page=%d", sorts[rand() % 4], 1 + rand() % 5);
            st = hx_request("GET", path, cookie, 0);
        } else if (r < 50) {
            cat = C_SEARCH;
            if (rand() % 2) snprintf(path, sizeof path, "/api/search?q=%s+%s", words[rand() % 20], words[rand() % 20]);
            else snprintf(path, sizeof path, "/api/search?q=%.3s", words[rand() % 20]);
            st = hx_request("GET", path, cookie, 0);
        } else if (r < 70) {
            cat = C_GET;
            snprintf(path, sizeof path, "/api/issues/%s", keys[rand() % nkeys]);
            st = hx_request("GET", path, cookie, 0);
        } else if (r < 80) {
            cat = C_BOARD;
            snprintf(path, sizeof path, "/api/board?project=%d", 1 + rand() % 3);
            st = hx_request("GET", path, cookie, 0);
        } else if (r < 85) {
            cat = C_ANALYTICS;
            snprintf(path, sizeof path, "/api/analytics?days=%d", 7 + rand() % 24);
            st = hx_request("GET", path, cookie, 0);
        } else if (r < 90) {
            cat = C_ACTIVITY;
            st = hx_request("GET", "/api/activity?limit=50", cookie, 0);
        } else if (r < 95) {
            cat = C_WRITE;
            int k = rand() % 3;
            if (k == 0) st = hx_request("POST", "/api/issues", cookie, "{\"title\":\"Bench issue about the wifi driver timeout\",\"description\":\"created by the benchmark under load\",\"project\":1,\"priority\":2,\"labels\":[0,4]}");
            else if (k == 1) { snprintf(path, sizeof path, "/api/issues/%s", keys[rand() % nkeys]); st = hx_request("PATCH", path, cookie, (rand() % 2) ? "{\"status\":\"done\"}" : "{\"status\":\"in_progress\",\"priority\":3}"); }
            else { snprintf(path, sizeof path, "/api/issues/%s/comments", keys[rand() % nkeys]); st = hx_request("POST", path, cookie, "{\"body\":\"bench comment about the psram allocator\"}"); }
        } else {
            cat = C_STATS;
            st = hx_request("GET", (rand() % 2) ? "/api/stats" : "/api/bootstrap", 0, 0);
        }
        int64_t dt = now_ns() - t0;
        if (st < 200 || st >= 300) { fprintf(stderr, "unexpected status %d for %s\n", st, path); }
        samples[cat][counts[cat]++] = dt;
        all[i] = dt;
    }
    int64_t total = now_ns() - t_total;
    qsort(all, N, sizeof(int64_t), cmp_i64);
    printf("tessera-core native bench: %d mixed requests in %.1f ms (%.0f req/s)\n", N, total / 1e6, N / (total / 1e9));
    printf("  %-10s %6s %9s %9s %9s %9s\n", "category", "n", "p50_us", "p99_us", "max_us", "avg_us");
    for (int c = 0; c < C_N; c++) {
        if (!counts[c]) continue;
        qsort(samples[c], counts[c], sizeof(int64_t), cmp_i64);
        int64_t sum = 0; for (int i = 0; i < counts[c]; i++) sum += samples[c][i];
        printf("  %-10s %6d %9.1f %9.1f %9.1f %9.1f\n", names[c], counts[c],
               samples[c][(counts[c] - 1) * 50 / 100] / 1e3, samples[c][(counts[c] - 1) * 99 / 100] / 1e3,
               samples[c][counts[c] - 1] / 1e3, (double)sum / counts[c] / 1e3);
    }
    int64_t sum = 0; for (int i = 0; i < N; i++) sum += all[i];
    printf("  %-10s %6d %9.1f %9.1f %9.1f %9.1f\n", "all", N, all[(N - 1) * 50 / 100] / 1e3, all[(N - 1) * 99 / 100] / 1e3, all[N - 1] / 1e3, (double)sum / N / 1e3);
    printf("  response bytes written: %llu\n", (unsigned long long)shim.bytes_written);
    hx_request("GET", "/api/stats", 0, 0);
    printf("  stats: %s\n", shim.body);
    return 0;
}
