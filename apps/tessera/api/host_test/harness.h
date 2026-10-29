/* Helpers shared by the test, bench, size and profile tools: drive the API's
 * exports natively against an in-process tessera-db on a temporary volume. */
#ifndef TESSERA_HARNESS_H
#define TESSERA_HARNESS_H
#define _POSIX_C_SOURCE 200809L
#include "../../common/host/shim.h"
#include "../../tools/dataset/dataset.h"
#include "../src/app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* tessera-db's exports, renamed for the native link (see the Makefile) */
int32_t  tdb_k32_init(void);
uint32_t tdb_k32_alloc(uint32_t size);
int32_t  tdb_k32_handle(uint32_t ptr, uint32_t len);
uint8_t* tdb_resolve(uint32_t ptr);
struct tdb_t;

static inline double hx_now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6; }

/* Sends one request through k32_alloc/k32_handle. Returns the status. */
static inline int hx_request(const char* method, const char* path, const char* cookie, const char* body) {
    static char req[16384]; int n = 0;
    n += snprintf(req + n, sizeof req - n, "%s %s\n", method, path);
    if (cookie) n += snprintf(req + n, sizeof req - n, "Cookie: %s\n", cookie);
    if (body) n += snprintf(req + n, sizeof req - n, "Content-Type: application/json\n");
    n += snprintf(req + n, sizeof req - n, "\n");
    if (body) { size_t bl = strlen(body); if (n + bl < sizeof req) { memcpy(req + n, body, bl); n += (int)bl; } }
    uint32_t p = k32_alloc((uint32_t)n);
    if (!p) return -99;
    memcpy(tessera_resolve(p), req, (size_t)n);
    shim_reset_response();
    return k32_handle(p, (uint32_t)n);
}
/* Sends an event through k32_event. */
static inline int hx_event(const char* topic, const char* payload) {
    uint32_t tl = (uint32_t)strlen(topic), pl = (uint32_t)strlen(payload);
    uint32_t tp = k32_alloc(tl + pl + 2);
    if (!tp) return -99;
    uint8_t* base = tessera_resolve(tp);
    memcpy(base, topic, tl); memcpy(base + tl, payload, pl);
    return k32_event(tp, tl, tp + tl, pl);
}

/* ---- the in-process tessera-db ------------------------------------------------ */
static inline int32_t hx_db_handler(const char* req, uint32_t len) {
    uint32_t p = tdb_k32_alloc(len);
    if (!p) return 413;
    memcpy(tdb_resolve(p), req, len);
    return tdb_k32_handle(p, len);
}
/* A fresh volume in a temporary directory; with issues > 0 it holds the
 * generated dataset. Starts tessera-db on it and registers the service.
 * Returns the dataset build time in ms, or a negative value on failure. */
static inline double hx_db_setup(uint32_t issues, uint32_t comments, dataset_stats* st) {
    snprintf(shim_fs.root, sizeof shim_fs.root, "%s/tessera-api-test-%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid());
    if (shim_fs_reset_dir(shim_fs.root) != 0) return -1;
    double t0 = hx_now_ms();
    dataset_stats tmp; if (!st) st = &tmp;
    memset(st, 0, sizeof *st);
    if (issues && dataset_build(issues, comments, 1793059200000ll, st) != 0) return -2;
    double dt = hx_now_ms() - t0;
    if (tdb_k32_init() != 0) return -3;
    if (!shim_service_add("tessera-db", hx_db_handler)) return -4;
    return dt;
}
static inline void hx_db_cleanup(void) { shim_fs_reset_dir(shim_fs.root); rmdir(shim_fs.root); }
/* Restarts tessera-db on the same volume (a pod restart). */
static inline int hx_db_restart(void) { return tdb_k32_init(); }

/* Ticks until the API reports idle (the load finished or is waiting).
 * Returns the number of k32_tick calls. */
static inline int hx_load_complete(void) {
    int n = 0;
    while (k32_tick(0) > 0 && n < 1000000) n++;
    return n + 1;
}
#endif
