// Kube32 firmware entry point for ESP32-S3.
//
// One image boots both cluster roles; the factory MAC selects the role.
//   esp32-node-01  control plane + ingress + local kubelet, SoftAP host,
//                  integrated display/keyboard/speaker exposed to pods
//   esp32-node-02  worker: kubelet, pod HTTP server, Wi-Fi station
//
// Nothing application specific lives here: workloads arrive as Wasm pods
// through the Kubernetes API, images come from the embedded registry, and
// hardware reaches pods only through capability-gated host functions.

#if defined(KUBE32_TARGET_ESP32)

#include "storage/store.hpp"
#include "scheduler/scheduler.hpp"
#include "controller/node_lifecycle_controller.hpp"
#include "controller/deployment_controller.hpp"
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
#include "app/app_wiring.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"
#include "osal/socket.hpp"
#include "hal/tft_keyboard_hardware.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/pod_log.hpp"
#include "models/kube_models.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>

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
    uint16_t id{1};
    std::string mac;
};

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
    id.name = control_plane ? "esp32-node-01" : "esp32-node-02";
    id.id = control_plane ? 1 : 2;
    return id;
}

// Reads one string from the "k32" NVS namespace; empty when absent.
std::string read_nvs_str(const char* key) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READONLY, &h) != ESP_OK) return {};
    char v[65] = {0};
    size_t vl = sizeof(v);
    bool ok = nvs_get_str(h, key, v, &vl) == ESP_OK;
    nvs_close(h);
    return ok ? std::string(v) : std::string();
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
//   K32_CMD:WIFI:<ssid>:<password>                       -> stored in NVS, reboot
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
                char buf[256];
                int l = snprintf(buf, sizeof(buf),
                    "K32_STATUS node=%s role=%s mac=%s free_heap=%u min_free_heap=%u free_psram=%u uptime_ms=%llu pods=%u\n",
                    ident.name.c_str(), ident.is_control_plane ? "control-plane" : "worker", ident.mac.c_str(),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned long long)kube32::osal::System::get_uptime_ms(),
                    (unsigned)kube32::runtime::AppPodHost::instance().list_pods().size());
                con.write(buf, static_cast<size_t>(l));
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
        kube32_pool_reserve_stack(49152);   // one pod thread stack, while a contiguous block still exists
        kube32::runtime::PodLogStore::runtime().set_ring_capacity(1024);   // 300 KB SRAM: a short log per pod
    } else {
        // Keep the largest PSRAM block whole for a big data pod (24 Wasm pages
        // plus header) before task stacks and buffers fragment the heap.
        kube32_pool_reserve_external(1340 * 1024);   // 20 pages + header
    }
    printf("[MEM] reserved %u bytes for pod linear memory\n", (unsigned)kube32_pool_block_size());

    // Peripherals of the integrated display board
    bool has_display = false;
    if (ident.has_display) {
        has_display = kube32::hal::init_display_and_backlight();
        kube32::hal::init_speaker();
        kube32::hal::init_keyboard_scanner();
    }
    install_hardware_hooks(has_display);
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
    std::unique_ptr<kube32::api::HttpServer> api_server;
    std::unique_ptr<kube32::kubelet::Kubelet> kubelet;
    std::unique_ptr<kube32::app::AppWiring> wiring;

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
    kubelet_cfg.control_plane = ident.is_control_plane;
    kubelet_cfg.core_affinity = 1;

    if (ident.is_control_plane) {
        kube32::storage::StoreOptions store_opts;
        store_opts.base_path = "/littlefs";
        store_opts.enable_flash_persistence = false;
        store = kube32::storage::create_radix_store(store_opts);

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
        api_server = std::make_unique<kube32::api::HttpServer>(http_opts, *store, scheduler.get());
        api_server->route_handler().set_lease_callback([nl = node_lifecycle.get()](const std::string& node) {
            if (nl) nl->record_node_lease(node);
        });
        if (!api_server->start()) ESP_LOGE(TAG, "API server failed to start");
        s_api_server = api_server.get();
        printf("[CONTROL-PLANE] API server http://%s:6443, scheduler + node lifecycle active\n", ip.c_str());

        kubelet_cfg.api_server_host = "127.0.0.1";
        kubelet_cfg.api_server_port = 6443;
        kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        kubelet->start();

        kube32::app::AppWiringOptions aw;
        aw.app_name = "tessera";
        aw.core_pod = "tessera-core";
        aw.local_node_name = ident.name;
        aw.store = store.get();
        aw.run_ingress = true;
        aw.ingress_port = 80;
        aw.ingress_workers = ident.has_psram ? 2 : 1;
        aw.ingress_worker_stack = ident.has_psram ? 98304 : 8192;   // local pod calls run the interpreter here
        aw.ingress_rx_buffer = ident.has_psram ? 4096 : 2560;
        // The ingress calls a local request-handling pod directly; the pod
        // HTTP server only matters on worker nodes that host such pods. Not
        // starting it here keeps 64 KB of PSRAM for the pod runtime itself.
        aw.run_pod_server = false;
        aw.pod_workers = 1;
        aw.pod_worker_stack = 65536;
        aw.pod_port = 7000;
        aw.core_affinity = 1;
        aw.stacks_in_psram = ident.has_psram;
        aw.cluster_events_from_hub = false;
        wiring = std::make_unique<kube32::app::AppWiring>(aw);
        wiring->start();
    } else {
        kubelet_cfg.api_server_host = api_host;
        kubelet_cfg.api_server_port = 6443;
        // The control plane may sit on another access point: reach it over UDP too
        kube32::ipc::ChannelManager::instance().add_seed_peer(api_host, 7400);
        kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        kubelet->start();
        printf("[WORKER] kubelet -> http://%s:6443\n", api_host.c_str());

        kube32::app::AppWiringOptions aw;
        aw.app_name = "tessera";
        aw.core_pod = "tessera-core";
        aw.local_node_name = ident.name;
        aw.run_pod_server = ident.has_psram;
        aw.pod_port = 7000;
        aw.pod_workers = 2;
        aw.pod_worker_stack = 24576;
        aw.core_affinity = 1;
        aw.stacks_in_psram = ident.has_psram;
        wiring = std::make_unique<kube32::app::AppWiring>(aw);
        wiring->start();
        if (has_display) {
            kube32::hal::display_fill_rect(0, 0, kube32::hal::LCD_WIDTH, 18, 0x194B);
            kube32::hal::display_draw_string(6, 5, "KUBE32  worker node", kube32::hal::COLOR_GREEN, 0x194B);
            std::string l = "node " + ident.name + "  " + ip;
            kube32::hal::display_draw_string(6, 28, l.c_str(), kube32::hal::COLOR_WHITE, kube32::hal::COLOR_BG_DARK);
            std::string app_url = "app  http://" + api_host + "/";
            kube32::hal::display_draw_string(6, 40, app_url.c_str(), kube32::hal::COLOR_CYAN, kube32::hal::COLOR_BG_DARK);
            kube32::hal::display_draw_string(6, 52, "waiting for pods...", kube32::hal::COLOR_WHITE, kube32::hal::COLOR_BG_DARK);
        }
    }
    log_memory("cluster services up");

    // Console on a small task of its own
    console.init(ident.has_display);   // USB-Serial-JTAG on the display board, UART0 (CH340) on the devkit
    if (!ident.is_control_plane) s_forward_host = api_host;   // workers relay console HTTP to the control plane
    if (wiring) s_ingress_server = wiring->ingress_server();
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

    // Housekeeping: stats line for the display pod, memory log
    uint32_t iter = 0;
    uint64_t last_req = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        iter++;
        uint64_t now = kube32::osal::System::get_uptime_ms();
        if (ident.is_control_plane && wiring && wiring->ingress_server()) {
            uint64_t reqs = wiring->ingress_server()->metrics().requests.load();
            uint32_t rps = static_cast<uint32_t>(reqs - last_req);
            last_req = reqs;
            char ev[160];
            int l = snprintf(ev, sizeof(ev), "{\"t\":\"stats\",\"rps\":%u,\"viewers\":%u,\"heap_kb\":%u,\"uptime_s\":%u}",
                             rps, (unsigned)wiring->ingress()->sse().count(),
                             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024), (unsigned)(now / 1000));
            kube32::ipc::ChannelManager::instance().publish("ingress", "tessera.events", reinterpret_cast<const uint8_t*>(ev), static_cast<size_t>(l));
            if (iter % 2 == 0 && wiring->ingress()->sse().count() > 0) {
                wiring->ingress()->sse().broadcast("cluster", wiring->ingress()->build_cluster_json());
            }
        }
        if (ident.is_control_plane && store && iter % 10 == 0) {
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
        }
        if (iter % 30 == 0) log_memory("steady state");
        if (iter < 180 && iter % 5 == 0 && wiring) {
            uint32_t age = 0;
            std::string cur = kube32::runtime::AppPodHost::instance().in_flight(age);
            auto* ing = wiring->ingress_server();
            printf("[DIAG] t=%us ingress{req=%llu active=%u queued=%u err=%llu} inflight='%s' age=%ums heap=%u\n",
                   iter, ing ? (unsigned long long)ing->metrics().requests.load() : 0ull,
                   ing ? (unsigned)ing->metrics().active.load() : 0u, ing ? (unsigned)ing->queued() : 0u,
                   ing ? (unsigned long long)ing->metrics().errors.load() : 0ull,
                   cur.c_str(), (unsigned)age, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }
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
