/*
 * Host ABI bridge. Under wasm32 the ABI header declares the imports with
 * K32_IMPORT; the native test build declares the same functions as plain
 * externs implemented by host_test/shim.c.
 */
#ifndef K32_HOST_H
#define K32_HOST_H

#include "../../../../include/runtime/k32_app_abi.h"

#if !defined(__wasm32__)
void    k32_resp_begin(int32_t status, const char* content_type, uint32_t content_type_len);
void    k32_resp_header(const char* name, uint32_t name_len, const char* value, uint32_t value_len);
int32_t k32_resp_write(const void* data, uint32_t len);
void    k32_resp_end(void);
int32_t k32_event_emit(const char* topic, uint32_t topic_len, const void* data, uint32_t len);
int64_t k32_time_ms(void);
void    k32_log(const char* msg, uint32_t len);
uint32_t k32_random(void);
int32_t k32_env_get(const char* key, uint32_t key_len, char* out, uint32_t out_cap);
int32_t k32_mem_stats(uint32_t* out_used, uint32_t* out_limit);
#define K32_EXPORT(name)
#endif

/* Guest exports (same prototypes in both builds). */
K32_EXPORT("k32_init")   int32_t  k32_init(void);
K32_EXPORT("k32_alloc")  uint32_t k32_alloc(uint32_t size);
K32_EXPORT("k32_handle") int32_t  k32_handle(uint32_t ptr, uint32_t len);
K32_EXPORT("k32_tick")   int32_t  k32_tick(uint32_t uptime_ms);
K32_EXPORT("k32_event")  int32_t  k32_event(uint32_t topic_ptr, uint32_t topic_len, uint32_t ptr, uint32_t len);

#endif
