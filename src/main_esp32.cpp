// Kube32 firmware entry point for ESP32-S3.
//
// One image boots both cluster roles. A board with PSRAM becomes the control
// plane and any other board a worker; the NVS key "role" (K32_CMD:ROLE:)
// overrides that, and the NVS key "board" (K32_CMD:BOARD:) selects the
// peripheral profile.
//   control plane  API server, scheduler, controllers, object store, ingress
//                  on port 80 and a local kubelet; it stages firmware images
//                  and rolls them out (docs/firmware-updates.md)
//   worker         kubelet; with the "cardputer" profile the display, keypad,
//                  speaker and microSD card are exposed to pods
// A node is named by the NVS key "name" (K32_CMD:NAME:), or "esp32-" and the
// last three bytes of its Wi-Fi MAC address when that is not set.
//
// Nothing application specific lives here: workloads arrive as Wasm pods
// through the Kubernetes API, images come from the embedded registry, and
// hardware reaches pods only through capability-gated host functions.

#if defined(KUBE32_TARGET_ESP32)

#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "controller/deployment_controller.hpp"
#include "controller/workload_controller.hpp"
#include "controller/firmware_rollout_controller.hpp"
#include "api/firmware_api.hpp"
#include "hal/firmware_slots.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/i2c_discovery.hpp"
#include "kubelet/metrics_provider.hpp"
#include "esp_sntp.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "api/http_server.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/app_pod.hpp"
#include "runtime/image_registry.hpp"
#include "embedded/embedded_blobs.hpp"
#include "kubelet/kubelet.hpp"
#include "ipc/channel.hpp"
#include "net/network_manager.hpp"
#include "app/ingress_controller.hpp"
#include "app/pod_server.hpp"
#include "app/service_client.hpp"
#include "app/node_storage_api.hpp"
#include "app/console_relay.hpp"
#include "api/node_proxy.hpp"
#include "kubelet/kube_api_client.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"
#include "osal/socket.hpp"
#include "hal/tft_keyboard_hardware.hpp"
#include "hal/sd_card.hpp"
#include "hal/node_storage.hpp"
#include "storage/flash_log.hpp"
#include "api/timestamp_backfill.hpp"
#include "osal/tls.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/pod_log.hpp"
#include "models/kube_models.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_debug_helpers.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

static const char* TAG = "Kube32";

// Role selection is capability based: the board with external PSRAM hosts the
// control plane, the API server, the ingress and the large data pod; a board
// without PSRAM is a worker. Board peripherals come from a profile stored in
// NVS ("board" = "cardputer" enables the ST7789 display, matrix keyboard and
// I2S speaker). Both can be set over the console:
//   K32_CMD:ROLE:control-plane|worker|auto
//   K32_CMD:BOARD:cardputer|generic

namespace {

struct NodeIdentity {
    bool is_control_plane{true};
    bool has_display{false};
    bool has_psram{false};
    std::string name;
    uint32_t id{1};       // channel node id, from the MAC address
    std::string mac;
};

// RFC 1123 label: what a Node name needs to be usable as kubernetes.io/hostname
bool valid_node_name(const std::string& n) {
    if (n.empty() || n.size() > 63 || n.front() == '-' || n.back() == '-') return false;
    for (char c : n) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    return true;
}

// Bearer tokens: printable, no spaces, 16 to 128 characters
bool valid_token(const std::string& t) {
    if (t.size() < 16 || t.size() > 128) return false;
    for (char c : t) if (c <= ' ' || c > '~') return false;
    return true;
}

std::string read_nvs_str(const char* key);
bool valid_ap_password(const std::string& p);

NodeIdentity detect_identity() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    NodeIdentity id;
    id.mac = mac_str;
    id.has_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    id.has_display = false;
    nvs_handle_t h;
    char role[24] = {0}, board[24] = {0};
    size_t len = sizeof(role), blen = sizeof(board);
    if (nvs_open("k32", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, "role", role, &len) != ESP_OK) role[0] = 0;
        if (nvs_get_str(h, "board", board, &blen) != ESP_OK) board[0] = 0;
        nvs_close(h);
    }
    bool control_plane = id.has_psram;
    if (role[0]) control_plane = (std::strcmp(role, "control-plane") == 0);
    if (board[0]) id.has_display = (std::strcmp(board, "cardputer") == 0 || std::strcmp(board, "display") == 0);
    id.is_control_plane = control_plane;
    char derived[16];
    snprintf(derived, sizeof(derived), "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    std::string stored = read_nvs_str("name");
    id.name = valid_node_name(stored) ? stored : std::string(derived);
    id.id = (static_cast<uint32_t>(mac[3]) << 16) | (static_cast<uint32_t>(mac[4]) << 8) | mac[5];
    if (id.id == 0) id.id = 1;
    return id;
}

// Reads one string from the "k32" NVS namespace; empty when absent.
std::string read_nvs_str(const char* key) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READONLY, &h) != ESP_OK) return {};
    char v[129] = {0};
    size_t vl = sizeof(v);
    bool ok = nvs_get_str(h, key, v, &vl) == ESP_OK;
    nvs_close(h);
    return ok ? std::string(v) : std::string();
}

// WPA2 passphrase of the cluster access point: 8 to 63 printable characters
bool valid_ap_password(const std::string& p) {
    if (p.size() < 8 || p.size() > 63) return false;
    for (char c : p) {
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

// The cluster access point's password (NVS "appass", K32_CMD:APPASS:). A
// control plane without one makes one up on its first boot and keeps it;
// runs before Wi-Fi and the ADC are in use, while the SAR ADC can serve as
// the entropy source.
std::string cluster_ap_password(bool control_plane) {
    std::string pass = read_nvs_str("appass");
    if (valid_ap_password(pass) || !control_plane) return valid_ap_password(pass) ? pass : std::string();
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    bootloader_random_enable();
    pass.clear();
    for (int i = 0; i < 16; ++i) pass.push_back(alphabet[esp_random() % (sizeof(alphabet) - 1)]);
    bootloader_random_disable();
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "appass", pass.c_str());
        nvs_commit(h);
        nvs_close(h);
    }
    printf("[NET] generated a password for the cluster access point: K32_CMD:APPASS prints it\n");
    return pass;
}

bool read_nvs_blob(const char* key, std::string& out) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    bool ok = nvs_get_blob(h, key, nullptr, &len) == ESP_OK && len > 0 && len < 4096;
    if (ok) {
        out.assign(len, '\0');
        ok = nvs_get_blob(h, key, &out[0], &len) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

void write_nvs_blob(const char* key, const std::string& value) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, value.data(), value.size());
    nvs_commit(h);
    nvs_close(h);
}

bool read_nvs_wifi(std::string& ssid, std::string& pass) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READONLY, &h) != ESP_OK) return false;
    char s[33] = {0}, p[65] = {0};
    size_t sl = sizeof(s), pl = sizeof(p);
    bool ok = (nvs_get_str(h, "wifi_ssid", s, &sl) == ESP_OK) && (nvs_get_str(h, "wifi_pass", p, &pl) == ESP_OK);
    nvs_close(h);
    if (!ok || !s[0]) return false;
    ssid = s;
    pass = p;
    return true;
}

// ---------------------------------------------------------------------------
// Console transport. Generic boards listen on both the native USB-Serial-JTAG
// port and UART0 (CH340-style bridges) and answer on whichever carried the
// command. Boards whose profile uses the UART0 pins for peripherals (the
// display board drives I2S on GPIO 43) use USB only.
// ---------------------------------------------------------------------------
struct Console {
    bool uart_enabled{false};
    bool last_from_usb{true};
    void init(bool usb_only) {
        if (!usb_serial_jtag_is_driver_installed()) {
            // Console lines are short and writes wait for room: small rings
            // on a board without PSRAM, whose internal RAM pods need
            const bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
            usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = psram ? 4096u : 2048u, .rx_buffer_size = psram ? 4096u : 1024u };
            usb_serial_jtag_driver_install(&cfg);
        }
        // Once the driver owns the USB-JTAG FIFO, printf/ESP_LOG must go through
        // it too. Left on the default path they write the FIFO directly, race
        // the driver's interrupt handler, and the port stops answering until
        // the board is power cycled.
        usb_serial_jtag_vfs_use_driver();
        uart_enabled = !usb_only;
        if (uart_enabled) {
            if (!uart_is_driver_installed(UART_NUM_0)) {
                uart_driver_install(UART_NUM_0, 8192, 8192, 0, nullptr, 0);
            }
            // Route printf/ESP_LOG through the driver so a log line and a protocol
            // frame can never interleave byte-wise.
            uart_vfs_dev_use_driver(UART_NUM_0);
        }
    }
    // Waits on the port that carried the last bytes and only polls the other.
    // The USB-Serial/JTAG driver drops what arrives while its receive ring
    // (4 KiB) is full, so a wait on UART0 while a request streams in over USB
    // would lose part of it; UART0's own 8 KiB ring holds a line meanwhile.
    int read(char* buf, size_t cap, uint32_t timeout_ms) {
        if (!uart_enabled) return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(timeout_ms));
        const TickType_t wait = pdMS_TO_TICKS(timeout_ms);
        int n = usb_serial_jtag_read_bytes(buf, cap, last_from_usb ? wait : 0);
        if (n > 0) { last_from_usb = true; return n; }
        n = uart_read_bytes(UART_NUM_0, buf, cap, last_from_usb ? 0 : wait);
        if (n > 0) { last_from_usb = false; return n; }
        if (!last_from_usb) {
            n = usb_serial_jtag_read_bytes(buf, cap, 0);
            if (n > 0) last_from_usb = true;
        }
        return n;
    }
    void write(const char* data, size_t len) {
        if (last_from_usb || !uart_enabled) {
            // The driver takes a write whole or not at all, and never one
            // larger than its TX ring
            size_t off = 0;
            while (off < len) {
                size_t part = len - off < 1024 ? len - off : 1024;
                int n = usb_serial_jtag_write_bytes(data + off, part, pdMS_TO_TICKS(200));
                if (n <= 0) break;
                off += static_cast<size_t>(n);
            }
        } else {
            uart_write_bytes(UART_NUM_0, data, len);
        }
    }
    // One frame in one driver write: log output can surround it but never
    // split it. The frame must fit the TX ring (4096 or 2048 bytes on USB).
    bool write_frame(const char* data, size_t len, uint32_t timeout_ms) {
        if (last_from_usb || !uart_enabled) {
            return usb_serial_jtag_write_bytes(data, len, pdMS_TO_TICKS(timeout_ms)) == static_cast<int>(len);
        }
        return uart_write_bytes(UART_NUM_0, data, len) == static_cast<int>(len);
    }
};

