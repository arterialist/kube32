#ifndef TESSERA_SHIM_H
#define TESSERA_SHIM_H
#include <stdint.h>

#define SHIM_MAX_HEADERS 8
#define SHIM_MAX_EVENTS 256

typedef struct { char name[64]; char value[256]; } shim_header;
typedef struct { char topic[32]; char data[256]; uint32_t len; } shim_event;

typedef struct {
    int status; int begin_calls, end_calls;
    char content_type[64];
    shim_header headers[SHIM_MAX_HEADERS]; int header_n;
    char* body; uint32_t body_len, body_cap; uint64_t bytes_written;
    shim_event events[SHIM_MAX_EVENTS]; int event_n; uint64_t events_total; int oversized_events;
    int64_t time_offset_ms;
    int quiet;
} shim_state;

extern shim_state shim;
void shim_reset_response(void);
void shim_reset_events(void);
#endif
