// Kube32 firmware entry point for ESP32-S3.
//
// One image boots both cluster roles. A board with PSRAM becomes the control
// plane and any other board a worker; the NVS key "role" (K32_CMD:ROLE:)
// overrides that, and the NVS key "board" (K32_CMD:BOARD:) selects the
// peripheral profile.
//   control plane  API server, scheduler, controllers, object store, ingress
//                  on port 80 and a local kubelet
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
#include "hal/capability_firewall.hpp"
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

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
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
            usb_serial_jtag_driver_config_t cfg = { .tx_buffer_size = 4096, .rx_buffer_size = 4096 };
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
    int read(char* buf, size_t cap, uint32_t timeout_ms) {
        int n = usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(uart_enabled ? timeout_ms / 2 : timeout_ms));
        if (n > 0) { last_from_usb = true; return n; }
        if (!uart_enabled) return n;
        n = uart_read_bytes(UART_NUM_0, buf, cap, pdMS_TO_TICKS(timeout_ms / 2));
        if (n > 0) last_from_usb = false;
        return n;
    }
    void write(const char* data, size_t len) {
        if (last_from_usb || !uart_enabled) {
            size_t off = 0;
            while (off < len) {
                int n = usb_serial_jtag_write_bytes(data + off, len - off, pdMS_TO_TICKS(200));
                if (n <= 0) break;
                off += static_cast<size_t>(n);
            }
        } else {
            uart_write_bytes(UART_NUM_0, data, len);
        }
    }
};

// Forwards one raw HTTP request to a server over loopback, or over the cluster
// network when this node does not host the port (workers forward to the control
// plane). The request must carry "Connection: close".
static std::string s_forward_host = "127.0.0.1";
static kube32::api::HttpServer* s_api_server = nullptr;       // for console toggles
static kube32::storage::Store* s_store = nullptr;             // control plane: journal figures for the console
static bool s_api_token_set = false;
static kube32::osal::TlsServer* s_tls = nullptr;               // control plane: the API server's certificate
static kube32::api::HttpServer* s_ingress_server = nullptr;
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

