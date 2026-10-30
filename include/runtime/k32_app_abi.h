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
 * whole response. Headers set before k32_resp_begin() (or before
 * k32_resp_asset()) are kept and sent with the response; setting a name to
 * an empty value before the response starts withdraws the headers of that
 * name set so far (names compare without case). If k32_handle() returns
 * without a response the host replies 500.
 *
 * Pod annotation kube32.io/wasm-stack (e.g. "2Ki", 512 bytes to 64Ki) sets
 * the module's operand stack, by default 4 KiB for pods of at most one page;
 * a pod that exhausts it traps with exit code 139 and a message naming it.
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

/* k32_event receives every message published on the channel topics listed in
 * the pod's annotation kube32.io/event-topics (comma separated), from any
 * node:
 *
 *   metadata:
 *     annotations:
 *       kube32.io/event-topics: myapp.input,myapp.events
 */

/* Return codes for k32_handle: an HTTP status (100..599) after a complete
 * response was produced, or a negative error. */
#define K32_HANDLE_ERR_NO_RESPONSE   (-1)
#define K32_HANDLE_ERR_BAD_REQUEST   (-2)

/* Error codes (negative POSIX errno values, mirroring hal_types.hpp) */
#define K32_OK               0
#define K32_ERR_PERM        (-1)    /* capability not granted to this pod            */
#define K32_ERR_NOENT       (-2)    /* no such file or directory                     */
#define K32_ERR_IO          (-5)    /* I/O error: the card, or the connection to a remote pod */
#define K32_ERR_TOOBIG      (-7)    /* response body or IR frame larger than the buffer (E2BIG) */
#define K32_ERR_BADF        (-9)    /* fd not open, or opened for the other direction */
#define K32_ERR_AGAIN       (-11)   /* nothing queued yet (key, IR frame)              */
#define K32_ERR_NOMEM       (-12)
#define K32_ERR_FAULT       (-14)   /* pointer/length outside linear memory           */
#define K32_ERR_BUSY        (-16)   /* every TLS session for https calls is in use (EBUSY) */
#define K32_ERR_EXIST       (-17)   /* K32_FS_O_EXCL and the file exists              */
#define K32_ERR_NODEV       (-19)   /* no card mounted on this node                   */
#define K32_ERR_ISDIR       (-21)
#define K32_ERR_INVAL       (-22)   /* bad path (absolute, "..", empty), bad flags    */
#define K32_ERR_MFILE       (-24)   /* 4 files already open (per pod or node-wide)    */
#define K32_ERR_NOSPC       (-28)   /* card full                                      */
#define K32_ERR_NAMETOOLONG (-36)   /* path longer than K32_FS_MAX_PATH               */
#define K32_ERR_LOOP        (-40)   /* the call would re-enter a pod already serving it (ELOOP) */
#define K32_ERR_TLS         (-71)   /* the TLS handshake failed (EPROTO)              */
#define K32_ERR_NOTSUP      (-95)   /* https:// URL on a cluster without TLS (EOPNOTSUPP) */
#define K32_ERR_TIMEOUT     (-110)  /* no answer within the timeout (ETIMEDOUT)       */
#define K32_ERR_CONNREFUSED (-111)  /* the host refused the connection (ECONNREFUSED) */
#define K32_ERR_HOSTUNREACH (-113)  /* no route to the host (EHOSTUNREACH)            */
#define K32_ERR_CERT        (-129)  /* the server's TLS certificate was not accepted (EKEYREJECTED) */

/* ---- Pod storage (microSD volume) -------------------------------------
 * A pod gets a directory of its own on the node's microSD card when its
 * manifest requests the resource and names a volume claim:
 *
 *   spec:
 *     volumes:
 *       - name: data
 *         kube32.io/sd: { claim: notes }        # -> /sd/kube32/volumes/notes/
 *     containers:
 *       - resources:
 *           requests: { hardware.kube32.io/sd: "1" }
 *
 * Every k32_fs_* path is relative to that directory: "boot.log",
 * "logs/today.txt". Absolute paths and ".." components are rejected with
 * K32_ERR_INVAL. Without the grant every call returns K32_ERR_PERM; on a
 * node without a card, K32_ERR_NODEV. File descriptors are small integers
 * (0..3); a pod may hold at most K32_FS_MAX_FDS open at once. One read or
 * write call moves at most K32_FS_MAX_IO bytes; loop for more. See
 * docs/sd-card.md. */
