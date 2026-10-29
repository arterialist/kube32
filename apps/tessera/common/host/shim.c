/*
 * Native shim for the k32_* host imports (see shim.h). Error codes and
 * limits follow the firmware: 4 descriptors, 4096 bytes per read or write,
 * relative paths of [A-Za-z0-9._-] components.
 */
#define _POSIX_C_SOURCE 200809L
#include "shim.h"
#include "../k32_host.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

shim_state shim;
shim_fs_state shim_fs = { "", -1, 0, 0, 0, 0, 0, 0, 0 };

static int64_t mono_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static int64_t t_start = -1;

void shim_reset_response(void) {
    shim.status = 0; shim.begin_calls = 0; shim.end_calls = 0; shim.header_n = 0;
    shim.body_len = 0; shim.content_type[0] = 0; shim.asset_path[0] = 0;
    if (shim.body) shim.body[0] = 0;
}
void shim_reset_events(void) { shim.event_n = 0; }

static void body_reserve(uint32_t extra) {
    if (!shim.body) { shim.body_cap = 1 << 20; shim.body = malloc(shim.body_cap); shim.body[0] = 0; }
    while (shim.body_len + extra + 1 > shim.body_cap) { shim.body_cap *= 2; shim.body = realloc(shim.body, shim.body_cap); }
}

/* ---- responses ------------------------------------------------------------- */
void k32_resp_begin(int32_t status, const char* content_type, uint32_t content_type_len) {
    shim.begin_calls++;
    shim.status = status;
    if (content_type_len >= sizeof shim.content_type) content_type_len = sizeof shim.content_type - 1;
    memcpy(shim.content_type, content_type, content_type_len);
    shim.content_type[content_type_len] = 0;
    shim.body_len = 0;
    body_reserve(0);
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
    body_reserve(len);
    memcpy(shim.body + shim.body_len, data, len);
    shim.body_len += len;
    shim.body[shim.body_len] = 0;
    shim.bytes_written += len;
    return (int32_t)len;
}
void k32_resp_end(void) { shim.end_calls++; }

static const char* ctype_for(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".js")) return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    return "application/octet-stream";
}
int32_t k32_resp_asset(const char* path, uint32_t path_len, uint32_t status) {
    char rel[256], full[1024];
    struct stat st;
    if (path_len == 0 || path_len >= sizeof rel) return K32_ERR_NOENT;
    memcpy(rel, path, path_len); rel[path_len] = 0;
    if (rel[0] == '/' || strstr(rel, "..")) return K32_ERR_NOENT;
    snprintf(full, sizeof full, "%s/%s", shim.asset_root, rel);
    int found = stat(full, &st) == 0 && S_ISREG(st.st_mode);
    if (!found) { snprintf(full, sizeof full, "%s/%s.gz", shim.asset_root, rel); found = stat(full, &st) == 0 && S_ISREG(st.st_mode); }
    if (!found) return K32_ERR_NOENT;
    shim.asset_calls++;
    snprintf(shim.asset_path, sizeof shim.asset_path, "%s", rel);
    const char* ct = ctype_for(rel);
    k32_resp_begin(status ? (int32_t)status : 200, ct, (uint32_t)strlen(ct));
    k32_resp_end();
    return 0;
}

/* ---- events, time, log, env -------------------------------------------------- */
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
    if (t_start < 0) t_start = mono_us();
    return (mono_us() - t_start) / 1000 + shim.time_offset_ms;
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
    return -1;   /* not available natively: callers fall back to their own numbers */
}

/* ---- service calls --------------------------------------------------------------- */
shim_service* shim_service_add(const char* name, shim_handler handler) {
    shim_service* s = shim_service_get(name);
    if (!s) {
        if (shim.service_n >= SHIM_MAX_SERVICES) return 0;
        s = &shim.services[shim.service_n++];
    }
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->handler = handler;
    return s;
}
shim_service* shim_service_get(const char* name) {
    for (int i = 0; i < shim.service_n; i++) if (!strcmp(shim.services[i].name, name)) return &shim.services[i];
    return 0;
}

