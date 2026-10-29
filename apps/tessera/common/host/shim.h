/*
 * Native stand-ins for the k32_* host imports, shared by the host tests of
 * every Tessera pod: responses and events are captured, the pod volume is a
 * host directory, service calls go to in-process handlers and assets are
 * looked up in a host directory.
 */
#ifndef TESSERA_SHIM_H
#define TESSERA_SHIM_H
#include <stdint.h>

#define SHIM_MAX_HEADERS 8
#define SHIM_MAX_EVENTS 256
#define SHIM_MAX_SERVICES 4

typedef struct { char name[64]; char value[256]; } shim_header;
typedef struct { char topic[32]; char data[256]; uint32_t len; } shim_event;

/* A service reachable with k32_http_call("http://<name>/..."). handler gets
 * the request block in the host encoding and produces a response through
 * the k32_resp_* imports, like a pod's k32_handle. */
typedef int32_t (*shim_handler)(const char* req, uint32_t len);
typedef struct {
    char name[32];
    shim_handler handler;
    int32_t down;               /* nonzero: calls return this K32_ERR_* */
    int32_t fail_calls;         /* the next n calls return K32_ERR_TIMEOUT */
    int32_t lose_replies;       /* the next n calls reach the handler, then return K32_ERR_TIMEOUT */
    uint32_t calls;
    char last_method[8], last_path[256];
    uint32_t last_body_len;
} shim_service;

typedef struct {
    int status; int begin_calls, end_calls;
    char content_type[64];
    shim_header headers[SHIM_MAX_HEADERS]; int header_n;
    char* body; uint32_t body_len, body_cap; uint64_t bytes_written;
    shim_event events[SHIM_MAX_EVENTS]; int event_n; uint64_t events_total; int oversized_events;
    int64_t time_offset_ms;
    int quiet;
    /* k32_resp_asset */
    char asset_root[512];
    char asset_path[256]; uint32_t asset_calls;
    /* k32_http_call */
    shim_service services[SHIM_MAX_SERVICES]; int service_n;
    uint64_t call_us;           /* time spent in service handlers */
} shim_state;

typedef struct {
    char root[512];             /* the volume directory */
    int32_t fail_write_after;   /* >= 0: the next write stores this many bytes, then fails */
    int32_t nodev;              /* nonzero: every call returns K32_ERR_NODEV */
    uint32_t opens, reads, writes, open_now;
    uint64_t read_bytes, write_bytes;
} shim_fs_state;

extern shim_state shim;
extern shim_fs_state shim_fs;
void shim_reset_response(void);
void shim_reset_events(void);
shim_service* shim_service_add(const char* name, shim_handler handler);
shim_service* shim_service_get(const char* name);
/* Makes an empty directory (removing a previous one with its files). */
int  shim_fs_reset_dir(const char* path);
#endif
