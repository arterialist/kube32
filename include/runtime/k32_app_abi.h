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
#define K32_EXPORT_RESTORED "k32_restored" /* int32 k32_restored(void)                optional  */

/* k32_restored runs once, in place of k32_init, when the pod starts from a
 * snapshot taken on another node (docs/migration.md): memory, globals and
 * k32_time_ms continue from the snapshot. A pod refreshes what depends on
 * the node there; a non-zero result fails the container like k32_init. */

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

/* ---- Blobs: large read-only data, read in place ------------------------
 * A blob is a named, read-only byte string the node keeps for its pods, such
 * as model weights: in the node's flash blob region (mapped into the address
 * space) or as a file in kube32/blobs/ on its microSD card (docs/storage.md).
 * A pod may open the blobs its annotation names, at most K32_BLOB_MAX_OPEN at
 * once:
 *
 *   metadata:
 *     annotations:
 *       kube32.io/blobs: stories-weights,stories-tokenizer
 *
 * k32_blob_read copies a range into linear memory. A blob that
 * k32_blob_mapped reports as mapped is also read in place by the linear
 * algebra imports: pass K32_BLOB_ADDR(blob, offset) as their w or w_scale
 * operand, and the weights never enter the pod's memory. A blob address is
 * no linear memory address; any other import rejects it as out of bounds.
 * The kernels answer K32_ERR_NOTSUP for a blob that is not mapped (a file
 * on a board's card): read its rows into memory instead.
 *
 * Errors: K32_ERR_PERM (the name is not in kube32.io/blobs), K32_ERR_NOENT
 * (no such blob on this node), K32_ERR_NODEV (the node keeps no blobs),
 * K32_ERR_MFILE (K32_BLOB_MAX_OPEN open), K32_ERR_BADF (no such handle),
 * K32_ERR_INVAL, K32_ERR_NAMETOOLONG, K32_ERR_FAULT, K32_ERR_IO. Handles are
 * closed when the container stops or restarts; a pod restored from a
 * checkpoint opens its blobs again in k32_restored. */
#define K32_BLOB_MAX_OPEN      4
#define K32_BLOB_NAME_MAX      47            /* bytes of [A-Za-z0-9._-] */
#define K32_BLOB_ADDR_FLAG     0x80000000u
#define K32_BLOB_HANDLE_SHIFT  28
#define K32_BLOB_OFFSET_MASK   0x0FFFFFFFu   /* blob addresses reach the first 256 MiB of a blob */
#define K32_BLOB_ADDR(blob, offset) \
    ((const void*)(uintptr_t)(K32_BLOB_ADDR_FLAG | ((uint32_t)(blob) << K32_BLOB_HANDLE_SHIFT) | ((uint32_t)(offset) & K32_BLOB_OFFSET_MASK)))

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

/* e^x as k32_softmax_f32 computes it on every node: x = n ln2 + r with
 * |r| <= ln2 / 2, then a degree-6 polynomial for e^r, each step one IEEE
 * binary32 operation. 0 below -87, e^88 above 88. A pod that computes exp
 * with this function gets the host's bits; build it without fused
 * multiply-adds (Wasm has none). */