static uint32_t crc32_bytes(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Console protocol (line oriented, binary-safe payloads):
//   K32_HTTP <port> <len> [crc32hex]\n<len bytes of HTTP request> -> K32_RESP <len>\n<bytes>
//   A CRC mismatch answers K32_ERR crc so the gateway can resend.
//   K32_CMD:STATUS                                       -> telemetry line
//   K32_CMD:STORE                                        -> object store journal figures
//   K32_CMD:WIFI:<ssid>:<password>                       -> stored in NVS, reboot
//   K32_CMD:NAME:<node name>|auto                        -> stored in NVS, reboot
//   K32_CMD:TOKEN:<API bearer token>|off                 -> stored in NVS, reboot
//   K32_CMD:CERT                                         -> the API server's TLS certificate (PEM)
//   K32_CMD:REBOOT
void console_task(void* arg) {
    auto* ctx = static_cast<std::pair<Console*, NodeIdentity*>*>(arg);
    Console& con = *ctx->first;
    NodeIdentity& ident = *ctx->second;
    std::string line;
    char rx[256];
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
            if (line.rfind("K32_HTTP ", 0) == 0) {
                unsigned port = 0, len = 0, crc = 0;
                int fields = sscanf(line.c_str() + 9, "%u %u %x", &port, &len, &crc);
                if (fields >= 2 && len > 0 && len < 64 * 1024) {
                    std::string req;
                    req.reserve(len);
                    // Bytes left in rx after the newline belong to the payload
                    for (int j = i + 1; j < n && req.size() < len; ++j) req.push_back(rx[j]);
                    i = n;
                    uint32_t waited = 0;
                    while (req.size() < len && waited < 5000) {
                        int m = con.read(rx, sizeof(rx) < (len - req.size()) ? sizeof(rx) : (len - req.size()), 100);
                        if (m > 0) req.append(rx, static_cast<size_t>(m)); else waited += 100;
                    }
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
                        kube32::hal::node_storage_path("store/journal.img").c_str(), (unsigned)ps.live_keys, (unsigned)ps.live_bytes,
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
                    for (UBaseType_t i = 0; i < got && !found; ++i) {
                        if (std::strncmp(st[i].pcTaskName, want.c_str(), want.size()) != 0) continue;
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

void log_memory(const char* stage) {
    printf("[MEM] %-22s internal free %6u (min %6u)  psram free %7u\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void cluster_task(void* pv) {
    (void)pv;
    static NodeIdentity ident = detect_identity();
    static Console console;
    print_banner(ident);

    // Without PSRAM, reserve one Wasm page (+ interpreter headroom) now, before
    // Wi-Fi and lwIP fragment the heap, so a 64 KiB pod can still be placed.
    if (!ident.has_psram) {
        kube32_pool_reserve(67 * 1024);
        // One stack for the interpreter, while a contiguous block still
        // exists: every call into a pod (k32_init, k32_handle, k32_tick,
        // k32_event) runs on it, so the threads that make those calls need
        // no large stacks of their own. Xtensa has no tail calls, so the
        // interpreter holds a native frame per instruction until a function
        // returns: a request handler needs 20-40 KB.
        kube32_pool_reserve_stack(40960);
        size_t interp_size = kube32_pool_stack_size();
        kube32_interp_stack_set(kube32_pool_stack_alloc(interp_size), interp_size);
        kube32::runtime::AppPodHost::instance().set_event_thread_stack(6144, false);
        kube32::runtime::PodLogStore::runtime().set_ring_capacity(1024);   // 300 KB SRAM: a short log per pod
    } else {
        // Keep the largest PSRAM block whole for a big data pod (20 Wasm pages
        // plus header) before task stacks and buffers fragment the heap.
        kube32_pool_reserve_external(20 * 64 * 1024 + 4096);   // 20 Wasm pages plus wasm3's memory header
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
    install_hardware_hooks(has_display);
    kube32::app::install_default_platform_hooks();   // event_emit through the channel layer
    // What pods may claim: the chip's pins minus those the board's own
    // peripherals use, and the buses the board has pins for. A bare devkit
    // offers both I2C controllers and UART1/UART2 (defaults: I2C0 on GPIO
    // 8/9, UART1 on 17/18). The Cardputer's only free connector is the Grove
    // port (GPIO 2 and 1), so it offers I2C0 or UART1 there.
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
        }
        kube32::hal::CapabilityFirewall::instance().set_board_profile(profile);
    }
    log_memory("after peripherals");

    // Network
    kube32::net::WifiConfig wifi_cfg;
    wifi_cfg.ap_ssid = "Kube32-Cluster";
    wifi_cfg.ap_password = "kube32password";
    wifi_cfg.ssid = "Kube32-Cluster";
    wifi_cfg.password = "kube32password";
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

    // Node CPU utilisation for metrics.k8s.io: cumulative run time of every
    // non-idle task (both cores, esp_timer microseconds) against wall time.
    kube32::kubelet::set_platform_cpu_sampler([](uint64_t& busy_us, uint64_t& wall_us) -> bool {
        UBaseType_t n = uxTaskGetNumberOfTasks();
        auto* states = static_cast<TaskStatus_t*>(heap_caps_malloc((n + 4) * sizeof(TaskStatus_t), MALLOC_CAP_8BIT));
        if (!states) return false;
        configRUN_TIME_COUNTER_TYPE total = 0;
        UBaseType_t got = uxTaskGetSystemState(states, n + 4, &total);
        uint64_t busy = 0;
        for (UBaseType_t i = 0; i < got; ++i) {
            if (states[i].pcTaskName && std::strncmp(states[i].pcTaskName, "IDLE", 4) == 0) continue;
            busy += static_cast<uint64_t>(states[i].ulRunTimeCounter);
        }
        heap_caps_free(states);
        if (got == 0) return false;
        busy_us = busy;
        wall_us = static_cast<uint64_t>(esp_timer_get_time());
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
    // The kubelet task parses manifests and loads/compiles Wasm modules (deep
    // recursion); workers with PSRAM get a generous stack.
    // Module parse + compile + k32_init (seeding) all run on the kubelet task.
    // Runs k32_init: on Xtensa the interpreter's op chain is not fully tail
    // call optimised, so a seed chunk needs well over 40 KB of native stack.
    kubelet_cfg.task_stack_size = ident.has_psram ? 65536 : 12288;
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

    if (ident.is_control_plane) {
        // The object store's journal lives on the node storage root; without
        // one the cluster state is kept in RAM only
        kube32::storage::StoreOptions store_opts;
        store_opts.enable_flash_persistence = false;
        static std::unique_ptr<kube32::storage::FlashRegion> journal;
        const kube32::hal::NodeStorageInfo storage_info = kube32::hal::node_storage_info();
        const std::string journal_dir = kube32::hal::node_storage_path("store");
        if (!journal_dir.empty() && kube32::hal::node_storage_make_dirs(journal_dir)) {
            // 1 MiB on internal flash (the rest is for volumes and logs), 4 MiB on a card
            const size_t journal_bytes = storage_info.medium == kube32::hal::StorageMedium::SdCard ? 4u << 20 : 1u << 20;
            kube32::hal::node_storage_call([&]() {
                journal = kube32::storage::create_file_flash_region(journal_dir + "/journal.img", journal_bytes);
            });
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
            printf("[STORE] journal %s/journal.img: %u object(s) recovered in %llu ms, revision %llu%s\n", journal_dir.c_str(),
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
        api_server->set_handler(node_proxy.get());
        if (!api_server->start()) ESP_LOGE(TAG, "API server failed to start");

        // k32_http_call: Services resolved from the store
        static std::unique_ptr<kube32::app::ServiceClient> service_client;
        kube32::app::ServiceClientOptions sc_opts;
        sc_opts.pool_size = 4;
        service_client = std::make_unique<kube32::app::ServiceClient>(kube32::app::make_store_service_resolver(*store), sc_opts);
        kube32::app::install_service_client(service_client.get());
        s_api_server = api_server.get();
        printf("[CONTROL-PLANE] API server http://%s:6443, scheduler + node lifecycle active\n", ip.c_str());

        // Pods here behind a Service are reached by service calls from other
        // nodes through the pod HTTP server; the ingress calls them in-process.
        // One worker: its stack (PSRAM) holds one request pod's interpreter.
        {
            kube32::app::PodServerOptions po;
            po.port = 7000;
            po.workers = 1;
            po.worker_stack = 49152;
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
        // streams leave the worker once their head is sent. Each worker stack
        // holds a local pod call (the interpreter recursion peaks near 56 KB),
        // so a second one would cost 64 KB of PSRAM that a second app's pod
        // needs more.
        io.workers = 1;
        io.worker_stack = ident.has_psram ? 65536 : 8192;
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
        po.worker_stack = ident.has_psram ? 24576 : 12288;   // FATFS over SPI for node storage needs about 10 KB
        po.rx_buffer = ident.has_psram ? 4096 : 2048;
        po.core_affinity = 1;
        po.stacks_in_psram = ident.has_psram;
        if (s_api_token_set) po.token = api_token;
        pod_server = std::make_unique<kube32::app::PodServer>(po);
        if (pod_server->start()) kubelet_cfg.kubelet_port = po.port;

        // k32_http_call: Services and their Endpoints read from the API server
        static std::shared_ptr<kube32::kubelet::HttpKubeClient> svc_api;
        svc_api = std::make_shared<kube32::kubelet::HttpKubeClient>(api_host, 6443);
        if (s_api_token_set) svc_api->set_bearer_token(api_token);
        if (!ident.has_psram) svc_api->set_max_body(16 * 1024);
        static std::unique_ptr<kube32::app::ServiceClient> service_client;
        service_client = std::make_unique<kube32::app::ServiceClient>(kube32::app::make_api_service_resolver(
            [](const std::string& path, std::string& body) { return svc_api->get(path, body); }));
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
    static std::pair<Console*, NodeIdentity*> console_ctx(&console, &ident);
    xTaskCreatePinnedToCore(console_task, "k32_console", 4096, &console_ctx, 4, nullptr, 0);

    // Pod ticks run the interpreter, so they get a thread with a stack sized
    // for it rather than this 9 KB task. A pod that returns >0 from k32_tick
    // has background work left (incremental seeding, for example) and is
    // ticked again after a short yield so requests interleave; otherwise 1 Hz.
    {
        kube32::osal::Thread::Config t_cfg;
        t_cfg.name = "k32_tick";
        t_cfg.stack_size = ident.has_psram ? 65536 : 4096;   // same reason as the kubelet stack; display boards host no ticking pods
        t_cfg.stack_in_psram = ident.has_psram;
        t_cfg.priority = 4;
        t_cfg.core_affinity = 1;
        kube32::osal::Thread::create([](void*) {
            while (true) {
                bool busy = false;
                uint32_t now = static_cast<uint32_t>(kube32::osal::System::get_uptime_ms());
                for (const auto& id : kube32::runtime::AppPodHost::instance().list_pods()) {
                    if (kube32::runtime::AppPodHost::instance().tick(id, now) > 0) busy = true;
                }
                vTaskDelay(pdMS_TO_TICKS(busy ? 10 : 1000));
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
        if (store) {
            // Changes reach the journal here, on this task's internal-RAM stack
            kube32::hal::node_storage_call([&]() { store->flush(false); });
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