int32_t k32_http_call(const char* method, uint32_t method_len, const char* url, uint32_t url_len,
                      const char* headers, uint32_t headers_len, const uint8_t* body, uint32_t body_len,
                      uint8_t* resp, uint32_t resp_cap, uint32_t* resp_len, uint32_t timeout_ms) {
    (void)timeout_ms;
    *resp_len = 0;
    char u[512];
    if (url_len >= sizeof u || method_len == 0 || method_len >= 8) return K32_ERR_INVAL;
    memcpy(u, url, url_len); u[url_len] = 0;
    if (strncmp(u, "http://", 7) != 0) return K32_ERR_INVAL;
    char* host = u + 7;
    char* path = strchr(host, '/');
    char hostname[64];
    size_t hl = path ? (size_t)(path - host) : strlen(host);
    if (hl >= sizeof hostname) return K32_ERR_NOENT;
    memcpy(hostname, host, hl); hostname[hl] = 0;
    char* colon = strchr(hostname, ':'); if (colon) *colon = 0;
    char* dot = strchr(hostname, '.'); if (dot) *dot = 0;
    shim_service* s = shim_service_get(hostname);
    if (!s || !s->handler) return K32_ERR_NOENT;
    s->calls++;
    memcpy(s->last_method, method, method_len); s->last_method[method_len] = 0;
    snprintf(s->last_path, sizeof s->last_path, "%s", path ? path : "/");
    s->last_body_len = body_len;
    if (s->down) return s->down;
    if (s->fail_calls > 0) { s->fail_calls--; return K32_ERR_TIMEOUT; }

    /* request block: METHOD SP PATH LF headers LF LF body */
    size_t cap = 1024 + headers_len + body_len;
    char* req = malloc(cap);
    int n = snprintf(req, cap, "%.*s %s\n", (int)method_len, method, path ? path : "/");
    for (uint32_t i = 0; i < headers_len; i++) if (headers[i] != '\r') req[n++] = headers[i];
    req[n++] = '\n';
    memcpy(req + n, body, body_len); n += (int)body_len;

    /* the callee answers into its own capture; the caller's response stays as it was */
    shim_state outer = shim;
    shim.body = 0; shim.body_cap = 0; shim.body_len = 0;
    shim.begin_calls = shim.end_calls = 0; shim.header_n = 0; shim.status = 0;
    int64_t t0 = mono_us();
    int32_t rc = s->handler(req, (uint32_t)n);
    uint64_t spent = (uint64_t)(mono_us() - t0);
    int32_t status = shim.begin_calls ? shim.status : (rc < 0 ? 500 : rc);
    uint32_t got = shim.body_len;
    int32_t result = status;
    if (got > resp_cap) { got = resp_cap; result = K32_ERR_TOOBIG; }
    if (got && shim.body) memcpy(resp, shim.body, got);
    *resp_len = got;
    free(shim.body);
    shim_event ev_keep[SHIM_MAX_EVENTS]; int ev_n = shim.event_n; uint64_t ev_total = shim.events_total;
    memcpy(ev_keep, shim.events, sizeof ev_keep);
    shim = outer;
    memcpy(shim.events, ev_keep, sizeof ev_keep); shim.event_n = ev_n; shim.events_total = ev_total;
    shim.call_us += spent;
    free(req);
    if (s->lose_replies > 0) { s->lose_replies--; *resp_len = 0; return K32_ERR_TIMEOUT; }
    return result;
}

/* ---- pod volume ------------------------------------------------------------------------ */
#define FS_FDS 4
static int g_fds[FS_FDS] = { -1, -1, -1, -1 };
static int g_fd_mode[FS_FDS];   /* bit 0 readable, bit 1 writable */