#define K32_FS_MAX_PATH   96     /* bytes, relative to the volume root */
#define K32_FS_MAX_FDS    4      /* open files per pod */
#define K32_FS_MAX_IO     4096   /* bytes per k32_fs_read / k32_fs_write / k32_fs_list call */

/* Flags for k32_fs_open (Linux values) */
#define K32_FS_O_RDONLY   0x0000
#define K32_FS_O_WRONLY   0x0001
#define K32_FS_O_RDWR     0x0002
#define K32_FS_O_CREAT    0x0040
#define K32_FS_O_EXCL     0x0080
#define K32_FS_O_TRUNC    0x0200
#define K32_FS_O_APPEND   0x0400

/* Limits enforced by the host */
#define K32_MAX_REQUEST_BYTES   (16 * 1024)
#define K32_MAX_EVENT_BYTES     (200)        /* fits one ESP-NOW frame after the K32C header */
#define K32_MAX_HEADER_BYTES    (256)

/* ---- Infrared: RMT pulse trains, gated by the capability firewall -------
 * The pod claims one RMT channel per direction and names its pin in an
 * annotation, or uses the board's default pin (the Cardputer's TX default is
 * its IR LED on GPIO 44; RX has no default):
 *
 *   metadata:
 *     annotations:
 *       kube32.io/rmt-tx: "pin=2,carrier=38000,duty=33"   # carrier=0 sends bare pulses
 *       kube32.io/rmt-rx: "pin=1,active=low,idle_us=12000"
 *   spec:
 *     containers:
 *       - resources:
 *           requests: { esp32.io/rmt-tx: "1", esp32.io/rmt-rx: "1" }
 *
 * A frame is alternating mark and space durations in microseconds (1 to
 * 65535 each), mark first. esp32_rmt_send transmits one, marks modulated
 * with the claim's carrier, and returns when it is out: count, or
 * K32_ERR_PERM (no esp32.io/rmt-tx claim), K32_ERR_INVAL (count 0 or above
 * K32_RMT_MAX_DURATIONS, a zero duration, a frame longer than
 * K32_RMT_MAX_FRAME_US), K32_ERR_FAULT (durations outside linear memory).
 * The calling pod waits for the frame; where pods share one interpreter
 * (boards without PSRAM) every other pod's call waits as well.
 *
 * The receiver is armed from admission until the pod ends. It queues up to
 * K32_RMT_RX_QUEUE_FRAMES frames, K32_RMT_RX_QUEUE_DURATIONS durations in
 * all; a space of idle_us ends a frame. esp32_rmt_receive never waits: it
 * returns the duration count of the oldest frame (a mark is the receiver's
 * active level), K32_ERR_AGAIN when none is queued, K32_ERR_TOOBIG when the
 * frame has more than cap durations (the first cap are copied and the frame
 * leaves the queue), or K32_ERR_PERM (no esp32.io/rmt-rx claim). The next
 * call reports frames the receiver dropped (queue full, or longer than
 * K32_RMT_MAX_DURATIONS) in the pod's log. */
#define K32_RMT_MAX_DURATIONS       512       /* per frame, sent or received */
#define K32_RMT_MAX_FRAME_US        500000    /* total length of a frame sent */
#define K32_RMT_RX_QUEUE_FRAMES     8
#define K32_RMT_RX_QUEUE_DURATIONS  512

#if defined(__wasm32__)
#define K32_IMPORT(name) __attribute__((import_module("env"), import_name(name)))
#define K32_EXPORT(name) __attribute__((export_name(name)))

/* ---- Host imports available to every app pod ------------------------- */
K32_IMPORT("k32_resp_begin")  void    k32_resp_begin(int32_t status, const char* content_type, uint32_t content_type_len);
K32_IMPORT("k32_resp_header") void    k32_resp_header(const char* name, uint32_t name_len, const char* value, uint32_t value_len);
K32_IMPORT("k32_resp_write")  int32_t k32_resp_write(const void* data, uint32_t len);
K32_IMPORT("k32_resp_end")    void    k32_resp_end(void);

/* Publish an event on a cluster virtual channel (ESP-NOW on hardware).
 * topic is the channel name, e.g. "myapp.events". Requires the
 * CHANNEL_IPC capability. Returns bytes sent or a negative error. */