static inline float k32_expf(float x) {
    if (x < -87.0f) return 0.0f;
    if (x > 88.0f) x = 88.0f;
    float t = x * 1.44269504f;
    int32_t n = (int32_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
    float r = x - (float)n * 0.693145752f;
    r = r - (float)n * 1.42860677e-6f;
    float p = 1.38888889e-3f;
    p = p * r + 8.33333333e-3f;
    p = p * r + 4.16666667e-2f;
    p = p * r + 1.66666667e-1f;
    p = p * r + 0.5f;
    p = p * r + 1.0f;
    p = p * r + 1.0f;
    uint32_t bits = (uint32_t)(n + 127) << 23;
    float scale;
    __builtin_memcpy(&scale, &bits, sizeof scale);
    return p * scale;
}

/* Modes for esp32_gpio_set_mode */
#define K32_GPIO_INPUT          0
#define K32_GPIO_OUTPUT         1
#define K32_GPIO_INPUT_PULLUP   2
#define K32_GPIO_INPUT_PULLDOWN 3

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
K32_IMPORT("k32_unix_time_ms") int64_t k32_unix_time_ms(void); /* wall clock, Unix milliseconds; 0 while the node's clock is not set */
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

/* ---- Linear algebra: quantized products for neural networks -----------
 * The node computes these natively. They need no capability. Every float
 * step is one IEEE binary32 operation in the order written here, and
 * integer sums are exact int32 values (wrapping beyond that range), so every
 * node computes the same bits. A metered pod pays 10,000 gas units plus 45
 * per unit of work before the call runs: rows x (cols + 1) units for a
 * matrix (n times that for k32_matmul_*), 32 per element for
 * k32_softmax_f32 and k32_swiglu_f32, 4 for k32_rmsnorm_f32 and
 * k32_quantize_i8 and k32_quantize_i8_static, 3 for k32_rope_f32, 2 for
 * k32_add_f32_i8, 1 for k32_add_f32 and k32_argmax_f32, 8 per butterfly for
 * k32_fft_q15; a convolution pays for a cout x 9 cin matrix per pixel,
 * and k32_attention_f32_i8 for the two products and the softmax of every
 * head of every position.
 *
 * w is a matrix of rows x cols weights, row r starting row_stride bytes
 * after row r - 1 (0: the rows follow each other); w_scale holds one float
 * per row, scale_stride bytes apart (0: 4). w and w_scale may also be blob
 * addresses (K32_BLOB_ADDR): the node reads them from the blob in place, and
 * a blob that is not mapped makes the call return K32_ERR_NOTSUP. out must
 * not overlap the other operands. Each call returns K32_OK, or K32_ERR_INVAL for a stride that is
 * not 0 and below a row's bytes (or 4 for scale_stride). An operand outside
 * the linear memory, or a blob address outside every open blob, traps (exit
 * code 139) with a message that names it; a call writes nothing outside the
 * linear memory.
 *
 * k32_matvec_i8, k32_matvec_i4: int8 weights times an int8 vector x of cols
 * values with the scale x_scale, into rows floats:
 *   out[r] = (float)(sum over c of w[r][c] * x[c]) * (w_scale[r] * x_scale)
 * k32_matvec_i4 takes int4 weights, two a byte: column 2j in the low nibble
 * and 2j + 1 in the high nibble, each a signed value -8..7; with an odd cols
 * a row ends in an unused high nibble.
 *
 * k32_matmul_i8, k32_matmul_i4: the same product for n vectors at once, x
 * holding n vectors of cols int8 values one after another and x_scale n
 * floats, into n vectors of rows floats:
 *   out[p * rows + r] = (float)(sum over c of w[r][c] * x[p * cols + c]) * (w_scale[r] * x_scale[p])
 * the bits of n k32_matvec calls. The node reads each row of w once for all
 * n vectors, so weights that stream from flash are read once a call instead
 * of once a vector. w_scale is packed (4 bytes apart).
 *
 * k32_matvec_f32_i8: int8 rows times a float vector x of cols values, the
 * sum from 0 in column order (attention scores over an int8 key cache):
 *   out[r] = ((sum over c of x[c] * (float)w[r][c]) * w_scale[r]) * alpha
 *
 * k32_vecmat_f32_i8: a weighted sum of int8 rows, x holding rows floats, the
 * sum from 0 in row order, into cols floats (attention over a value cache):
 *   out[c] = sum over r of (x[r] * w_scale[r]) * (float)w[r][c]
 *
 * k32_softmax_f32: x[i] = k32_expf(x[i] - max) / sum, in place over n
 * floats, the sum from 0 in order.
 *
 * Element-wise operations, over count vectors of n floats one after another
 * (the positions of a batch, or the heads of one), each element as one
 * IEEE binary32 operation per step in the order written:
 *
 * k32_rmsnorm_f32: out = w * (x * r) with r = 1 / sqrt(ss / n + eps), ss the
 * sum of x^2 from 0 in order; w holds n floats for every vector. out may be
 * x; w may be a blob address.
 *
 * k32_quantize_i8: q = round(x * (127 / max |x|)), half away from zero,
 * clamped to -127..127 (0 for NaN), and scale = max |x| / 127, for each
 * vector: q receives count x n values, scale count floats. A vector of zeros
 * gets q 0 and scale 0. Its scale is the x_scale of k32_matvec and
 * k32_matmul, or of a KV cache row.
 *
 * k32_rope_f32: the rotary embedding in place. Vector p turns each pair
 * (x[i], x[i + 1]) by the angle of cs row p, head_size floats of (cos, sin)
 * pairs taken at i % head_size: x[i] = x[i] * c - x[i + 1] * s and
 * x[i + 1] = x[i] * s + x[i + 1] * c. cs may be a blob address. n and
 * head_size must be even (K32_ERR_INVAL otherwise).
 *
 * k32_swiglu_f32: x = (x * (1 / (1 + k32_expf(-x)))) * g over n floats, the
 * gate of a SwiGLU feed-forward block.
 *
 * k32_add_f32: x = x + y over n floats, a residual connection.
 *
 * k32_argmax_f32: the index of the first largest of n floats (0 for n = 0),
 * a greedy pick from logits.
 *
 * k32_attention_f32_i8: the attention of n query positions pos0 .. pos0 +
 * n - 1 over an int8 KV cache in linear memory, every head in one call. q
 * holds n vectors of heads x hs floats and out receives the same. For
 * position p and head h, with KV head kh = h / (heads / kvh): the scores
 * against key rows 0 .. p, row r at kq + r x kv_stride + kh x hs (hs int8
 * values) with the scale ks[r x kvh + kh], as k32_matvec_f32_i8 with alpha
 * = scale into att; k32_softmax_f32 over them; the weighted sum of the
 * value rows (vq, vs, the same layout) as k32_vecmat_f32_i8 into head h of
 * the position's out: the bits of those three calls per head. kv_stride 0
 * packs the rows (kvh x hs bytes). att is scratch of pos0 + n floats.
 * heads must be a multiple of kvh and kv_stride at least kvh x hs
 * (K32_ERR_INVAL otherwise).
 *
 * k32_attention_full_f32_i8: the same with every one of the n positions
 * attending to key rows 0 .. rows - 1, for a model whose positions see the
 * whole window: the bits of the three calls per head over rows rows. att
 * is scratch of n x (heads / kvh) x rows floats.
 *
 * k32_add_f32_i8: x = x + (float)q * scale over n floats, a residual from
 * an int8 map.
 *
 * k32_quantize_i8_static: q = round(x * inv), half away from zero, clamped
 * to -127..127 (0 for NaN), over n floats: quantization at a fixed scale,
 * such as a convolution's input scale.
 *
 * k32_conv3x3_i8: a 3x3 convolution with padding 1 over n int8 maps `in`
 * of h x w pixels of cin channels (channels innermost, pixel after pixel,
 * map after map), read as their 2x nearest-neighbour upsample when up is 1.
 * Output pixel (y, x) takes input pixels (stride * y + ky - 1,
 * stride * x + kx - 1) of the (h << up) x (w << up) map, zero outside it,
 * for ky, kx in 0 .. 2; the output map is ho x wo with each side the
 * input's << up, divided by stride, rounded up. A call computes output rows
 * y0 .. y1 - 1 of every map: out receives n x (y1 - y0) x wo pixels of cout
 * floats, pixel after pixel,
 *   out[o] = (float)(sum over ky, kx, c of w[o][ky][kx][c] * in[y', x'][c]) * (w_scale[o] * in_scale) + bias[o]
 * with w a cout x 9 cin int8 matrix (row o the taps of output channel o,
 * ky, kx, c innermost): the bits of k32_matvec_i8 over the pixel's patch,
 * then one float add of the bias (bias 0: none). w and w_scale may be blob
 * addresses. The weights are read once per call, in blocks that fit the
 * cache, so a pod computes as many rows per call as its memory holds.
 * stride is 1 or 2, up 0 or 1, cin at most 256 and y0 <= y1 <= ho
 * (K32_ERR_INVAL otherwise). */
K32_IMPORT("k32_matvec_i8") int32_t k32_matvec_i8(float* out, const int8_t* w, const float* w_scale, const int8_t* x, float x_scale,
                                                  uint32_t rows, uint32_t cols, uint32_t row_stride);
K32_IMPORT("k32_matvec_i4") int32_t k32_matvec_i4(float* out, const void* w, const float* w_scale, const int8_t* x, float x_scale,
                                                  uint32_t rows, uint32_t cols, uint32_t row_stride);
K32_IMPORT("k32_matmul_i8") int32_t k32_matmul_i8(float* out, const int8_t* w, const float* w_scale, const int8_t* x, const float* x_scale,
                                                  uint32_t rows, uint32_t cols, uint32_t row_stride, uint32_t n);
K32_IMPORT("k32_matmul_i4") int32_t k32_matmul_i4(float* out, const void* w, const float* w_scale, const int8_t* x, const float* x_scale,
                                                  uint32_t rows, uint32_t cols, uint32_t row_stride, uint32_t n);
K32_IMPORT("k32_matvec_f32_i8") int32_t k32_matvec_f32_i8(float* out, const int8_t* w, const float* w_scale, const float* x, float alpha,
                                                          uint32_t rows, uint32_t cols, uint32_t row_stride, uint32_t scale_stride);
K32_IMPORT("k32_vecmat_f32_i8") int32_t k32_vecmat_f32_i8(float* out, const int8_t* w, const float* w_scale, const float* x,
                                                          uint32_t rows, uint32_t cols, uint32_t row_stride, uint32_t scale_stride);
K32_IMPORT("k32_softmax_f32") int32_t k32_softmax_f32(float* x, uint32_t n);
K32_IMPORT("k32_rmsnorm_f32") int32_t k32_rmsnorm_f32(float* out, const float* x, const float* w, uint32_t n, uint32_t count, float eps);
K32_IMPORT("k32_quantize_i8") int32_t k32_quantize_i8(int8_t* q, float* scale, const float* x, uint32_t n, uint32_t count);
K32_IMPORT("k32_rope_f32") int32_t k32_rope_f32(float* x, const float* cs, uint32_t n, uint32_t head_size, uint32_t count);
K32_IMPORT("k32_swiglu_f32") int32_t k32_swiglu_f32(float* x, const float* g, uint32_t n);
K32_IMPORT("k32_add_f32") int32_t k32_add_f32(float* x, const float* y, uint32_t n);
K32_IMPORT("k32_argmax_f32") int32_t k32_argmax_f32(const float* x, uint32_t n);
K32_IMPORT("k32_attention_f32_i8") int32_t k32_attention_f32_i8(float* out, const int8_t* kq, const float* ks, const int8_t* vq, const float* vs,
                                                                const float* q, float scale, uint32_t heads, uint32_t kvh, uint32_t hs,
                                                                uint32_t kv_stride, uint32_t pos0, uint32_t n, float* att);
K32_IMPORT("k32_attention_full_f32_i8") int32_t k32_attention_full_f32_i8(float* out, const int8_t* kq, const float* ks, const int8_t* vq,
                                                                          const float* vs, const float* q, float scale, uint32_t heads,
                                                                          uint32_t kvh, uint32_t hs, uint32_t kv_stride, uint32_t rows,
                                                                          uint32_t n, float* att);
K32_IMPORT("k32_add_f32_i8") int32_t k32_add_f32_i8(float* x, const int8_t* q, float scale, uint32_t n);
K32_IMPORT("k32_quantize_i8_static") int32_t k32_quantize_i8_static(int8_t* q, const float* x, float inv, uint32_t n);
K32_IMPORT("k32_conv3x3_i8") int32_t k32_conv3x3_i8(float* out, const int8_t* w, const float* w_scale, const float* bias, const int8_t* in,
                                                    float in_scale, uint32_t cin, uint32_t cout, uint32_t h, uint32_t w_, uint32_t stride,
                                                    uint32_t up, uint32_t n, uint32_t y0, uint32_t y1);

/* k32_fft_q15: the butterflies of an n-point radix-2 FFT (n a power of two,
 * 2..4096) in place over re[n] and im[n], int32 in bit-reversed order, with
 * n / 2 Q15 twiddles tw_re[k], tw_im[k] (usually cos and -sin of 2 pi k / n).
 * Each butterfly takes t = W x[b] with each part an int64 sum of products
 * rounded as (p + 16384) >> 15, then x[b] = x[a] - t, x[a] = x[a] + t in
 * int32; no scaling between stages. Returns K32_OK, or K32_ERR_INVAL for an
 * n out of range; operands outside the linear memory trap. A metered pod
 * pays for n / 2 x log2 n butterflies, 8 units each. */
K32_IMPORT("k32_fft_q15")    int32_t k32_fft_q15(int32_t* re, int32_t* im, uint32_t n, const int16_t* tw_re, const int16_t* tw_im);

/* ---- Hardware imports, gated by the capability firewall ---------------
 * The pod manifest must request the matching resource:
 *   hardware.kube32.io/tft-display: "1"  -> k32_display_*
 *   hardware.kube32.io/keyboard: "1"     -> k32_key_read
 *   hardware.kube32.io/speaker: "1"      -> k32_tone, k32_audio_play
 *   hardware.kube32.io/microphone: "1"   -> k32_mic_read
 * Calls without the grant return K32_ERR_PERM and are logged as violations.
 *
 * The speaker plays at most K32_SPEAKER_MAX_PCT percent of full scale:
 * volume_pct 100 is that ceiling, never more. On the Cardputer the speaker
 * and the microphone share a clock pin (GPIO 43), so one of them runs at a
 * time and the speaker comes first: k32_mic_read waits for playback to end.
 *
 * k32_audio_play queues count signed 16-bit mono samples at sample_rate
 * (8000 to 48000 Hz), at most one second of them per call, and returns the
 * number queued once they are in the output buffer; the last 100 ms or so
 * play after it returns. A call that follows before they have played
 * continues the sound without a gap. The amplifier is switched off once the
 * sound has played out.
 *
 * k32_mic_read starts the microphone (16 kHz, signed 16-bit mono PCM, raw
 * from the PDM decimator) and copies up to max_samples samples (at most
 * 16000) to out, waiting up to timeout_ms (at most 1000) for them. It
 * returns the number copied, or K32_ERR_BUSY when the speaker still plays
 * after timeout_ms. The microphone keeps recording between calls into a
 * 64 ms buffer and stops one second after the last call; max_samples 0
 * stops it at once. Pods that hold the grant share one sample stream. */
#define K32_SPEAKER_MAX_PCT 25
K32_IMPORT("k32_display_text") int32_t k32_display_text(int32_t x, int32_t y, const char* text, uint32_t len, uint32_t fg565, uint32_t bg565);
K32_IMPORT("k32_display_rect") int32_t k32_display_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color565);
K32_IMPORT("k32_display_bar")  int32_t k32_display_bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg565, uint32_t bg565);
K32_IMPORT("k32_key_read")     int32_t k32_key_read(void);    /* next key code (ASCII), K32_ERR_AGAIN when empty */
K32_IMPORT("k32_tone")         int32_t k32_tone(uint32_t freq_hz, uint32_t duration_ms, uint32_t volume_pct);
K32_IMPORT("k32_audio_play")   int32_t k32_audio_play(const int16_t* samples, uint32_t count, uint32_t sample_rate, uint32_t volume_pct);
K32_IMPORT("k32_mic_read")     int32_t k32_mic_read(int16_t* out, uint32_t max_samples, uint32_t timeout_ms);

