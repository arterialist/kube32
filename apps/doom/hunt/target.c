// SPDX-License-Identifier: GPL-2.0-or-later
/* doom-target: a pod the hunt can shoot. It does one thing: it tells the
 * sim it exists, on the channel topic doom.pods, every second tick ("<pod>
 * <node>"). The sim gives every pod that speaks up a demon and drops the demon
 * of a pod that falls silent (port/hunt.c).
 *
 *   env POD_NAME, NODE_NAME   from the downward API
 *   GET /                     the announcement */
#include <stdint.h>
#include "runtime/k32_app_abi.h"

static char line[96];
static uint32_t line_len;
static uint8_t req[128];

static void field(const char* key, uint32_t key_len) {
    const int32_t n = k32_env_get(key, key_len, line + line_len, sizeof line - line_len - 1);
    if (n > 0) line_len += (uint32_t)n;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    field("POD_NAME", 8);
    line[line_len++] = ' ';
    field("NODE_NAME", 9);
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    static uint32_t ticks;
    if (!(ticks++ & 1)) k32_event_emit("doom.pods", 9, line, line_len);
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr; (void)len;
    k32_resp_begin(200, "text/plain", 10);
    k32_resp_write(line, line_len);
    k32_resp_end();
    return 200;
}
