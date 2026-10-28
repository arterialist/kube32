/*
 * Kube32 application ABI for request-handling WebAssembly pods.
 *
 * This header is shared between the firmware (host side, C++) and the
 * WebAssembly workloads (guest side, freestanding C compiled with
 * clang --target=wasm32). It defines the names and signatures of the
 * functions the host links into the "env" import module and the exports
 * the host looks up in the guest.
 *
 * Request encoding handed to k32_handle():
 *
 *   METHOD SP PATH[?QUERY] LF
 *   (Header-Name ":" SP value LF)*
 *   LF
 *   body bytes
 *
 * The host forwards only a small, fixed set of headers: Cookie,
 * Content-Type, X-Request-Id, X-Forwarded-For. The guest parses the block
 * in place and must not keep pointers into it after returning.
 *
 * Response: the guest calls k32_resp_begin() once, optionally
 * k32_resp_header() zero or more times, then k32_resp_write() any number
 * of times, then k32_resp_end(). The host streams the body with HTTP
 * chunked transfer encoding, so the guest never needs a buffer for the
 * whole response. If k32_handle() returns without calling
 * k32_resp_begin() the host replies 500.
 */
#ifndef KUBE32_APP_ABI_H
#define KUBE32_APP_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Guest exports the host looks up by name ------------------------- */
#define K32_EXPORT_INIT    "k32_init"    /* int32 k32_init(void)                        */
#define K32_EXPORT_ALLOC   "k32_alloc"   /* uint32 k32_alloc(uint32 size)               */
#define K32_EXPORT_HANDLE  "k32_handle"  /* int32 k32_handle(uint32 ptr, uint32 len)    */
#define K32_EXPORT_TICK    "k32_tick"    /* int32 k32_tick(uint32 uptime_ms)  optional  */
#define K32_EXPORT_EVENT   "k32_event"   /* int32 k32_event(uint32 topic_ptr, uint32 topic_len, uint32 ptr, uint32 len) optional */

/* Return codes for k32_handle: an HTTP status (100..599) after a complete
 * response was produced, or a negative error. */
#define K32_HANDLE_ERR_NO_RESPONSE   (-1)
#define K32_HANDLE_ERR_BAD_REQUEST   (-2)

/* Hardware error codes mirror hal_types.hpp */
#define K32_OK          0
#define K32_ERR_PERM   (-1)
#define K32_ERR_INVAL  (-22)
#define K32_ERR_AGAIN  (-11)

/* Limits enforced by the host */
#define K32_MAX_REQUEST_BYTES   (16 * 1024)
#define K32_MAX_EVENT_BYTES     (200)        /* fits one ESP-NOW frame after the K32C header */
#define K32_MAX_HEADER_BYTES    (256)

#if defined(__wasm32__)
#define K32_IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define K32_EXPORT(name) __attribute__((export_name(name)))

/* ---- Host imports available to every app pod ------------------------- */
K32_IMPORT("k32_resp_begin")  void    k32_resp_begin(int32_t status, const char* content_type, uint32_t content_type_len);
K32_IMPORT("k32_resp_header") void    k32_resp_header(const char* name, uint32_t name_len, const char* value, uint32_t value_len);
K32_IMPORT("k32_resp_write")  int32_t k32_resp_write(const void* data, uint32_t len);
K32_IMPORT("k32_resp_end")    void    k32_resp_end(void);

/* Publish an event on a cluster virtual channel (ESP-NOW on hardware).
 * topic is the channel name, e.g. "tessera.events". Requires the
 * CHANNEL_IPC capability. Returns bytes sent or a negative error. */
K32_IMPORT("k32_event_emit")  int32_t k32_event_emit(const char* topic, uint32_t topic_len, const void* data, uint32_t len);

K32_IMPORT("k32_time_ms")     int64_t k32_time_ms(void);      /* milliseconds since node boot */
K32_IMPORT("k32_log")         void    k32_log(const char* msg, uint32_t len);
K32_IMPORT("k32_random")      uint32_t k32_random(void);      /* hardware RNG on ESP32 */
K32_IMPORT("k32_env_get")     int32_t k32_env_get(const char* key, uint32_t key_len, char* out, uint32_t out_cap); /* value length, or -1 */
K32_IMPORT("k32_mem_stats")   int32_t k32_mem_stats(uint32_t* out_used, uint32_t* out_limit);

/* ---- Hardware imports, gated by the capability firewall ---------------
 * The pod manifest must request the matching resource:
 *   hardware.kube32.io/tft-display: "1"  -> k32_display_*
 *   hardware.kube32.io/keyboard: "1"     -> k32_key_read
 *   hardware.kube32.io/speaker: "1"      -> k32_tone
 * Calls without the grant return K32_ERR_PERM and are logged as violations. */
K32_IMPORT("k32_display_text") int32_t k32_display_text(int32_t x, int32_t y, const char* text, uint32_t len, uint32_t fg565, uint32_t bg565);
K32_IMPORT("k32_display_rect") int32_t k32_display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color565);
K32_IMPORT("k32_display_bar")  int32_t k32_display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg565, uint32_t bg565);
K32_IMPORT("k32_key_read")     int32_t k32_key_read(void);    /* next key code (ASCII), K32_ERR_AGAIN when empty */
K32_IMPORT("k32_tone")         int32_t k32_tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct);

/* Existing low-level imports (see host_functions.cpp) remain available:
 * esp32_gpio_set_mode, esp32_gpio_write, esp32_gpio_read, esp32_adc_read,
 * esp32_i2c_write, esp32_i2c_read, esp32_uart_write, esp32_uart_read,
 * esp32_get_time_us, esp32_delay_ms, kube32_channel_open/send/recv/close. */
K32_IMPORT("esp32_delay_ms")      void    esp32_delay_ms(uint32_t ms);
K32_IMPORT("kube32_channel_open") int32_t kube32_channel_open(const char* name, uint32_t flags);
K32_IMPORT("kube32_channel_send") int32_t kube32_channel_send(int32_t handle, const void* data, uint32_t len);
K32_IMPORT("kube32_channel_recv") int32_t kube32_channel_recv(int32_t handle, void* buf, uint32_t max_len, uint32_t timeout_ms);
K32_IMPORT("kube32_channel_close") int32_t kube32_channel_close(int32_t handle);

#endif /* __wasm32__ */

#ifdef __cplusplus
}
#endif

#endif /* KUBE32_APP_ABI_H */