/* ---- Pod storage imports, gated by hardware.kube32.io/sd -------------- */
K32_IMPORT("k32_fs_open")   int32_t k32_fs_open(const char* path, uint32_t path_len, uint32_t flags);   /* fd (0..3) or error */
K32_IMPORT("k32_fs_read")   int32_t k32_fs_read(int32_t fd, void* buf, uint32_t len);                   /* bytes read, 0 at end, or error */
K32_IMPORT("k32_fs_write")  int32_t k32_fs_write(int32_t fd, const void* buf, uint32_t len);            /* bytes written or error */
K32_IMPORT("k32_fs_close")  int32_t k32_fs_close(int32_t fd);                                           /* 0 or error */
K32_IMPORT("k32_fs_size")   int32_t k32_fs_size(int32_t fd);                                            /* file size in bytes or error */
K32_IMPORT("k32_fs_list")   int32_t k32_fs_list(const char* dir, uint32_t dir_len, char* out, uint32_t out_cap); /* bytes written: "name\n" per entry, directories end in "/" */
K32_IMPORT("k32_fs_remove") int32_t k32_fs_remove(const char* path, uint32_t path_len);                 /* 0 or error; removes a file or an empty directory */

/* ---- Blobs, granted by the annotation kube32.io/blobs (see above) ------- */
K32_IMPORT("k32_blob_open")   int32_t k32_blob_open(const char* name, uint32_t name_len);                    /* handle (0..3) or error */
K32_IMPORT("k32_blob_size")   int32_t k32_blob_size(int32_t blob);                                          /* bytes or error */
K32_IMPORT("k32_blob_read")   int32_t k32_blob_read(int32_t blob, uint32_t offset, void* buf, uint32_t len); /* bytes copied (fewer only at the end, 0 past it) or error */
K32_IMPORT("k32_blob_mapped") int32_t k32_blob_mapped(int32_t blob);                                        /* 1: the kernels read it through K32_BLOB_ADDR; 0: k32_blob_read only; or error */
K32_IMPORT("k32_blob_close")  int32_t k32_blob_close(int32_t blob);                                         /* 0 or error */

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

