// SPDX-License-Identifier: GPL-2.0-or-later
/* doom-reaper: the hunt's one client of the API server. Each event on
 * doom.kill names a pod; this pod deletes it by name and answers on
 * doom.reaped ("<pod> <http status> <ms>"). A delete that fails is tried again
 * from k32_tick; the pod being gone (404) counts as done. It runs on its own
 * pod worker, so the sim's tics do not wait for the API server. It writes no
 * log: deletes come when the control plane is short of memory (port/hunt.c).
 *
 *   annotation kube32.io/event-topics: doom.kill
 *   env API_SERVER      the API server from this node (default http://127.0.0.1:6443)
 *   env POD_NAMESPACE   the namespace of the pods (default default)
 *   GET /               counters as JSON */
#include <stdint.h>
#include "runtime/k32_app_abi.h"

#define QUEUE 8
#define NAME_BYTES 40
#define TRIES 10

static struct { char name[NAME_BYTES]; uint8_t len, tries; } queue[QUEUE];
static unsigned head, count;
static char api[64], ns[32], url[192];
static uint32_t api_len, ns_len;
static uint32_t deleted, errors, failed, retried, dropped;
static uint8_t resp[64], req[256];

/* the tiny libc clang expects for a freestanding module */
void* memcpy(void* d, const void* s, unsigned long n) { uint8_t* a = d; const uint8_t* b = s; while (n--) *a++ = *b++; return d; }

static unsigned put_str(char* p, const char* s) { unsigned n = 0; while (s[n]) { p[n] = s[n]; n++; } return n; }
static unsigned put_num(char* p, int32_t v) {
    char b[12];
    unsigned n = 0, o = 0;
    if (v < 0) { p[o++] = '-'; v = -v; }
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) p[o++] = b[--n];
    return o;
}

static uint32_t env(const char* key, uint32_t key_len, char* out, uint32_t cap, const char* def) {
    int32_t n = k32_env_get(key, key_len, out, cap - 1);
    if (n <= 0 || n >= (int32_t)cap) n = (int32_t)put_str(out, def);
    return (uint32_t)n;
}

K32_EXPORT("k32_init") int32_t k32_init(void) {
    api_len = env("API_SERVER", 10, api, sizeof api, "http://127.0.0.1:6443");
    ns_len = env("POD_NAMESPACE", 13, ns, sizeof ns, "default");
    return 0;
}

/* One delete; 1 when the pod needs no other try */
static int reap(const char* name, unsigned len) {
    unsigned n = 0;
    memcpy(url, api, api_len);
    n = api_len;
    n += put_str(url + n, "/api/v1/namespaces/");
    memcpy(url + n, ns, ns_len);
    n += ns_len;
    n += put_str(url + n, "/pods/");
    memcpy(url + n, name, len);
    n += len;
    n += put_str(url + n, "?gracePeriodSeconds=1");
    const int64_t t0 = k32_time_ms();
    uint32_t got = 0;
    /* the answer is the whole Pod: only its status matters */
    int32_t rc = k32_http_call("DELETE", 6, url, n, 0, 0, 0, 0, resp, sizeof resp, &got, 5000);
    if (rc == K32_ERR_TOOBIG) rc = k32_http_status();
    const int32_t ms = (int32_t)(k32_time_ms() - t0);
    const int done = rc == 200 || rc == 202 || rc == 404;
    if (rc < 200 || rc >= 300) errors++;
    char msg[NAME_BYTES + 24];
    unsigned m = 0;
    memcpy(msg, name, len);
    m = len;
    msg[m++] = ' ';
    m += put_num(msg + m, rc);
    msg[m++] = ' ';
    m += put_num(msg + m, ms);
    if (done) {
        deleted++;
        k32_event_emit("doom.reaped", 11, msg, m);
    }
    return done;
}

static void drain(void) {
    while (count) {
        char* name = queue[head].name;
        if (!reap(name, queue[head].len) && ++queue[head].tries < TRIES) {
            retried++;
            return;                       /* the next tick tries it again */
        }
        if (queue[head].tries >= TRIES) failed++;
        head = (head + 1) % QUEUE;
        count--;
    }
}

K32_EXPORT("k32_event") int32_t k32_event(uint32_t topic, uint32_t topic_len, uint32_t ptr, uint32_t len) {
    (void)topic; (void)topic_len;
    const char* name = (const char*)(uintptr_t)ptr;
    if (!len || len >= NAME_BYTES) return 0;
    for (unsigned i = 0; i < count; i++) {     /* the sim resends until it hears back */
        const unsigned at = (head + i) % QUEUE;
        if (queue[at].len != len) continue;
        unsigned j = 0;
        while (j < len && queue[at].name[j] == name[j]) j++;
        if (j == len) return 0;
    }
    if (count == QUEUE) { dropped++; return 0; }
    const unsigned at = (head + count) % QUEUE;
    memcpy(queue[at].name, name, len);
    queue[at].len = (uint8_t)len;
    queue[at].tries = 0;
    count++;
    if (count == 1) drain();
    return 0;
}

K32_EXPORT("k32_tick") int32_t k32_tick(uint32_t uptime_ms) {
    (void)uptime_ms;
    drain();
    return 0;
}

K32_EXPORT("k32_alloc") uint32_t k32_alloc(uint32_t size) {
    return size <= sizeof req ? (uint32_t)(uintptr_t)req : 0;
}

K32_EXPORT("k32_handle") int32_t k32_handle(uint32_t ptr, uint32_t len) {
    (void)ptr; (void)len;
    char out[112];
    unsigned n = put_str(out, "{\"deleted\":");
    n += put_num(out + n, (int32_t)deleted);
    n += put_str(out + n, ",\"errors\":");
    n += put_num(out + n, (int32_t)errors);
    n += put_str(out + n, ",\"retried\":");
    n += put_num(out + n, (int32_t)retried);
    n += put_str(out + n, ",\"failed\":");
    n += put_num(out + n, (int32_t)failed);
    n += put_str(out + n, ",\"dropped\":");
    n += put_num(out + n, (int32_t)dropped);
    out[n++] = '}';
    k32_resp_begin(200, "application/json", 16);
    k32_resp_write(out, n);
    k32_resp_end();
    return 200;
}