static int32_t map_errno(int e) {
    switch (e) {
        case ENOENT: case ENOTDIR: return K32_ERR_NOENT;
        case EEXIST: return K32_ERR_EXIST;
        case EISDIR: return K32_ERR_ISDIR;
        case ENOSPC: return K32_ERR_NOSPC;
        case EINVAL: case ENOTEMPTY: return K32_ERR_INVAL;
        case EMFILE: case ENFILE: return K32_ERR_MFILE;
        case EACCES: case EPERM: return K32_ERR_PERM;
        default: return K32_ERR_IO;
    }
}
static int32_t full_path(const char* path, uint32_t len, int allow_empty, char* out, size_t cap) {
    if (shim_fs.nodev) return K32_ERR_NODEV;
    if (!shim_fs.root[0]) return K32_ERR_PERM;
    if (len > K32_FS_MAX_PATH) return K32_ERR_NAMETOOLONG;
    if (len == 0 && !allow_empty) return K32_ERR_INVAL;
    if (len && (path[0] == '/' || path[len - 1] == '/')) return K32_ERR_INVAL;
    uint32_t start = 0;
    for (uint32_t i = 0; i <= len; i++) {
        if (i == len || path[i] == '/') {
            uint32_t cl = i - start;
            if (len && (cl == 0 || (cl == 1 && path[start] == '.') || (cl == 2 && path[start] == '.' && path[start + 1] == '.'))) return K32_ERR_INVAL;
            start = i + 1;
        } else {
            char c = path[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return K32_ERR_INVAL;
        }
    }
    snprintf(out, cap, "%s/%.*s", shim_fs.root, (int)len, path);
    return 0;
}
int32_t k32_fs_open(const char* path, uint32_t path_len, uint32_t flags) {
    char full[1024];
    int32_t rc = full_path(path, path_len, 0, full, sizeof full);
    if (rc) return rc;
    uint32_t acc = flags & 3;
    if (acc == 3) return K32_ERR_INVAL;
    int slot = -1;
    for (int i = 0; i < FS_FDS; i++) if (g_fds[i] < 0) { slot = i; break; }
    if (slot < 0) return K32_ERR_MFILE;
    int of = acc == K32_FS_O_WRONLY ? O_WRONLY : acc == K32_FS_O_RDWR ? O_RDWR : O_RDONLY;
    if (flags & K32_FS_O_CREAT) of |= O_CREAT;
    if (flags & K32_FS_O_EXCL) of |= O_EXCL;
    if (flags & K32_FS_O_TRUNC) of |= O_TRUNC;
    if (flags & K32_FS_O_APPEND) of |= O_APPEND;
    int fd = open(full, of, 0666);
    if (fd < 0) return map_errno(errno);
    g_fds[slot] = fd;
    g_fd_mode[slot] = (acc != K32_FS_O_WRONLY ? 1 : 0) | (acc != K32_FS_O_RDONLY ? 2 : 0);
    shim_fs.opens++; shim_fs.open_now++;
    return slot;
}
int32_t k32_fs_read(int32_t fd, void* buf, uint32_t len) {
    if (fd < 0 || fd >= FS_FDS || g_fds[fd] < 0 || !(g_fd_mode[fd] & 1)) return K32_ERR_BADF;
    if (len > K32_FS_MAX_IO) len = K32_FS_MAX_IO;
    ssize_t n = read(g_fds[fd], buf, len);
    if (n < 0) return map_errno(errno);
    shim_fs.reads++; shim_fs.read_bytes += (uint64_t)n;
    return (int32_t)n;
}
int32_t k32_fs_write(int32_t fd, const void* buf, uint32_t len) {
    if (fd < 0 || fd >= FS_FDS || g_fds[fd] < 0 || !(g_fd_mode[fd] & 2)) return K32_ERR_BADF;
    if (len > K32_FS_MAX_IO) len = K32_FS_MAX_IO;
    if (shim_fs.fail_write_after >= 0) {
        uint32_t keep = (uint32_t)shim_fs.fail_write_after < len ? (uint32_t)shim_fs.fail_write_after : len;
        shim_fs.fail_write_after = -1;
        if (keep && write(g_fds[fd], buf, keep) < 0) return map_errno(errno);
        return K32_ERR_IO;
    }
    ssize_t n = write(g_fds[fd], buf, len);
    if (n < 0) return map_errno(errno);
    shim_fs.writes++; shim_fs.write_bytes += (uint64_t)n;
    return (int32_t)n;
}
int32_t k32_fs_close(int32_t fd) {
    if (fd < 0 || fd >= FS_FDS || g_fds[fd] < 0) return K32_ERR_BADF;
    int rc = close(g_fds[fd]);
    g_fds[fd] = -1;
    shim_fs.open_now--;
    return rc == 0 ? 0 : map_errno(errno);
}
int32_t k32_fs_size(int32_t fd) {
    if (fd < 0 || fd >= FS_FDS || g_fds[fd] < 0) return K32_ERR_BADF;
    struct stat st;
    if (fstat(g_fds[fd], &st) != 0) return map_errno(errno);
    return (int32_t)st.st_size;
}
int32_t k32_fs_list(const char* dir, uint32_t dir_len, char* out, uint32_t out_cap) {
    char full[1024];
    int32_t rc = full_path(dir, dir_len, 1, full, sizeof full);
    if (rc) return rc;
    if (dir_len == 0) snprintf(full, sizeof full, "%s", shim_fs.root);
    if (out_cap > K32_FS_MAX_IO) out_cap = K32_FS_MAX_IO;
    DIR* d = opendir(full);
    if (!d) return map_errno(errno);
    uint32_t w = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || (e->d_name[1] == '.' && e->d_name[2] == 0))) continue;
        size_t n = strlen(e->d_name);
        if (w + n + 2 > out_cap) break;
        memcpy(out + w, e->d_name, n); w += (uint32_t)n;
        out[w++] = '\n';
    }
    closedir(d);
    return (int32_t)w;
}
int32_t k32_fs_remove(const char* path, uint32_t path_len) {
    char full[1024];
    int32_t rc = full_path(path, path_len, 0, full, sizeof full);
    if (rc) return rc;
    struct stat st;
    if (stat(full, &st) != 0) return map_errno(errno);
    int r = S_ISDIR(st.st_mode) ? rmdir(full) : unlink(full);
    return r == 0 ? 0 : map_errno(errno);
}

int shim_fs_reset_dir(const char* path) {
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        char f[1024];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(f, sizeof f, "%s/%s", path, e->d_name);
            unlink(f);
        }
        closedir(d);
    } else {
        char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", path);
        for (char* p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0777); *p = '/'; }
        if (mkdir(path, 0777) != 0 && errno != EEXIST) return -1;
    }
    return 0;
}