/* ---- GPIO, gated by esp32.io/gpio-<N> ----------------------------------- */
K32_IMPORT("esp32_gpio_set_mode") int32_t esp32_gpio_set_mode(uint32_t pin, uint32_t mode);   /* K32_GPIO_*: 0 or error */
K32_IMPORT("esp32_gpio_write")    int32_t esp32_gpio_write(uint32_t pin, uint32_t level);     /* 0 or error */
K32_IMPORT("esp32_gpio_read")     int32_t esp32_gpio_read(uint32_t pin);                      /* level 0 or 1, or error */

/* ---- Chip temperature, gated by hardware.kube32.io/chip-temp ------------
 * The ESP32-S3's internal sensor (the die, not the room: it reads above
 * ambient while the radio is busy). Stores milli-degrees Celsius in
 * *out_milli_c and returns 0, or K32_ERR_PERM, K32_ERR_FAULT, K32_ERR_NODEV,
 * K32_ERR_IO. */
K32_IMPORT("esp32_chip_temp_read") int32_t esp32_chip_temp_read(int32_t* out_milli_c);

/* Existing low-level imports (see host_functions.cpp) remain available:
 * esp32_adc_read, esp32_uart_write, esp32_uart_read, esp32_get_time_us,
 * esp32_delay_ms, kube32_channel_open/send/recv/close. */
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