K32_IMPORT("k32_event_emit")  int32_t k32_event_emit(const char* topic, uint32_t topic_len, const void* data, uint32_t len);

K32_IMPORT("k32_time_ms")     int64_t k32_time_ms(void);      /* milliseconds since node boot */
K32_IMPORT("k32_log")         void    k32_log(const char* msg, uint32_t len);
K32_IMPORT("k32_random")      uint32_t k32_random(void);      /* hardware RNG on ESP32 */
K32_IMPORT("k32_env_get")     int32_t k32_env_get(const char* key, uint32_t key_len, char* out, uint32_t out_cap); /* value length, or -1 */
K32_IMPORT("k32_mem_stats")   int32_t k32_mem_stats(uint32_t* out_used, uint32_t* out_limit);

/* Sends a file from this pod's image bundle as the response to the current
 * request. path is relative to the bundle root ("index.html", "js/app.js").
 * The host prefers the .gz variant when the client accepts gzip, sets
 * Content-Type from the extension, streams the file from flash (or the card)
 * without copying it into linear memory, and keeps headers set earlier with
 * k32_resp_header. status 0 means 200. Returns 0 once the response is sent,
 * or K32_ERR_NOENT (no such file): then nothing was sent and the headers set
 * earlier are still pending, so the guest can answer itself (a 404, say).
 *
 * The bundle of an embedded image <name> is images/web/<name>/ in the
 * firmware; the bundle of wasm://sd/<name> is the directory
 * kube32/images/<name>/ on the card. A file stored only gzip-compressed is
 * answered 406 to a client that does not accept gzip. */
K32_IMPORT("k32_resp_asset")  int32_t k32_resp_asset(const char* path, uint32_t path_len, uint32_t status);

/* Synchronous HTTP request to a Service in the cluster or to a host outside it.
 * url: "http://<host>[:<port>]<path>" or "https://<host>[:<port>]<path>".
 * An http:// host resolves as Kubernetes DNS names do in a pod:
 *   <service>                                a Service in the caller's namespace
 *   <service>.<namespace>                    that Service; a host on the network
 *                                            when there is no such Service
 *   <service>.<namespace>.svc[.cluster.local] a Service
 *   anything else                            a host outside the cluster: an IPv4
 *                                            address or a DNS name (port 80)
 * An https:// host is always outside the cluster (port 443). Its certificate
 * must chain to a trusted root (the common public CAs) and name the host.
 * The control plane makes the TLS connection: for its own pods, and for pods
 * on workers, whose node forwards the call to the control plane's egress
 * gateway. The calling pod's NetworkPolicies apply either way.
 * A Service's port defaults to its first port. headers: optional
 * "Name: value\r\n" lines. The response body is copied into resp up to
 * resp_cap; *resp_len receives the number of bytes copied.
 * Returns the HTTP status, or a negative error:
 *   K32_ERR_NOENT       no such Service, no ready pod behind it, or the
 *                       host name does not resolve
 *   K32_ERR_TIMEOUT     no complete answer within timeout_ms (0: 5000 ms)
 *   K32_ERR_TOOBIG      the body is larger than resp_cap (the first resp_cap
 *                       bytes are copied; k32_http_status() gives the status)
 *   K32_ERR_LOOP        the target pod is already serving this call chain
 *                       (a pod calling itself, or a cycle across pods)
 *   K32_ERR_IO          the pod's node or the control plane could not be
 *                       reached, or the connection failed
 *   K32_ERR_CONNREFUSED the host refused the connection
 *   K32_ERR_HOSTUNREACH no route to the host
 *   K32_ERR_CERT        https: the certificate is not trusted, names another
 *                       host, or is outside its validity dates
 *   K32_ERR_TLS         https: the TLS handshake failed otherwise
 *   K32_ERR_BUSY        https: every TLS session of the control plane is in
 *                       use; try again later
 *   K32_ERR_NOTSUP      https: the cluster has no TLS client
 *   K32_ERR_PERM        the pod lacks the SERVICE_CALL capability:
 *                         securityContext: { capabilities: { add: [SERVICE_CALL] } }
 *                       or a NetworkPolicy does not allow the connection, or
 *                       the egress gateway did not accept the pod's node
 * A pod on the same node is called in-process; others are reached through
 * their node's pod HTTP server. The calling pod is suspended (its thread
 * blocks) until the answer arrives. Request headers the target sees:
 * Content-Type, Cookie, X-Request-Id and Accept-Encoding; a host outside the
 * cluster gets every header except the ones the connection owns (Host,
 * Content-Length, Connection, ...). */
