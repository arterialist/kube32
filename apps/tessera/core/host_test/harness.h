/* Helpers shared by the test, bench and size tools: drive k32_handle natively. */
#ifndef TESSERA_HARNESS_H
#define TESSERA_HARNESS_H
#include "shim.h"
#include "../src/app.h"
#include "../src/k32_host.h"
#include <string.h>
#include <stdio.h>

/* Sends one request through k32_alloc/k32_handle. Returns the status. */
static inline int hx_request(const char* method, const char* path, const char* cookie, const char* body) {
    char req[16384]; int n = 0;
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
/* Drives k32_tick until the incremental seeding reports idle. Returns the
 * number of ticks it took. */
static inline int hx_seed_complete(void) {
    int n = 0;
    while (k32_tick(0) > 0) n++;
    return n + 1;
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
#endif