// Forwards one raw HTTP request to a server over loopback, or over the cluster
// network when this node does not host the port (workers forward to the control
// plane). The request must carry "Connection: close".
static std::string s_forward_host = "127.0.0.1";
static kube32::api::HttpServer* s_api_server = nullptr;       // for console toggles
static kube32::storage::Store* s_store = nullptr;             // control plane: journal figures for the console
static std::string s_journal_location;                         // control plane: where the store's journal is
static bool s_api_token_set = false;
static kube32::osal::TlsServer* s_tls = nullptr;               // control plane: the API server's certificate
static kube32::api::HttpServer* s_ingress_server = nullptr;
static std::unique_ptr<kube32::app::ConsoleRelay> s_relay;     // streams for tools/kube32_serial_gateway.py
static std::shared_ptr<kube32::hal::FirmwareSlots> s_firmware_slots;   // the app slots (firmware rollouts)
static kube32::api::FirmwareApi* s_firmware_api = nullptr;     // control plane: the staged image
std::string loopback_http(uint16_t port, const std::string& request) {
    std::string response;
    kube32::osal::Socket sock;
    if (!sock.create(kube32::osal::SocketType::Tcp) || !sock.connect(s_forward_host.c_str(), port, 2000)) {
        return "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    sock.set_timeout(8000, 5000);
    if (!sock.send_all(request.data(), request.size())) {
        return "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    char buf[1024];
    while (true) {
        ssize_t n = sock.recv(buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
        if (response.size() > 256 * 1024) break;
    }
    return response;
}

// One "K32_SD <path> <size|dir>" line per entry under `path`, two levels deep
// (images/ and volumes/<claim>/), at most 64 entries.
static void console_list_sd(Console& con, const std::string& path, int depth, int& budget) {
    DIR* d = opendir(path.c_str());
    if (!d) return;
    while (budget > 0) {
        struct dirent* e = readdir(d);
        if (!e) break;
        if (e->d_name[0] == '.') continue;
        std::string child = path + "/" + e->d_name;
        struct stat st;
        bool is_dir = (stat(child.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
        char l[160];
        int ll = is_dir ? snprintf(l, sizeof(l), "K32_SD %s/ dir\n", child.c_str())
                        : snprintf(l, sizeof(l), "K32_SD %s %lld\n", child.c_str(), (long long)st.st_size);
        con.write(l, static_cast<size_t>(ll));
        budget--;
        if (is_dir && depth > 1) console_list_sd(con, child, depth - 1, budget);
    }
    closedir(d);
}

// "K32_HEAP used|free <bucket> <blocks> <bytes>" lines: internal heap blocks
// grouped by size. The walk holds the heap lock, so the counts live in statics.
struct HeapBuckets { uint32_t count[2][7]; uint32_t bytes[2][7]; };
static HeapBuckets s_heap_buckets;
static const size_t k_heap_bucket_max[] = {32, 128, 512, 1024, 1700, 4096, SIZE_MAX};

static void console_heap_histogram(Console& con) {
    std::memset(&s_heap_buckets, 0, sizeof(s_heap_buckets));
    heap_caps_walk(MALLOC_CAP_INTERNAL, [](walker_heap_into_t, walker_block_info_t b, void*) {
        size_t i = 0;
        while (b.size > k_heap_bucket_max[i]) ++i;
        s_heap_buckets.count[b.used ? 0 : 1][i]++;
        s_heap_buckets.bytes[b.used ? 0 : 1][i] += static_cast<uint32_t>(b.size);
        return true;
    }, nullptr);
    for (int u = 0; u < 2; ++u) {
        for (size_t i = 0; i < 7; ++i) {
            char l[80];
            int ll = snprintf(l, sizeof(l), "K32_HEAP %s <=%u %u %u\n", u == 0 ? "used" : "free",
                              i == 6 ? 99999u : (unsigned)k_heap_bucket_max[i],
                              (unsigned)s_heap_buckets.count[u][i], (unsigned)s_heap_buckets.bytes[u][i]);
            con.write(l, static_cast<size_t>(ll));
        }
    }
}

// lwIP's TCP connections, copied in the TCP/IP thread: state, ports, the
// bytes queued to send or waiting for an ACK, and data the application has
// not taken yet.
struct TcpPcbRow { uint8_t state; uint8_t peer; uint16_t local_port, remote_port, queuelen; uint32_t unsent, unacked, refused, ooseq; uint16_t nrtx; };
static TcpPcbRow s_tcp_rows[24];
static size_t s_tcp_count;

static void console_tcp_pcbs(Console& con) {
    s_tcp_count = 0;
    struct tcpip_api_call_data call = {};
    tcpip_api_call([](struct tcpip_api_call_data*) -> err_t {
        auto seg_bytes = [](const struct tcp_seg* seg) {
            uint32_t n = 0;
            for (; seg; seg = seg->next) n += seg->p ? seg->p->tot_len : 0;
            return n;
        };
        for (struct tcp_pcb* const* list : {&tcp_active_pcbs, &tcp_tw_pcbs}) {
            for (const struct tcp_pcb* pcb = *list; pcb && s_tcp_count < 24; pcb = pcb->next) {
                TcpPcbRow& r = s_tcp_rows[s_tcp_count++];
                r.state = static_cast<uint8_t>(pcb->state);
                r.peer = static_cast<uint8_t>(ntohl(ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))) & 0xff);
                r.local_port = pcb->local_port;
                r.remote_port = pcb->remote_port;
                r.queuelen = pcb->snd_queuelen;
                r.unsent = seg_bytes(pcb->unsent);
                r.unacked = seg_bytes(pcb->unacked);
                r.refused = pcb->refused_data ? pcb->refused_data->tot_len : 0;
                r.ooseq = seg_bytes(pcb->ooseq);
                r.nrtx = pcb->nrtx;
            }
        }
        return ERR_OK;
    }, &call);
    static const char* const states[] = {"CLOSED", "LISTEN", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                         "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"};
    for (size_t i = 0; i < s_tcp_count; ++i) {
        const TcpPcbRow& r = s_tcp_rows[i];
        char l[160];
        int ll = snprintf(l, sizeof(l), "K32_TCP %-11s local=%u peer=.%u:%u queuelen=%u unsent=%u unacked=%u refused=%u ooseq=%u nrtx=%u\n",
                          r.state <= 10 ? states[r.state] : "?", r.local_port, r.peer, r.remote_port, r.queuelen,
                          (unsigned)r.unsent, (unsigned)r.unacked, (unsigned)r.refused, (unsigned)r.ooseq, r.nrtx);
        con.write(l, static_cast<size_t>(ll));
    }
}

static uint32_t crc32_bytes(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Console protocol (line oriented, binary-safe payloads):
//   K32_RESET, K32_OPEN, K32_CLOSE: concurrent streamed HTTP (include/app/console_relay.hpp)
//   K32_HTTP <port> <len> [crc32hex]\n<len bytes of HTTP request> -> K32_RESP <len>\n<bytes>
//   A CRC mismatch answers K32_ERR crc so the gateway can resend.
//   K32_CMD:STATUS                                       -> telemetry line
//   K32_CMD:STORE                                        -> object store journal figures
//   K32_CMD:WIFI:<ssid>:<password>                       -> stored in NVS, reboot
//   K32_CMD:NAME:<node name>|auto                        -> stored in NVS, reboot
//   K32_CMD:TOKEN:<API bearer token>|off                 -> stored in NVS, reboot
//   K32_CMD:APPASS:<cluster access point password>       -> stored in NVS, reboot
//   K32_CMD:APPASS                                       -> K32_APPASS <password>
//   K32_CMD:CERT                                         -> the API server's TLS certificate (PEM)
//   K32_CMD:FIRMWARE                                     -> the running image and its slot (and what is staged)
//   K32_CMD:REBOOT
void console_task(void* arg) {
    auto* ctx = static_cast<std::pair<Console*, NodeIdentity*>*>(arg);
    Console& con = *ctx->first;
    NodeIdentity& ident = *ctx->second;
    std::string line;
    char rx[256];
    // The `len` bytes after a frame header: what rx holds past index i, then
    // further reads of exactly the bytes still missing. Leaves i on the last
    // byte taken from rx, so a frame that follows in the same read is kept.
    auto read_payload = [&](int& i, int n, size_t len) {
        std::string data;
        data.reserve(len);
        int j = i + 1;
        while (j < n && data.size() < len) data.push_back(rx[j++]);
        i = j - 1;
        uint32_t waited = 0;
        while (data.size() < len && waited < 5000) {
            size_t want = len - data.size();
            int m = con.read(rx, want < sizeof(rx) ? want : sizeof(rx), 100);
            if (m > 0) data.append(rx, static_cast<size_t>(m)); else waited += 100;
        }
        return data;
    };
    while (true) {
        int n = con.read(rx, sizeof(rx), 50);
        if (n <= 0) continue;
        for (int i = 0; i < n; ++i) {
            char c = rx[i];
            if (c != '\n' && c != '\r') {
                if (line.size() < 512) line.push_back(c);
                continue;
            }
            if (line.empty()) continue;
            if (line == "K32_RESET") {
                s_relay->reset();
            } else if (line.rfind("K32_OPEN ", 0) == 0) {
                unsigned id = 0, port = 0, len = 0, crc = 0;
                if (sscanf(line.c_str() + 9, "%u %u %u %x", &id, &port, &len, &crc) == 4 && len > 0 && len < 64 * 1024) {
                    std::string req = read_payload(i, n, len);
                    s_relay->open(id, static_cast<uint16_t>(port), std::move(req), crc);
                }
            } else if (line.rfind("K32_CLOSE ", 0) == 0) {
                s_relay->close(static_cast<uint32_t>(strtoul(line.c_str() + 10, nullptr, 10)));
            } else if (line.rfind("K32_HTTP ", 0) == 0) {
                unsigned port = 0, len = 0, crc = 0;
                int fields = sscanf(line.c_str() + 9, "%u %u %x", &port, &len, &crc);
                if (fields >= 2 && len > 0 && len < 64 * 1024) {
                    std::string req = read_payload(i, n, len);
                    if (fields == 3 && crc32_bytes(reinterpret_cast<const uint8_t*>(req.data()), req.size()) != crc) {
                        con.write("K32_ERR crc\n", 12);
                        line.clear();
                        continue;
                    }
                    std::string resp = loopback_http(static_cast<uint16_t>(port), req);
                    char hdr[40];
                    int hl = snprintf(hdr, sizeof(hdr), "K32_RESP %u\n", static_cast<unsigned>(resp.size()));
                    // One write per frame: the UART driver keeps it contiguous on the wire
                    std::string frame;
                    frame.reserve(static_cast<size_t>(hl) + resp.size());
                    frame.append(hdr, static_cast<size_t>(hl));
                    frame.append(resp);
                    con.write(frame.data(), frame.size());
                }
            } else if (line == "K32_CMD:STATUS") {
                char buf[384];
                int l = snprintf(buf, sizeof(buf),
                    "K32_STATUS node=%s role=%s mac=%s free_heap=%u min_free_heap=%u largest_block=%u pod_pool_free=%u free_psram=%u uptime_ms=%llu pods=%u storage=%s auth=%s tls=%s\n",
                    ident.name.c_str(), ident.is_control_plane ? "control-plane" : "worker", ident.mac.c_str(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)kube32_pool_available(nullptr),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned long long)kube32::osal::System::get_uptime_ms(),
                    (unsigned)kube32::runtime::AppPodHost::instance().list_pods().size(),
                    kube32::hal::storage_medium_name(kube32::hal::node_storage_info().medium), s_api_token_set ? "token" : "off",
                    s_tls ? "on" : "off");
                con.write(buf, static_cast<size_t>(l));
            } else if (line == "K32_CMD:STORE") {
                kube32::storage::PersistenceStats ps;
                char buf[320];
                int l;
                if (s_store && s_store->persistence_stats(ps)) {
                    l = snprintf(buf, sizeof(buf),
                        "K32_STORE journal=%s keys=%u live_bytes=%u capacity_bytes=%u sectors=%u/%u pending=%u records=%llu bytes=%llu moved=%llu unchanged=%llu errors=%llu revision=%llu\n",
                        s_journal_location.c_str(), (unsigned)ps.live_keys, (unsigned)ps.live_bytes,
                        (unsigned)ps.capacity_bytes, (unsigned)ps.used_sectors, (unsigned)ps.sector_count, (unsigned)ps.pending_keys,
                        (unsigned long long)ps.records_written, (unsigned long long)ps.bytes_written, (unsigned long long)ps.records_moved,
                        (unsigned long long)ps.unchanged_skipped, (unsigned long long)ps.write_errors,
                        (unsigned long long)s_store->current_version());
                } else {
                    l = snprintf(buf, sizeof(buf), "K32_STORE journal=none\n");
                }
                con.write(buf, static_cast<size_t>(l));
            } else if (line == "K32_CMD:CERT") {
                if (s_tls) {
                    const std::string& pem = s_tls->certificate_pem();
                    size_t start = 0;
                    while (start < pem.size()) {
                        size_t end = pem.find('\n', start);
                        if (end == std::string::npos) end = pem.size();
                        std::string l = "K32_CERT " + pem.substr(start, end - start) + "\n";
                        con.write(l.data(), l.size());
                        start = end + 1;
                    }
                    con.write("K32_OK cert\n", 12);
                } else {
                    con.write("K32_ERR no TLS on this node\n", 28);
                }
            } else if (line == "K32_CMD:FIRMWARE") {
                char buf[320];
                int l;
                if (s_firmware_slots) {
                    const kube32::hal::FirmwareInfo fw = s_firmware_slots->running();
                    const kube32::api::StagedFirmware st = s_firmware_api ? s_firmware_api->staged() : kube32::api::StagedFirmware{};
                    l = snprintf(buf, sizeof(buf), "K32_FIRMWARE version=%s sha256=%s slot=%s state=%s update_slot=%s staged=%s staged_sha256=%s staged_version=%s\n",
                                 fw.version.c_str(), fw.sha256.c_str(), fw.slot.c_str(), fw.pending_verify ? "Verifying" : "Confirmed",
                                 s_firmware_slots->passive_slot().c_str(), s_firmware_api ? st.phase.c_str() : "-",
                                 st.sha256.empty() ? "-" : st.sha256.c_str(), st.version.empty() ? "-" : st.version.c_str());
                } else {
                    l = snprintf(buf, sizeof(buf), "K32_ERR no firmware slots\n");
                }
                con.write(buf, static_cast<size_t>(l));
            } else if (line == "K32_CMD:APPASS") {
                const std::string pass = read_nvs_str("appass");
                const std::string out = pass.empty() ? "K32_ERR no cluster access point password stored\n" : "K32_APPASS " + pass + "\n";
                con.write(out.data(), out.size());
            } else if (line.rfind("K32_CMD:APPASS:", 0) == 0) {
                // The same password on every node; empty erases it (a control
                // plane makes up a new one at boot)
                const std::string value = line.substr(15);
                if (!value.empty() && !valid_ap_password(value)) {
                    const char* why = "K32_ERR password must be 8 to 63 printable characters\n";
                    con.write(why, std::strlen(why));
                } else {
                    nvs_handle_t h;
                    if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                        if (value.empty()) nvs_erase_key(h, "appass"); else nvs_set_str(h, "appass", value.c_str());
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    con.write("K32_OK password stored, rebooting\n", 34);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            } else if (line.rfind("K32_CMD:NAME:", 0) == 0 || line.rfind("K32_CMD:TOKEN:", 0) == 0) {
                // Node name (a DNS-1123 label, "auto" for the MAC-derived one)
                // and API bearer token ("off" disables authentication)
                const bool is_name = line.rfind("K32_CMD:NAME:", 0) == 0;
                std::string value = line.substr(is_name ? 13 : 14);
                const bool clear = is_name ? (value == "auto") : (value == "off");
                if (!clear && !(is_name ? valid_node_name(value) : valid_token(value))) {
                    const char* why = is_name ? "K32_ERR name must be a DNS-1123 label (a-z, 0-9, '-', at most 63)\n"
                                              : "K32_ERR token must be 16 to 128 printable characters without spaces\n";
                    con.write(why, std::strlen(why));
                } else {
                    nvs_handle_t h;
                    if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                        const char* key = is_name ? "name" : "token";
                        if (clear) nvs_erase_key(h, key); else nvs_set_str(h, key, value.c_str());
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    const char* ok = is_name ? "K32_OK name stored, rebooting\n" : "K32_OK token stored, rebooting\n";
                    con.write(ok, std::strlen(ok));
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            } else if (line.rfind("K32_CMD:WIFI:", 0) == 0) {
                std::string rest = line.substr(13);
                size_t sep = rest.find(':');
                std::string ssid = (sep == std::string::npos) ? rest : rest.substr(0, sep);
                std::string pass = (sep == std::string::npos) ? "" : rest.substr(sep + 1);
                nvs_handle_t h;
                if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                    if (ssid.empty()) { nvs_erase_key(h, "wifi_ssid"); nvs_erase_key(h, "wifi_pass"); }
                    else { nvs_set_str(h, "wifi_ssid", ssid.c_str()); nvs_set_str(h, "wifi_pass", pass.c_str()); }
                    nvs_commit(h);
                    nvs_close(h);
                }
                con.write("K32_OK wifi stored, rebooting\n", 30);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (line.rfind("K32_CMD:BOARD:", 0) == 0 || line.rfind("K32_CMD:ROLE:", 0) == 0 || line.rfind("K32_CMD:AP:", 0) == 0 || line.rfind("K32_CMD:API:", 0) == 0) {
                // Profile keys: board=<cardputer|generic>, role=<auto|worker>, ap=<on|off>, api=<control plane host|auto>
                bool is_board = line.rfind("K32_CMD:BOARD:", 0) == 0;
                bool is_ap = line.rfind("K32_CMD:AP:", 0) == 0;
                bool is_api = line.rfind("K32_CMD:API:", 0) == 0;
                std::string value = line.substr(is_board ? 14 : (is_ap ? 11 : (is_api ? 12 : 13)));
                nvs_handle_t h;
                if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                    const char* key = is_board ? "board" : (is_ap ? "ap" : (is_api ? "api" : "role"));
                    if (value.empty() || value == "auto" || value == "generic" || value == "on") nvs_erase_key(h, key);
                    else nvs_set_str(h, key, value.c_str());
                    nvs_commit(h);
                    nvs_close(h);
                }
                con.write("K32_OK profile stored, rebooting\n", 33);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (line.rfind("K32_CMD:TASKBT:", 0) == 0) {
                // Saved context of a blocked task: the first words of its stack
                // frame, so a host can addr2line the return addresses and see
                // where it waits. K32_CMD:TASKBT:<task name prefix>
                std::string want = line.substr(15);
                UBaseType_t n = uxTaskGetNumberOfTasks();
                auto* st = static_cast<TaskStatus_t*>(heap_caps_malloc((n + 4) * sizeof(TaskStatus_t), MALLOC_CAP_8BIT));
                bool found = false;
                if (st) {
                    UBaseType_t got = uxTaskGetSystemState(st, n + 4, nullptr);
                    // An exact name wins over a prefix ("k32_kubelet" vs "k32_kubelet_hb")
                    UBaseType_t pick = got;
                    for (UBaseType_t i = 0; i < got; ++i) {
                        if (std::strncmp(st[i].pcTaskName, want.c_str(), want.size()) != 0) continue;
                        if (pick == got || st[i].pcTaskName[want.size()] == '\0') pick = i;
                    }
                    for (UBaseType_t i = pick; i < got && !found; ++i) {
                        found = true;
                        // pxTopOfStack is the first member of the TCB
                        const uint32_t* sp = *reinterpret_cast<uint32_t* const*>(st[i].xHandle);
                        char l[160];
                        int ll = snprintf(l, sizeof(l), "K32_BT %s state=%d sp=%p\n", st[i].pcTaskName, (int)st[i].eCurrentState, (const void*)sp);
                        con.write(l, static_cast<size_t>(ll));
                        for (int w = 0; w < 48; w += 8) {
                            ll = snprintf(l, sizeof(l), "K32_BT %08x %08x %08x %08x %08x %08x %08x %08x\n",
                                          (unsigned)sp[w], (unsigned)sp[w+1], (unsigned)sp[w+2], (unsigned)sp[w+3],
                                          (unsigned)sp[w+4], (unsigned)sp[w+5], (unsigned)sp[w+6], (unsigned)sp[w+7]);
                            con.write(l, static_cast<size_t>(ll));
                        }
                    }
                    heap_caps_free(st);
                }
                con.write(found ? "K32_OK bt\n" : "K32_ERR no such task\n", found ? 10 : 21);
            } else if (line.rfind("K32_CMD:TONE:", 0) == 0) {
                // Plays N tones back to back from the console task to exercise the
                // speaker driver without a pod: K32_CMD:TONE:<count>
                int n = std::atoi(line.c_str() + 13);
                if (n < 1) n = 1;
                if (n > 200) n = 200;
                uint64_t t0 = kube32::osal::System::get_uptime_ms();
                for (int i = 0; i < n; ++i) kube32::hal::play_tone(880 + (i % 4) * 110, 20, 15);
                char l[64];
                int ll = snprintf(l, sizeof(l), "K32_OK %d tone(s) in %u ms\n", n, (unsigned)(kube32::osal::System::get_uptime_ms() - t0));
                con.write(l, static_cast<size_t>(ll));
            } else if (line.rfind("K32_CMD:ACCESSLOG:", 0) == 0) {
                bool on = line.substr(18) == "on";
                if (s_api_server) s_api_server->set_access_log(on);
                if (s_ingress_server) s_ingress_server->set_access_log(on);
                con.write(on ? "K32_OK access log on\n" : "K32_OK access log off\n", on ? 21 : 22);
            } else if (line == "K32_CMD:BOOTLOADER") {
                // Reboot into the ROM serial bootloader so a host can flash with
                // "--before no-reset" when the USB auto-reset does not work.
                con.write("K32_OK entering download mode\n", 30);
                vTaskDelay(pdMS_TO_TICKS(100));
                REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
                esp_restart();
            } else if (line == "K32_CMD:SD") {
                // Card state, measured figures and what is on it under /sd/kube32
                kube32::hal::SdInfo si = kube32::hal::sd_info();
                char l[256];
                int ll;
                if (si.mounted) {
                    ll = snprintf(l, sizeof(l), "K32_SD mounted=1 name=%s type=%s total_bytes=%llu free_bytes=%llu spi_khz=%u path=/sd\n",
                                  si.name.c_str(), si.type.c_str(), (unsigned long long)si.total_bytes, (unsigned long long)si.free_bytes, (unsigned)si.bus_khz);
                    con.write(l, static_cast<size_t>(ll));
                    int budget = 64;
                    console_list_sd(con, "/sd/kube32", 3, budget);
                } else {
                    ll = snprintf(l, sizeof(l), "K32_SD mounted=0 error=\"%s\"\n", si.error.c_str());
                    con.write(l, static_cast<size_t>(ll));
                }
                con.write("K32_OK sd\n", 10);
            } else if (line == "K32_CMD:KEYPOP") {
                // Pops one key from the matrix queue on the host side (bypasses pods)
                int k = kube32::hal::keyboard_pop();
                char l[48];
                int ll = snprintf(l, sizeof(l), "K32_OK keypop=%d\n", k);
                con.write(l, static_cast<size_t>(ll));
            } else if (line == "K32_CMD:HEAP") {
                // Internal heap blocks by size, used and free: what holds the
                // memory on a board without PSRAM
                console_heap_histogram(con);
                con.write("K32_OK heap\n", 12);
            } else if (line == "K32_CMD:POOL") {
                // The pod pool's small heap, chunk by chunk: where pods' memory lies
                kube32_pool_chunks([](size_t off, size_t size, int used, void* ctx) {
                    char l[64];
                    int ll = snprintf(l, sizeof(l), "K32_POOL %6u %6u %s\n", (unsigned)off, (unsigned)size, used ? "used" : "free");
                    static_cast<Console*>(ctx)->write(l, static_cast<size_t>(ll));
                }, &con);
                con.write("K32_OK pool\n", 12);
            } else if (line == "K32_CMD:TCP") {
                // lwIP's TCP connections and the bytes each one holds
                console_tcp_pcbs(con);
                con.write("K32_OK tcp\n", 11);
            } else if (line == "K32_CMD:BTALL") {
                // Backtrace of every task, for a host to addr2line: where each one waits
                esp_backtrace_print_all_tasks(24);
                con.write("K32_OK btall\n", 13);
            } else if (line == "K32_CMD:TASKS") {
                // FreeRTOS task table: state, priority, core, stack high-water mark,
                // cumulative run time (run-time statistics are enabled in sdkconfig).
                UBaseType_t n = uxTaskGetNumberOfTasks();
                auto* st = static_cast<TaskStatus_t*>(heap_caps_malloc((n + 4) * sizeof(TaskStatus_t), MALLOC_CAP_8BIT));
                if (st) {
                    configRUN_TIME_COUNTER_TYPE total = 0;
                    UBaseType_t got = uxTaskGetSystemState(st, n + 4, &total);
                    static const char* const states[] = {"running", "ready", "blocked", "suspended", "deleted", "invalid"};
                    for (UBaseType_t i = 0; i < got; ++i) {
                        char l[128];
                        int ll = snprintf(l, sizeof(l), "K32_TASK %-18s %-9s prio=%2u core=%d stack_free=%6u run_ms=%llu\n",
                                          st[i].pcTaskName, states[st[i].eCurrentState <= 5 ? st[i].eCurrentState : 5],
                                          (unsigned)st[i].uxCurrentPriority, (int)xTaskGetCoreID(st[i].xHandle),
                                          (unsigned)st[i].usStackHighWaterMark, (unsigned long long)(st[i].ulRunTimeCounter / 1000));
                        con.write(l, static_cast<size_t>(ll));
                    }
                    heap_caps_free(st);
                }
                con.write("K32_OK tasks\n", 13);
            } else if (line.rfind("K32_CMD:KEY:", 0) == 0) {
                // Inject keystrokes into the matrix keyboard queue ("\\n" = Enter,
                // "\\b" = Backspace) so the input path can be exercised from a host.
                std::string text = line.substr(12);
                int n = 0;
                for (size_t i = 0; i < text.size(); ++i) {
                    char c = text[i];
                    if (c == '\\' && i + 1 < text.size()) {
                        char e = text[++i];
                        c = (e == 'n') ? '\n' : (e == 'b') ? '\b' : (e == 't') ? '\t' : e;
                    }
                    kube32::hal::keyboard_inject(c);
                    n++;
                }
                char l[48];
                int ll = snprintf(l, sizeof(l), "K32_OK %d key(s) queued\n", n);
                con.write(l, static_cast<size_t>(ll));
            } else if (line == "K32_CMD:SCAN") {
                // 2.4 GHz networks as seen by this radio (the only band an ESP32 has)
                wifi_scan_config_t sc = {};
                sc.show_hidden = false;
                if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
                    uint16_t n = 0;
                    esp_wifi_scan_get_ap_num(&n);
                    if (n > 20) n = 20;
                    wifi_ap_record_t* recs = static_cast<wifi_ap_record_t*>(calloc(n ? n : 1, sizeof(wifi_ap_record_t)));
                    if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                        for (uint16_t i = 0; i < n; ++i) {
                            char l[96];
                            int ll = snprintf(l, sizeof(l), "K32_AP ssid=\"%s\" rssi=%d ch=%u auth=%d\n", (const char*)recs[i].ssid, recs[i].rssi, recs[i].primary, (int)recs[i].authmode);
                            con.write(l, static_cast<size_t>(ll));
                        }
                    }
                    free(recs);
                    con.write("K32_OK scan done\n", 17);
                } else {
                    con.write("K32_ERR scan failed\n", 20);
                }
            } else if (line == "K32_CMD:REBOOT") {
                con.write("K32_OK rebooting\n", 17);
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            line.clear();
        }
    }
}

// ---------------------------------------------------------------------------
// Hardware hooks for pods on the display node
// ---------------------------------------------------------------------------
void install_hardware_hooks(bool has_display) {
    kube32::runtime::AppPlatformHooks hooks;
    hooks.random = []() -> uint32_t { return esp_random(); };
    if (has_display) {
        hooks.display_text = [](int32_t x, int32_t y, const char* text, size_t len, uint32_t fg, uint32_t bg) -> int32_t {
            kube32::hal::display_draw_string_n(x, y, text, len, static_cast<uint16_t>(fg), static_cast<uint16_t>(bg));
            return 0;
        };
        hooks.display_rect = [](int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) -> int32_t {
            kube32::hal::display_fill_rect(x, y, w, h, static_cast<uint16_t>(color));
            return 0;
        };
        hooks.display_bar = [](int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg, uint32_t bg) -> int32_t {
            kube32::hal::display_draw_bar(x, y, w, h, pct, static_cast<uint16_t>(fg), static_cast<uint16_t>(bg));
            return 0;
        };
        hooks.key_read = []() -> int32_t {
            int k = kube32::hal::keyboard_pop();
            return (k < 0) ? -11 : k;
        };
        hooks.tone = [](uint32_t f, uint32_t ms, uint32_t vol) -> int32_t {
            kube32::hal::play_tone(f, ms, static_cast<uint8_t>(vol));
            return 0;
        };
    }
    kube32::runtime::set_app_platform_hooks(hooks);
}

void print_banner(const NodeIdentity& id) {
    printf("\n================================================================\n");
    printf("  KUBE32  Kubernetes control plane + Wasm runtime on ESP32-S3\n");
    printf("================================================================\n");
    printf("[BOOT] MAC %s -> %s (%s%s%s)\n", id.mac.c_str(), id.name.c_str(), id.is_control_plane ? "control-plane" : "worker",
           id.has_psram ? ", psram" : "", id.has_display ? ", display board" : "");
    printf("[BOOT] Free internal SRAM: %u bytes | PSRAM: %u bytes\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Size of the most recent allocation the heap refused, and how many were
// refused since the last [MEM] line (Wi-Fi's own retries included)
std::atomic<uint32_t> s_alloc_fail_bytes{0};
std::atomic<uint32_t> s_alloc_fail_count{0};

void on_alloc_failed(size_t size, uint32_t caps, const char* function_name) {
    (void)caps;
    (void)function_name;
    s_alloc_fail_bytes.store(static_cast<uint32_t>(size));
    s_alloc_fail_count.fetch_add(1);
}

void log_memory(const char* stage) {
    char failed[64] = "";
    if (const uint32_t n = s_alloc_fail_count.exchange(0)) {
        snprintf(failed, sizeof(failed), "  %u failed alloc(s), last %u bytes", (unsigned)n, (unsigned)s_alloc_fail_bytes.load());
    }
    printf("[MEM] %-22s internal free %6u (min %6u, largest %6u)  psram free %7u%s\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), failed);
}

void cluster_task(void* pv) {
    (void)pv;
    static NodeIdentity ident = detect_identity();
    static Console console;
    heap_caps_register_failed_alloc_callback(on_alloc_failed);
    print_banner(ident);
    const std::string ap_password = cluster_ap_password(ident.is_control_plane);

    // Without PSRAM, reserve one Wasm page (+ interpreter headroom) now, before
    // Wi-Fi and lwIP fragment the heap, so a 64 KiB pod can still be placed.
    if (!ident.has_psram) {
        kube32_pool_reserve(80 * 1024);
        // One stack for the interpreter, while a contiguous block still
        // exists: every call into a pod (k32_init, k32_handle, k32_tick,
        // k32_event) runs on it, so the threads that make those calls need
        // no large stacks of their own. Xtensa has no tail calls, so the
        // interpreter holds a native frame per instruction until a function
        // returns: a request handler needs 20-40 KB.
        kube32_pool_reserve_stack(28672);
        size_t interp_size = kube32_pool_stack_size();
        kube32_interp_stack_set(kube32_pool_stack_alloc(interp_size), interp_size);
        // Events reach k32_event the way ticks reach k32_tick, on the shared
        // stack: the event thread needs no more stack than the tick thread
        kube32::runtime::AppPodHost::instance().set_event_thread_stack(4096, false);
        kube32::runtime::PodLogStore::runtime().set_ring_capacity(1024);   // 300 KB SRAM: a short log per pod
    } else {
        // Keep the largest PSRAM block whole for a big data pod (20 Wasm pages
        // plus header) before task stacks and buffers fragment the heap.
        kube32_pool_reserve_external(20 * 64 * 1024 + 4096);   // 20 Wasm pages plus wasm3's memory header
        // One shared interpreter stack here too: the interpreter recursion
        // peaks near 56 KB, and a stack that size on each thread that calls
        // into pods (kubelet, ticks, events, ingress, pod server) would take
        // the PSRAM a 2 MB board's pods need.
        const size_t interp_size = 64 * 1024;
        void* interp = heap_caps_malloc(interp_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (interp) {
            kube32_interp_stack_set(interp, interp_size);
            kube32::runtime::AppPodHost::instance().set_event_thread_stack(8192, true);
        }
    }
    printf("[MEM] reserved %u bytes for pod linear memory\n", (unsigned)kube32_pool_block_size());

    // Peripherals of the integrated display board
    bool has_display = false;
    if (ident.has_display) {
        has_display = kube32::hal::init_display_and_backlight();
        kube32::hal::init_speaker();
        kube32::hal::init_keyboard_scanner();
    }
    // Node storage: the microSD card of the display board (its own SPI bus,
    // FAT at /sd) when one is inserted, otherwise the internal flash
    // "storage" partition (FAT with wear levelling at /data). One of the two.
    {
        bool ok = kube32::hal::node_storage_mount(ident.has_display);
        kube32::hal::NodeStorageInfo si = kube32::hal::node_storage_info();
        if (ok) {
            printf("[STORAGE] %s at %s: %llu KB total, %llu KB free; mount took %lld bytes internal RAM, %lld bytes PSRAM\n",
                   kube32::hal::storage_medium_name(si.medium), si.root.c_str(), (unsigned long long)(si.total_bytes / 1024),
                   (unsigned long long)(si.free_bytes / 1024), (long long)si.internal_ram_bytes, (long long)si.psram_bytes);
            if (si.medium == kube32::hal::StorageMedium::SdCard) {
                kube32::hal::SdInfo card = kube32::hal::sd_info();
                printf("[SD] %s %s, SPI3 %u kHz\n", card.type.c_str(), card.name.c_str(), (unsigned)card.bus_khz);
            }
        } else {
            printf("[STORAGE] none: %s\n", si.error.c_str());
        }
    }
    // The app slots: the running image (its SHA-256 is computed here, while
    // the heap is whole) and the slot an update goes to
    s_firmware_slots = kube32::hal::create_esp_firmware_slots();
    {
        const kube32::hal::FirmwareInfo fw = s_firmware_slots->running();
        printf("[FIRMWARE] %s (%s) on %s, %s; updates go to %s\n", fw.version.c_str(), fw.sha256.substr(0, 12).c_str(), fw.slot.c_str(),
               fw.pending_verify ? "unconfirmed" : "confirmed", s_firmware_slots->passive_slot().empty() ? "nowhere (no second app slot)"
                                                                                                     : s_firmware_slots->passive_slot().c_str());
    }
    install_hardware_hooks(has_display);
    kube32::app::install_default_platform_hooks();   // event_emit through the channel layer
    // What pods may claim: the chip's pins minus those the board's own
    // peripherals use, and the buses the board has pins for. A bare devkit
    // offers both I2C controllers and UART1/UART2 (defaults: I2C0 on GPIO
    // 8/9, UART1 on 17/18). The Cardputer's only free connector is the Grove
    // port (GPIO 2 and 1), so it offers I2C0 or UART1 there. Its IR LED
    // hangs on GPIO 44, the UART0 RX pad: the board wires nothing else to it
    // and its console is the USB port, so an RMT TX channel may drive it
    // (the default TX pin); on a devkit the pad belongs to the USB-UART
    // bridge and stays reserved.
    {
        kube32::hal::BoardProfile profile;
        profile.name = ident.has_display ? "cardputer" : "generic";
        if (ident.has_display) {
            profile.reserved_gpio_mask = kube32::hal::peripheral_gpio_mask() | kube32::hal::sd_gpio_mask();
            profile.i2c_bus_mask = 0x1;
            profile.i2c_pins[0] = {2, 1};     // Grove SDA, SCL
            profile.i2c_pins[1] = {};
            profile.uart_port_mask = 0x2;
            profile.uart_pins[1] = {2, 1};    // Grove TX, RX
            profile.rmt_tx_pin_mask = 1ULL << 44;
            profile.rmt_tx_default_pin = 44;  // IR LED
        }
        kube32::hal::CapabilityFirewall::instance().set_board_profile(profile);
    }
    log_memory("after peripherals");

    // Network
    kube32::net::WifiConfig wifi_cfg;
    wifi_cfg.ap_ssid = "Kube32-Cluster";
    wifi_cfg.ap_password = ap_password;
    wifi_cfg.ssid = "Kube32-Cluster";
    wifi_cfg.password = ap_password;
    std::string uplink_ssid, uplink_pass;
    // Stored Wi-Fi credentials put the node on an existing network instead of
    // the cluster access point. A worker on an external network also needs the
    // control plane address (K32_CMD:API:<host>).
    bool uplink = read_nvs_wifi(uplink_ssid, uplink_pass);
    std::string api_host = read_nvs_str("api");
    if (api_host.empty()) api_host = "192.168.4.1";
    // K32_CMD:AP:off turns the cluster access point off (uplink only), useful
    // when a misbehaving station keeps flapping and disturbing the radio.
    bool ap_enabled = true;
    {
        nvs_handle_t h;
        char v[8] = {0};
        size_t vl = sizeof(v);
        if (nvs_open("k32", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, "ap", v, &vl) == ESP_OK && std::strcmp(v, "off") == 0) ap_enabled = false;
            nvs_close(h);
        }
    }
    if (uplink) {
        wifi_cfg.ssid = uplink_ssid;
        wifi_cfg.password = uplink_pass;
    } else if (!ident.is_control_plane && ap_password.empty()) {
        printf("[NET] no password for the cluster access point: send K32_CMD:APPASS:<password> "
               "(K32_CMD:APPASS on the control plane prints it)\n");
    }
    auto net_mgr = kube32::net::create_network_manager();
    net_mgr->init(wifi_cfg);
    if (ident.is_control_plane) {
        if (uplink && !ap_enabled) net_mgr->start_station();
        else if (uplink) net_mgr->start_station_and_softap();
        else net_mgr->start_softap();
    } else {
        net_mgr->start_station();
    }
    std::string ip = net_mgr->get_ip_address();
    printf("[NET] %s ip=%s%s\n", net_mgr->current_mode() == kube32::net::NetworkMode::Station ? "station" : "softap", ip.c_str(), uplink ? " (+uplink)" : "");
    log_memory("after wifi");
    if (uplink) {
        // Wall clock for creationTimestamp, event times and log timestamps.
        // Until the first sync the API omits those fields rather than
        // inventing them (kubectl shows AGE <unknown>).
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_set_time_sync_notification_cb([](struct timeval* tv) {
            printf("[CLOCK] synchronized, unix time %lld\n", (long long)(tv ? tv->tv_sec : 0));
        });
        esp_sntp_init();
        // Objects, leases and events get wall-clock timestamps from the start when
        // the clock answers within a few seconds; whatever is created before
        // it does is stamped afterwards (api/timestamp_backfill.hpp)
        for (int i = 0; i < 100 && kube32::osal::System::get_unix_time_ms() == 0; ++i) vTaskDelay(pdMS_TO_TICKS(100));
        if (kube32::osal::System::get_unix_time_ms() == 0) printf("[CLOCK] not synchronized yet, continuing\n");
    }

    // Node CPU utilisation for metrics.k8s.io: the time both cores spent
    // outside their idle tasks (esp_timer microseconds) against wall time.
    // Read from the idle tasks' counters: no task list, nothing allocated.
    kube32::kubelet::set_platform_cpu_sampler([](uint64_t& busy_us, uint64_t& wall_us) -> bool {
        const uint64_t wall = static_cast<uint64_t>(esp_timer_get_time());
        uint64_t idle = 0;
        for (BaseType_t core = 0; core < portNUM_PROCESSORS; ++core) {
            idle += static_cast<uint64_t>(ulTaskGetIdleRunTimeCounterForCore(core));
        }
        const uint64_t total = wall * portNUM_PROCESSORS;
        busy_us = total > idle ? total - idle : 0;
        wall_us = wall;
        return true;
    });

    // Channels and peer discovery
    kube32::ipc::ChannelManager::instance().init(ident.id, 7400);
    kube32::ipc::ChannelManager::instance().set_local_identity(ident.id, ident.name);
    // The dispatcher delivers events into Wasm pods (interpreter recursion):
    // generous when the stack can live in PSRAM.
    kube32::ipc::ChannelManager::instance().start_background(3000, ident.has_psram ? 32768 : 4096, ident.has_psram);

    // Local image registry
    {
        size_t n = 0;
        const auto* imgs = kube32::embedded::images(n);
        kube32::runtime::ImageRegistry::instance().add_all(imgs, n);
        printf("[IMAGES] %u embedded image(s):", (unsigned)n);
        for (size_t i = 0; i < n; ++i) printf(" %s(%u)", imgs[i].name, (unsigned)imgs[i].len);
        printf("\n");
    }

    auto wasm_runtime = kube32::runtime::create_wasm_runtime();
    std::unique_ptr<kube32::storage::Store> store;
    std::unique_ptr<kube32::controller::NodeLifecycleController> node_lifecycle;
    std::unique_ptr<kube32::scheduler::Scheduler> scheduler;
    std::unique_ptr<kube32::controller::DeploymentController> deployments;
    std::unique_ptr<kube32::controller::WorkloadController> workloads;
    std::unique_ptr<kube32::controller::FirmwareRolloutController> firmware_rollouts;
    std::unique_ptr<kube32::api::HttpServer> api_server;
    std::unique_ptr<kube32::kubelet::Kubelet> kubelet;
    std::unique_ptr<kube32::app::IngressController> ingress;
    std::unique_ptr<kube32::app::PodServer> pod_server;

    kube32::kubelet::KubeletConfig kubelet_cfg;
    kubelet_cfg.node_name = ident.name;
    kubelet_cfg.node_ip = ip;
    kubelet_cfg.node_ip_provider = [nm = net_mgr.get()]() { return nm->get_ip_address(); };
    kubelet_cfg.heartbeat_interval_ms = 5000;
    kubelet_cfg.sync_frequency_ms = 2000;
    // The kubelet task parses manifests and loads and compiles Wasm modules;
    // k32_init runs on the shared interpreter stack.
    kubelet_cfg.task_stack_size = kube32_interp_stack_active() ? (ident.has_psram ? 24576 : 12288) : 65536;
    kubelet_cfg.task_stack_in_psram = ident.has_psram;
    // Without PSRAM a large API answer (a list of pods carrying modules
    // inline) would take the heap the node runs on: refused instead
    kubelet_cfg.api_max_body = ident.has_psram ? 0 : 16 * 1024;
    kubelet_cfg.control_plane = ident.is_control_plane;
    kubelet_cfg.core_affinity = 1;
    // K32_CMD:TOKEN: the API server checks it, every kubelet sends it
    const std::string api_token = read_nvs_str("token");
    s_api_token_set = valid_token(api_token);
    if (s_api_token_set) kubelet_cfg.api_token = api_token;
    kubelet_cfg.node_labels["kube32.io/board"] = ident.has_display ? "cardputer" : "generic";
    // Devices answering on the board's I2C pins become node labels
    kubelet_cfg.node_label_source = [] {
        static kube32::hal::I2cDiscovery discovery;
        discovery.scan();
        return discovery.labels();
    };
    kubelet_cfg.node_label_prefix = "i2c.kube32.io/";
    // Firmware rollouts: the kubelet downloads the image into the update slot
    // in ranges, or on the control plane boots the slot it was staged in;
    // before the restart the control plane writes out its store journal
    kubelet_cfg.firmware_slots = s_firmware_slots;
    kubelet_cfg.firmware.local_staging = ident.is_control_plane;
    kubelet_cfg.firmware.range_bytes = ident.has_psram ? 32 * 1024 : 16 * 1024;
    kubelet_cfg.firmware.before_reboot = [] {
        if (s_store) s_store->flush(true);
    };

    if (ident.is_control_plane) {
        // The object store's journal: the raw `journal` partition on a board
        // whose storage root is internal flash, a file on a card; without
        // either the cluster state is kept in RAM only
        kube32::storage::StoreOptions store_opts;
        store_opts.enable_flash_persistence = false;
        // Journal writes made for API requests (commit) run on the API
        // workers, whose stacks may be in PSRAM
        store_opts.io_call = [](const std::function<void()>& fn) { kube32::hal::node_storage_call(fn); };
        static std::unique_ptr<kube32::storage::FlashRegion> journal;
        const kube32::hal::NodeStorageInfo storage_info = kube32::hal::node_storage_info();
        const std::string journal_dir = kube32::hal::node_storage_path("store");
        const std::string journal_file = journal_dir + "/journal.img";
        if (storage_info.medium == kube32::hal::StorageMedium::InternalFlash) {
            // Not a file on the FAT volume: FATFS rewrites a file's directory
            // entry on every sync and wear levelling rewrites a sector by
            // erasing it first, so a reset between the two loses the whole
            // file. A journal.img left on the FAT volume is moved over once.
            kube32::hal::node_storage_call([&]() { journal = kube32::storage::create_partition_flash_region("journal", 1u << 20); });
            bool found = false, moved = false, copied = false;
            if (journal && !journal_dir.empty()) {
                kube32::hal::node_storage_call([&]() {
                    struct stat st;
                    found = ::stat(journal_file.c_str(), &st) == 0;
                    if (!found) return;
                    auto file = kube32::storage::create_file_flash_region(journal_file, journal->size());
                    moved = file && kube32::storage::FlashLog::copy_log(*file, *journal, copied);
                    if (moved) {
                        file.reset();
                        ::unlink(journal_file.c_str());
                    } else {
                        // Retried at the next boot; this one keeps using the file
                        journal = std::move(file);
                    }
                });
            }
            if (found && !moved) {
                printf("[STORE] %s could not be moved to the journal partition\n", journal_file.c_str());
                if (journal) s_journal_location = journal_file;
            } else if (journal) {
                if (copied) printf("[STORE] %s moved to the journal partition\n", journal_file.c_str());
                s_journal_location = "partition:journal";
            }
        }
        if (!journal && !journal_dir.empty() && kube32::hal::node_storage_make_dirs(journal_dir)) {
            // 1 MiB on internal flash without a journal partition (an older
            // partition table), 4 MiB on a card
            const size_t journal_bytes = storage_info.medium == kube32::hal::StorageMedium::SdCard ? 4u << 20 : 1u << 20;
            kube32::hal::node_storage_call([&]() { journal = kube32::storage::create_file_flash_region(journal_file, journal_bytes); });
            if (journal) s_journal_location = journal_file;
            if (journal && storage_info.medium == kube32::hal::StorageMedium::InternalFlash) {
                printf("[STORE] no journal partition in the partition table: the journal is a file on the FAT volume, "
                       "which a reset during a write can lose (flash the current partitions.csv)\n");
            }
        }
        if (journal) {
            store_opts.enable_flash_persistence = true;
            store_opts.flash_region = journal.get();
        } else {
            printf("[STORE] no journal (node storage: %s): cluster state is kept in RAM only\n",
                   storage_info.error.empty() ? "not mounted" : storage_info.error.c_str());
        }
        store = kube32::storage::create_radix_store(store_opts);
        if (journal) {
            const uint64_t t0 = kube32::osal::System::get_uptime_ms();
            kube32::storage::Status rs = kube32::storage::Status::Ok();
            kube32::hal::node_storage_call([&]() { rs = store->recover(); });
            std::vector<std::string> keys;
            store->list("/registry/", keys);
            printf("[STORE] journal %s: %u object(s) recovered in %llu ms, revision %llu%s\n", s_journal_location.c_str(),
                   (unsigned)keys.size(), (unsigned long long)(kube32::osal::System::get_uptime_ms() - t0),
                   (unsigned long long)store->current_version(), rs.ok() ? "" : (" (" + rs.message + ")").c_str());
        }
        s_store = store.get();

        kube32::controller::NodeLifecycleConfig nl_cfg;
        nl_cfg.check_interval_ms = 1000;
        nl_cfg.not_ready_threshold_ms = 15000;
        nl_cfg.eviction_threshold_ms = 30000;
        node_lifecycle = std::make_unique<kube32::controller::NodeLifecycleController>(*store, nl_cfg);
        node_lifecycle->start();

        kube32::scheduler::SchedulerConfig sched_cfg;
        sched_cfg.strategy = kube32::scheduler::ScoringStrategy::LeastAllocated;
        sched_cfg.reconcile_interval_ms = 3000;
        sched_cfg.node_heartbeat_timeout_ms = 30000;
        scheduler = kube32::scheduler::create_scheduler(*store, sched_cfg);
        scheduler->start();

        deployments = std::make_unique<kube32::controller::DeploymentController>(*store, scheduler.get());
        deployments->start();

        kube32::controller::WorkloadControllerConfig wc_cfg;
        wc_cfg.reconcile_interval_ms = 1000;
        wc_cfg.stack_in_psram = ident.has_psram;
        workloads = std::make_unique<kube32::controller::WorkloadController>(*store, scheduler.get(), wc_cfg);
        workloads->start();

        // FirmwareRollouts: driven from the housekeeping loop below
        firmware_rollouts = std::make_unique<kube32::controller::FirmwareRolloutController>(*store, [] {
            return s_firmware_api ? s_firmware_api->staged() : kube32::api::StagedFirmware{};
        });

        kube32::api::HttpServerOptions http_opts;
        http_opts.host = "0.0.0.0";
        http_opts.port = 6443;
        http_opts.backlog = 4;
        http_opts.rx_buffer_size = 4096;
        http_opts.workers = ident.has_psram ? 2 : 1;
        http_opts.worker_stack_bytes = 16384; // pod admission + scheduler run on this stack
        http_opts.worker_stack_in_psram = ident.has_psram;
        http_opts.worker_core_affinity = 1;
        http_opts.name = "k32_api";
        // Watch streams (k9s opens several): one hub thread owns the sockets
        http_opts.watch_streams = ident.has_psram ? 8 : 4;
        http_opts.watch_stack_bytes = 16384;
        http_opts.watch_stack_in_psram = ident.has_psram;
        // HTTPS on the same port: kubectl sends a bearer token only over TLS.
        // The key and self-signed certificate are made once and kept in NVS.
        {
            static std::unique_ptr<kube32::osal::TlsServer> tls;
            std::string tls_error;
            const uint64_t t0 = kube32::osal::System::get_uptime_ms();
            const size_t ram0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            tls = kube32::osal::TlsServer::create({ident.name}, {ip},
                [](std::string& key, std::string& cert) { return read_nvs_blob("tls_key", key) && read_nvs_blob("tls_crt", cert); },
                [](const std::string& key, const std::string& cert) { write_nvs_blob("tls_key", key); write_nvs_blob("tls_crt", cert); },
                tls_error);
            if (tls) {
                http_opts.tls = tls.get();
                s_tls = tls.get();
                printf("[TLS] API server certificate ready in %llu ms (%d bytes internal RAM): https://%s:6443, tls-server-name kube32-apiserver\n",
                       (unsigned long long)(kube32::osal::System::get_uptime_ms() - t0),
                       (int)((long)ram0 - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)), ip.c_str());
            } else {
                printf("[TLS] off: %s\n", tls_error.c_str());
            }
        }
        api_server = std::make_unique<kube32::api::HttpServer>(http_opts, *store, scheduler.get());
        if (s_api_token_set) api_server->route_handler().set_bearer_token(api_token);
        // Node proxy (/api/v1/nodes/<node>/proxy/...): this node's storage
        // in-process, the workers' through their kubelet endpoint
        static std::unique_ptr<kube32::api::NodeProxyHandler> node_proxy;
        node_proxy = std::make_unique<kube32::api::NodeProxyHandler>(*store, api_server->route_handler(), ident.name,
            [](kube32::api::HttpConnection& conn, std::string_view path) {
                if (!kube32::app::is_node_storage_path(path)) return false;
                return kube32::app::serve_node_storage(conn, path, std::string(), 8u * 1024 * 1024);
            });
        if (s_api_token_set) node_proxy->set_node_token(api_token);
        // The staged firmware image (/apis/kube32.io/v1/firmware); everything
        // else goes on to the node proxy
        static std::unique_ptr<kube32::api::FirmwareApi> firmware_api;
        firmware_api = std::make_unique<kube32::api::FirmwareApi>(*s_firmware_slots, *store, api_server->route_handler(), node_proxy.get());
        s_firmware_api = firmware_api.get();
        api_server->set_handler(firmware_api.get());
        if (!api_server->start()) ESP_LOGE(TAG, "API server failed to start");

        // k32_http_call: Services and NetworkPolicies read from the store
        static std::unique_ptr<kube32::app::ServiceClient> service_client;
        kube32::app::ServiceClientOptions sc_opts;
        sc_opts.pool_size = 4;
        service_client = std::make_unique<kube32::app::ServiceClient>(kube32::app::make_store_service_resolver(*store), sc_opts,
                                                                      kube32::app::make_store_egress_source(*store));
        kube32::app::install_service_client(service_client.get());
        s_api_server = api_server.get();
        printf("[CONTROL-PLANE] API server http://%s:6443, scheduler + node lifecycle active\n", ip.c_str());

        // Pods here behind a Service are reached by service calls from other
        // nodes through the pod HTTP server; the ingress calls them in-process.
        // One worker; the pod's interpreter runs on the shared stack.
        {
            kube32::app::PodServerOptions po;
            po.port = 7000;
            po.workers = 1;
            po.worker_stack = kube32_interp_stack_active() ? 16384 : 49152;
            po.core_affinity = 1;
            po.stacks_in_psram = ident.has_psram;
            if (s_api_token_set) po.token = api_token;
            pod_server = std::make_unique<kube32::app::PodServer>(po);
            if (pod_server->start()) kubelet_cfg.kubelet_port = po.port;
        }

        kubelet_cfg.api_server_host = "127.0.0.1";
        kubelet_cfg.api_server_port = 6443;
        kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        kubelet->start();

        // Ingress controller on port 80, routes from networking.k8s.io/v1
        // Ingress objects. It calls request-handling pods on this node
        // directly.
        kube32::app::IngressControllerOptions io;
        io.store = store.get();
        io.local_node_name = ident.name;
        io.address = [nm = net_mgr.get()]() { return nm->get_ip_address(); };
        io.port = 80;
        // One worker: calls into a pod are serialized per pod anyway, and SSE
        // streams leave the worker once their head is sent. Local pod calls
        // run on the shared interpreter stack.
        io.workers = 1;
        io.worker_stack = kube32_interp_stack_active() ? (ident.has_psram ? 16384 : 8192) : 65536;
        io.rx_buffer = ident.has_psram ? 4096 : 2560;
        io.core_affinity = 1;
        io.stacks_in_psram = ident.has_psram;
        ingress = std::make_unique<kube32::app::IngressController>(io);
        ingress->start();
    } else {
        kubelet_cfg.api_server_host = api_host;
        kubelet_cfg.api_server_port = 6443;
        // The control plane may sit on another access point: reach it over UDP too
        kube32::ipc::ChannelManager::instance().add_seed_peer(api_host, 7400);

        // The pod HTTP server: the ingress controller and other nodes' service
        // calls reach request-handling pods here, and it is the kubelet
        // endpoint for node storage. A board without PSRAM runs one worker
        // whose stack holds one request pod's interpreter.
        kube32::app::PodServerOptions po;
        po.port = 7000;
        po.workers = ident.has_psram ? 2 : 1;
        // Without PSRAM pods are interpreted on the shared interpreter stack;
        // the worker's own stack only serves HTTP and node storage
        po.worker_stack = ident.has_psram ? 24576 : 10240;   // FATFS over SPI for node storage needs about 10 KB
        po.rx_buffer = ident.has_psram ? 4096 : 2048;
        if (!ident.has_psram) {
            // A request waiting here keeps its bytes in Wi-Fi RX buffers
            // until it is read; with one worker and a slow pod, a deep queue
            // took every buffer and the node stopped receiving
            po.backlog = 2;
            po.accept_queue_depth = 1;
            po.max_idle_connections = 2;
        }
        po.core_affinity = 1;
        po.stacks_in_psram = ident.has_psram;
        if (s_api_token_set) po.token = api_token;
        pod_server = std::make_unique<kube32::app::PodServer>(po);
        if (pod_server->start()) kubelet_cfg.kubelet_port = po.port;

        // k32_http_call: Services and their Endpoints read from the API
        // server; the kubelet keeps the NetworkPolicies of this node's
        // namespaces with its pod sync
        static std::shared_ptr<kube32::kubelet::HttpKubeClient> svc_api;
        svc_api = std::make_shared<kube32::kubelet::HttpKubeClient>(api_host, 6443);
        if (s_api_token_set) svc_api->set_bearer_token(api_token);
        if (!ident.has_psram) svc_api->set_max_body(16 * 1024);
        auto api_get = [](const std::string& path, std::string& body) { return svc_api->get(path, body); };
        kubelet_cfg.network_policies = std::make_shared<kube32::net::NodeNetworkPolicies>();
        static std::unique_ptr<kube32::app::ServiceClient> service_client;
        service_client = std::make_unique<kube32::app::ServiceClient>(kube32::app::make_api_service_resolver(api_get),
                                                                      kube32::app::ServiceClientOptions{},
                                                                      kube32::app::make_node_egress_source(kubelet_cfg.network_policies, api_get));
        kube32::app::install_service_client(service_client.get());

        kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        kubelet->start();
        printf("[WORKER] kubelet -> http://%s:6443\n", api_host.c_str());
        if (has_display) {
            kube32::hal::display_fill_rect(0, 0, kube32::hal::LCD_WIDTH, 18, 0x194B);
            // The boot screen stays product-neutral on purpose: what runs the
            // board is not announced on the display, the apps draw their own UI.
            kube32::hal::display_draw_string(6, 5, "ready", kube32::hal::COLOR_GREEN, 0x194B);
            std::string l = ip;
            kube32::hal::display_draw_string(6, 28, l.c_str(), kube32::hal::COLOR_WHITE, kube32::hal::COLOR_BG_DARK);
            std::string app_url = "http://" + api_host + "/";
            kube32::hal::display_draw_string(6, 40, app_url.c_str(), kube32::hal::COLOR_CYAN, kube32::hal::COLOR_BG_DARK);
        }
    }
    log_memory("cluster services up");

    // Console on a small task of its own
    console.init(ident.has_display);   // USB-Serial-JTAG on the display board, UART0 (CH340) on the devkit
    if (!ident.is_control_plane) s_forward_host = api_host;   // workers relay console HTTP to the control plane
    if (ingress) s_ingress_server = ingress->server();
    {
        kube32::app::ConsoleRelayOptions ro;
        ro.host = s_forward_host;
        ro.role = ident.is_control_plane ? "control-plane" : "worker";
        // An idle stream (an event stream or a watch) costs a socket; the
        // ingress takes 6 event streams and the API server 8 watches
        ro.slots = ident.has_psram ? 10 : 2;
        ro.chunk = ident.has_psram ? 2048 : 768;   // a DATA frame fits the 4096 or 2048-byte TX ring
        ro.stack_size = ident.has_psram ? 6144 : 5120;
        ro.stack_in_psram = ident.has_psram;
        s_relay = std::make_unique<kube32::app::ConsoleRelay>(ro, [](const char* data, size_t len) {
            return console.write_frame(data, len, 2000);
        });
    }
    static std::pair<Console*, NodeIdentity*> console_ctx(&console, &ident);
    // The USB-Serial/JTAG driver drops what arrives while its receive ring is
    // full (the host gets no backpressure), so the reader runs above the pod,
    // kubelet, HTTP and storage threads and drains a request from the gateway
    // as fast as it arrives. It blocks in reads nearly all the time.
    xTaskCreatePinnedToCore(console_task, "k32_console", 4096, &console_ctx, 11, nullptr, 0);

    // Pod ticks run the interpreter, so they get a thread with a stack sized
    // for it rather than this 9 KB task. A pod is ticked once per
    // kube32.io/tick-interval (1 s by default); one that returns >0 from
    // k32_tick has background work left (incremental seeding, for example)
    // and is ticked again after a short yield so requests interleave.
    {
        kube32::osal::Thread::Config t_cfg;
        t_cfg.name = "k32_tick";
        t_cfg.stack_size = kube32_interp_stack_active() ? (ident.has_psram ? 8192 : 4096) : 65536;
        t_cfg.stack_in_psram = ident.has_psram;
        t_cfg.priority = 4;
        t_cfg.core_affinity = 1;
        kube32::osal::Thread::create([](void*) {
            while (true) {
                const uint32_t wait = kube32::runtime::AppPodHost::instance().tick_due(kube32::osal::System::get_uptime_ms());
                vTaskDelay(pdMS_TO_TICKS(std::max<uint32_t>(wait, 10)));
            }
        }, nullptr, t_cfg);
    }

    // Housekeeping: ingress controller tick (routing table, Ingress status,
    // stats topics, telemetry streams), peer seeding, memory log
    uint32_t iter = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        iter++;
        if (ingress) ingress->tick();
        if (firmware_rollouts && iter % 3 == 0) {
            try {
                firmware_rollouts->reconcile();
            } catch (const std::exception& e) {
                printf("[FIRMWARE] rollout pass skipped: %s\n", e.what());
            }
        }
        if (store) {
            // Changes no API request waited for (controllers, Node
            // heartbeats) reach the journal here
            store->flush(false);
            // Objects made before the clock was set get their timestamps once it is
            static int backfills = 0;
            if (backfills < 2 && kube32::osal::System::get_unix_time_ms() != 0 && (backfills == 0 || iter % 60 == 0)) {
                size_t n = kube32::api::backfill_timestamps(*store);
                if (n) printf("[CLOCK] timestamps set on %u object(s) created before the clock synchronized\n", (unsigned)n);
                ++backfills;
            }
        }
        if (ident.is_control_plane && store && iter % 10 == 0) try {
            // Workers register their InternalIP; seed them as UDP peers so the
            // channel fabric works across access points, not only over ESP-NOW.
            std::vector<std::string> nodes;
            store->list("/registry/nodes/", nodes);
            for (const auto& nj : nodes) {
                kube32::models::JsonValue doc;
                std::string err;
                if (!kube32::models::parse_json(nj, doc, err)) continue;
                const auto* meta = doc.get("metadata");
                if (meta && meta->get_string("name", "") == ident.name) continue;
                const auto* status = doc.get("status");
                const auto* addrs = status ? status->get("addresses") : nullptr;
                if (!addrs || !addrs->is_array()) continue;
                for (const auto& a : addrs->arr_val) {
                    if (a.get_string("type", "") == "InternalIP") kube32::ipc::ChannelManager::instance().add_seed_peer(a.get_string("address", ""), 7400);
                }
            }
        } catch (const std::exception& e) {
            printf("[IPC] peer seeding skipped: %s\n", e.what());
        }
        if (!ident.is_control_plane && kubelet && iter % 10 == 0) {
            // A station can stay associated with an access point that no
            // longer knows it: a control plane that restarts its AP within
            // the beacon timeout drops the worker's frames while its beacons
            // keep the link up. Join again when the API server has not
            // answered a pod sync for a minute on a link that looks up.
            static uint64_t quiet_since = kube32::osal::System::get_uptime_ms();
            const uint64_t now = kube32::osal::System::get_uptime_ms();
            const uint64_t heard = std::max(kubelet->last_sync_ok_ms(), quiet_since);
            if (net_mgr->station_has_ip() && now - heard > 60000) {
                printf("[NET] no answer from the control plane for %u s on a connected link, joining the access point again\n",
                       (unsigned)((now - heard) / 1000));
                net_mgr->rejoin();
                quiet_since = now;
            }
        }
        if (iter % 30 == 0) log_memory("steady state");
        kube32::osal::System::feed_watchdog();
    }
}

} // namespace

extern "C" void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    xTaskCreatePinnedToCore(cluster_task, "k32_cluster", 9216, nullptr, 5, nullptr, 0);
}

#endif
