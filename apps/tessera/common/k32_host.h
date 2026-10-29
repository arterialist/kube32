/*
 * Host ABI bridge for the Tessera pods. Under wasm32 the ABI header declares
 * the imports with K32_IMPORT; the native test build declares every import
 * as a plain extern implemented by common/host/shim.c.
 */
#ifndef TESSERA_K32_HOST_H
#define TESSERA_K32_HOST_H

#include "../../../include/runtime/k32_app_abi.h"

#if !defined(__wasm32__)
#define K32_EXPORT(name)

void     k32_resp_begin(int32_t status, const char* content_type, uint32_t content_type_len);
void     k32_resp_header(const char* name, uint32_t name_len, const char* value, uint32_t value_len);
int32_t  k32_resp_write(const void* data, uint32_t len);
void     k32_resp_end(void);
int32_t  k32_event_emit(const char* topic, uint32_t topic_len, const void* data, uint32_t len);
int64_t  k32_time_ms(void);
void     k32_log(const char* msg, uint32_t len);
uint32_t k32_random(void);
int32_t  k32_env_get(const char* key, uint32_t key_len, char* out, uint32_t out_cap);
int32_t  k32_mem_stats(uint32_t* out_used, uint32_t* out_limit);

int32_t  k32_fs_open(const char* path, uint32_t path_len, uint32_t flags);
int32_t  k32_fs_read(int32_t fd, void* buf, uint32_t len);
int32_t  k32_fs_write(int32_t fd, const void* buf, uint32_t len);
int32_t  k32_fs_close(int32_t fd);
int32_t  k32_fs_size(int32_t fd);
int32_t  k32_fs_list(const char* dir, uint32_t dir_len, char* out, uint32_t out_cap);
int32_t  k32_fs_remove(const char* path, uint32_t path_len);

int32_t  k32_http_call(const char* method, uint32_t method_len, const char* url, uint32_t url_len,
                       const char* headers, uint32_t headers_len, const uint8_t* body, uint32_t body_len,
                       uint8_t* resp, uint32_t resp_cap, uint32_t* resp_len, uint32_t timeout_ms);
int32_t  k32_resp_asset(const char* path, uint32_t path_len, uint32_t status);

#endif

/* Guest exports (same prototypes in both builds). */
K32_EXPORT("k32_init")   int32_t  k32_init(void);
K32_EXPORT("k32_alloc")  uint32_t k32_alloc(uint32_t size);
K32_EXPORT("k32_handle") int32_t  k32_handle(uint32_t ptr, uint32_t len);

#endif