K32_IMPORT("k32_http_call")   int32_t k32_http_call(const char* method, uint32_t method_len, const char* url, uint32_t url_len,
                                                    const char* headers, uint32_t headers_len, const uint8_t* body, uint32_t body_len,
                                                    uint8_t* resp, uint32_t resp_cap, uint32_t* resp_len, uint32_t timeout_ms);
/* The HTTP status of the answer to this pod's last k32_http_call, also when
 * that call returned K32_ERR_TOOBIG; 0 when no answer arrived. */
K32_IMPORT("k32_http_status") int32_t k32_http_status(void);

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

/* ---- Pod storage imports, gated by hardware.kube32.io/sd -------------- */
K32_IMPORT("k32_fs_open")   int32_t k32_fs_open(const char* path, uint32_t path_len, uint32_t flags);   /* fd (0..3) or error */
K32_IMPORT("k32_fs_read")   int32_t k32_fs_read(int32_t fd, void* buf, uint32_t len);                   /* bytes read, 0 at end, or error */
K32_IMPORT("k32_fs_write")  int32_t k32_fs_write(int32_t fd, const void* buf, uint32_t len);            /* bytes written or error */
K32_IMPORT("k32_fs_close")  int32_t k32_fs_close(int32_t fd);                                           /* 0 or error */
K32_IMPORT("k32_fs_size")   int32_t k32_fs_size(int32_t fd);                                            /* file size in bytes or error */
K32_IMPORT("k32_fs_list")   int32_t k32_fs_list(const char* dir, uint32_t dir_len, char* out, uint32_t out_cap); /* bytes written: "name\n" per entry, directories end in "/" */
K32_IMPORT("k32_fs_remove") int32_t k32_fs_remove(const char* path, uint32_t path_len);                 /* 0 or error; removes a file or an empty directory */

/* ---- I2C and UART, gated by the capability firewall --------------------
 * The pod claims a bus as a resource and may name its pins in an annotation
 * (the board's default pins otherwise):
 *
 *   metadata:
 *     annotations:
 *       kube32.io/i2c-bus-0: "sda=8,scl=9,hz=100000"
 *       kube32.io/uart-1: "tx=17,rx=18,baud=115200"
 *   spec:
 *     containers:
 *       - resources:
 *           requests: { esp32.io/i2c-bus-0: "1", esp32.io/uart-1: "1" }
 *
 * The node installs the driver on those pins while the pod runs. Calls on a
 * bus the pod did not claim return K32_ERR_PERM. */
K32_IMPORT("esp32_i2c_write")  int32_t esp32_i2c_write(uint32_t bus, uint32_t addr, const void* data, uint32_t len); /* bytes written; len 0 probes the address: 0 = a device answered, K32_ERR_NODEV = none */
K32_IMPORT("esp32_i2c_read")   int32_t esp32_i2c_read(uint32_t bus, uint32_t addr, void* buf, uint32_t len);         /* bytes read or error */
K32_IMPORT("esp32_uart_write") int32_t esp32_uart_write(uint32_t port, const void* data, uint32_t len);              /* bytes queued or error */
K32_IMPORT("esp32_uart_read")  int32_t esp32_uart_read(uint32_t port, void* buf, uint32_t max_len);                  /* bytes received within 50 ms, or error */

/* ---- Infrared, gated by esp32.io/rmt-tx and esp32.io/rmt-rx (see above) -- */
K32_IMPORT("esp32_rmt_send")    int32_t esp32_rmt_send(const uint16_t* durations_us, uint32_t count);   /* count once sent, or error */
K32_IMPORT("esp32_rmt_receive") int32_t esp32_rmt_receive(uint16_t* out_us, uint32_t cap);              /* durations of the oldest frame, K32_ERR_AGAIN, or error */

/* Existing low-level imports (see host_functions.cpp) remain available:
 * esp32_gpio_set_mode, esp32_gpio_write, esp32_gpio_read, esp32_adc_read,
 * esp32_i2c_write, esp32_i2c_read, esp32_uart_write, esp32_uart_read,
 * esp32_rmt_send, esp32_rmt_receive, esp32_get_time_us, esp32_delay_ms, kube32_channel_open/send/recv/close. */
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
