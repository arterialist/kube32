/*
 * Native shim for the k32_* host imports. Responses are captured into a
 * buffer, events into a list; env comes from the real environment.
 */
#include "shim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

shim_state shim;

static int64_t mono_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int64_t t_start = -1;

void shim_reset_response(void) {
    shim.status = 0; shim.begin_calls = 0; shim.end_calls = 0; shim.header_n = 0;
    shim.body_len = 0; shim.content_type[0] = 0;
    if (shim.body) shim.body[0] = 0;
}
void shim_reset_events(void) { shim.event_n = 0; }

void k32_resp_begin(int32_t status, const char* content_type, uint32_t content_type_len) {
    shim.begin_calls++;
    shim.status = status;
    if (content_type_len >= sizeof shim.content_type) content_type_len = sizeof shim.content_type - 1;
    memcpy(shim.content_type, content_type, content_type_len);
    shim.content_type[content_type_len] = 0;
    shim.body_len = 0;
    if (!shim.body) { shim.body_cap = 1 << 20; shim.body = malloc(shim.body_cap); }
    shim.body[0] = 0;
}
void k32_resp_header(const char* name, uint32_t name_len, const char* value, uint32_t value_len) {
    if (shim.header_n >= SHIM_MAX_HEADERS) return;
    shim_header* h = &shim.headers[shim.header_n++];
    if (name_len >= sizeof h->name) name_len = sizeof h->name - 1;
    if (value_len >= sizeof h->value) value_len = sizeof h->value - 1;
    memcpy(h->name, name, name_len); h->name[name_len] = 0;
    memcpy(h->value, value, value_len); h->value[value_len] = 0;
}
int32_t k32_resp_write(const void* data, uint32_t len) {
    if (!shim.body) { shim.body_cap = 1 << 20; shim.body = malloc(shim.body_cap); }
    if (shim.body_len + len + 1 > shim.body_cap) {
        while (shim.body_len + len + 1 > shim.body_cap) shim.body_cap *= 2;
        shim.body = realloc(shim.body, shim.body_cap);
    }
    memcpy(shim.body + shim.body_len, data, len);
    shim.body_len += len;
    shim.body[shim.body_len] = 0;
    shim.bytes_written += len;
    return (int32_t)len;
}
void k32_resp_end(void) { shim.end_calls++; }

int32_t k32_event_emit(const char* topic, uint32_t topic_len, const void* data, uint32_t len) {
    if (len > 200) { fprintf(stderr, "EVENT TOO LARGE: %u bytes\n", len); shim.oversized_events++; }
    if (shim.event_n < SHIM_MAX_EVENTS) {
        shim_event* e = &shim.events[shim.event_n++];
        if (topic_len >= sizeof e->topic) topic_len = sizeof e->topic - 1;
        if (len >= sizeof e->data) len = sizeof e->data - 1;
        memcpy(e->topic, topic, topic_len); e->topic[topic_len] = 0;
        memcpy(e->data, data, len); e->data[len] = 0;
        e->len = len;
    }
    shim.events_total++;
    return (int32_t)len;
}
int64_t k32_time_ms(void) {
    if (t_start < 0) t_start = mono_ms();
    return mono_ms() - t_start + shim.time_offset_ms;
}
void k32_log(const char* msg, uint32_t len) {
    if (shim.quiet) return;
    fprintf(stderr, "[k32_log] %.*s\n", (int)len, msg);
}
uint32_t k32_random(void) { return (uint32_t)rand() ^ ((uint32_t)rand() << 16); }
int32_t k32_env_get(const char* key, uint32_t key_len, char* out, uint32_t out_cap) {
    char k[64];
    if (key_len >= sizeof k) return -1;
    memcpy(k, key, key_len); k[key_len] = 0;
    const char* v = getenv(k);
    if (!v) return -1;
    size_t n = strlen(v);
    if (n > out_cap) n = out_cap;
    memcpy(out, v, n);
    return (int32_t)strlen(v);
}
int32_t k32_mem_stats(uint32_t* out_used, uint32_t* out_limit) {
    (void)out_used; (void)out_limit;
    return -1;   /* unsupported natively: the core falls back to its arena numbers */
}
