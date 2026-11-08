// Kube32 firmware entry point.
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
#include "controller/device_controller.hpp"
#include "controller/checkpoint_controller.hpp"
#include "api/firmware_api.hpp"
#include "hal/firmware_slots.hpp"
#include "storage/blob_store.hpp"
#include "mem/function_ref.hpp"
#include "mem/large_blocks.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/i2c_discovery.hpp"
#include "models/device.hpp"
#include "kubelet/metrics_provider.hpp"
#include "esp_sntp.h"
#if __has_include("soc/rtc_cntl_reg.h")
#include "soc/rtc_cntl_reg.h"
#endif
#if __has_include("soc/lp_system_reg.h")
#include "soc/lp_system_reg.h"
#endif
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
#include "app/egress_gateway.hpp"
#include "app/node_storage_api.hpp"
#include "app/node_blobs_api.hpp"
#include "app/pod_checkpoint_api.hpp"
#include "api/checkpoint_api.hpp"
#include "app/console_relay.hpp"
#include "app/improv_serial.hpp"
#include "api/version_info.hpp"
#include "api/node_proxy.hpp"
#include "kubelet/kube_api_client.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"
#include "osal/socket.hpp"
#include "hal/board.hpp"
#if CONFIG_KUBE32_CONSOLE_USB_OTG
#include "hal/usb_otg_console.hpp"
#endif
#include "hal/sd_card.hpp"
#include "hal/node_storage.hpp"
#include "storage/flash_log.hpp"
#include "api/timestamp_backfill.hpp"
#include "osal/tls.hpp"
#include "runtime/pod_memory_pool.h"
#include "runtime/pod_log.hpp"
#include "models/kube_models.hpp"
#include "bench/bench_kernels.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
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
#include "hal/usb_serial_jtag_ll.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"

static const char* TAG = "Kube32";
static kube32::hal::Board* s_board = nullptr;   // on-board peripherals, nullptr on a bare board

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
    kube32::hal::Board* board{nullptr};   // on-board peripherals of the board profile
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
    if (board[0]) {
        id.board = kube32::hal::board_for(board);
        id.has_display = id.board != nullptr;
    }
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

// Reads one number from the "k32" NVS namespace; `fallback` when absent.
uint32_t read_nvs_u32(const char* key, uint32_t fallback) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READONLY, &h) != ESP_OK) return fallback;
    uint32_t v = fallback;
    if (nvs_get_u32(h, key, &v) != ESP_OK) v = fallback;
    nvs_close(h);
    return v;
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
    bool use_otg{false};
    void init(bool usb_only) {
#if CONFIG_KUBE32_CONSOLE_USB_OTG
        use_otg = kube32::hal::usb_otg_console::start();
        if (use_otg) {
            init_uart(usb_only);
            return;
        }
#endif
        if (!usb_serial_jtag_is_driver_installed()) {
            // A reset other than power-on keeps the USB link of the boot
            // before, and the clock changes at app start can leave it
            // wedged: the host then receives nothing until the board is
            // unplugged. Drop off the bus for a moment (D+ and D- pulled
            // low) so the host enumerates the port again, as after a replug.
            if (esp_reset_reason() != ESP_RST_POWERON) {
                usb_serial_jtag_pull_override_vals_t pulls = {};
                pulls.dp_pd = true;
                pulls.dm_pd = true;
                usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
                vTaskDelay(pdMS_TO_TICKS(100));
                // Back to the reset values (D+ pull-up on) before the PHY
                // takes the pulls over again: it keeps the register's bits
                pulls = {};
                pulls.dp_pu = true;
                usb_serial_jtag_ll_phy_enable_pull_override(&pulls);
                usb_serial_jtag_ll_phy_disable_pull_override();
            }
            // The early console flushes the TX FIFO only at a newline. A
            // partial line left in it keeps the FIFO from ever emptying, and
            // the driver sends nothing until it has emptied once: the port
            // stays silent until a power cycle. Send whatever is pending first.
            usb_serial_jtag_ll_txfifo_flush();
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
        init_uart(usb_only);
    }
    void init_uart(bool usb_only) {
        uart_enabled = !usb_only;
        if (uart_enabled) {
            if (!uart_is_driver_installed(UART_NUM_0)) {
                // Small rings on a board without PSRAM, whose internal RAM pods need
                const int ring = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0 ? 8192 : 1024;
                uart_driver_install(UART_NUM_0, ring, ring, 0, nullptr, 0);
            }
            // Route printf/ESP_LOG through the driver so a log line and a protocol
            // frame can never interleave byte-wise.
            uart_vfs_dev_use_driver(UART_NUM_0);
        }
    }
    int usb_read(char* buf, size_t cap, uint32_t timeout_ms) {
#if CONFIG_KUBE32_CONSOLE_USB_OTG
        if (use_otg) return kube32::hal::usb_otg_console::read(buf, cap, timeout_ms);
#endif
        return usb_serial_jtag_read_bytes(buf, cap, pdMS_TO_TICKS(timeout_ms));
    }
    // Waits on the port that carried the last bytes and only polls the other.
    // The USB-Serial/JTAG driver drops what arrives while its receive ring
    // (4 KiB) is full, so a wait on UART0 while a request streams in over USB
    // would lose part of it; UART0's own 8 KiB ring holds a line meanwhile.
    int read(char* buf, size_t cap, uint32_t timeout_ms) {
        if (!uart_enabled) return usb_read(buf, cap, timeout_ms);
        const TickType_t wait = pdMS_TO_TICKS(timeout_ms);
        int n = usb_read(buf, cap, last_from_usb ? timeout_ms : 0);
        if (n > 0) { last_from_usb = true; return n; }
        n = uart_read_bytes(UART_NUM_0, buf, cap, last_from_usb ? 0 : wait);
        if (n > 0) { last_from_usb = false; return n; }
        if (!last_from_usb) {
            n = usb_read(buf, cap, 0);
            if (n > 0) last_from_usb = true;
        }
        return n;
    }
    void write(const char* data, size_t len) {
#if CONFIG_KUBE32_CONSOLE_USB_OTG
        if (use_otg && (last_from_usb || !uart_enabled)) {
            kube32::hal::usb_otg_console::write(data, len);
            return;
        }
#endif
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
#if CONFIG_KUBE32_CONSOLE_USB_OTG
        if (use_otg && (last_from_usb || !uart_enabled)) {
            return kube32::hal::usb_otg_console::write_frame(data, len, timeout_ms);
        }
#endif
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
static kube32::runtime::WasmRuntime* s_runtime = nullptr;     // the containers K32_STATUS counts
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

// Runs a console command on a task of its own, for commands that go deeper
// than the console task's stack (file systems, drivers): the stack exists only
// while the command runs
static void console_run_deep(Console& con, kube32::mem::FunctionRef<void()> fn, uint32_t stack_bytes = 6144) {
    struct Job {
        kube32::mem::FunctionRef<void()>* fn;
        SemaphoreHandle_t done;
    } job{&fn, xSemaphoreCreateBinary()};
    auto entry = [](void* arg) {
        Job* j = static_cast<Job*>(arg);
        (*j->fn)();
        xSemaphoreGive(j->done);
        vTaskDelete(nullptr);
    };
    if (!job.done || xTaskCreatePinnedToCore(entry, "k32_con_job", stack_bytes, &job, 10, nullptr, 0) != pdPASS) {
        if (job.done) vSemaphoreDelete(job.done);
        con.write("K32_ERR no memory for the command's stack\n", 41);
        return;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
    vSemaphoreDelete(job.done);
}

// The node's blob stores and what each holds
static void console_blobs(Console& con) {
    for (kube32::storage::BlobStore* store : {static_cast<kube32::storage::BlobStore*>(kube32::storage::node_flash_blobs()),
                                              kube32::storage::node_card_blobs()}) {
        if (!store) continue;
        std::string why;
        const bool up = store->available(&why);
        char l[200];
        int ll = snprintf(l, sizeof(l), "K32_BLOBS medium=%s location=\"%s\" available=%d capacity=%llu free=%llu%s%s\n", store->medium(),
                          store->location().c_str(), up ? 1 : 0, (unsigned long long)store->capacity_bytes(),
                          (unsigned long long)(up ? store->free_bytes() : 0), up ? "" : " reason=", up ? "" : why.c_str());
        con.write(l, static_cast<size_t>(ll));
        for (const auto& b : store->list()) {
            ll = snprintf(l, sizeof(l), "K32_BLOB %s %s size=%llu offset=%llu crc32=0x%08x\n", store->medium(), b.name.c_str(),
                          (unsigned long long)b.size, (unsigned long long)b.offset, (unsigned)b.crc32);
            con.write(l, static_cast<size_t>(ll));
        }
    }
    con.write("K32_OK blobs\n", 13);
}

// Reads a blob natively, start to end: a sum of its 32-bit words through the
// mapping, or file reads of <chunk> bytes (512 to 16384, default 4096) into a
// buffer. args: <name>[:<chunk>]
static void console_blobread(Console& con, std::string args) {
    std::string name = args;
    uint32_t chunk = 4096;
    if (size_t colon = name.find(':'); colon != std::string::npos) {
        chunk = static_cast<uint32_t>(std::strtoul(name.c_str() + colon + 1, nullptr, 10));
        name.resize(colon);
    }
    chunk = std::max<uint32_t>(512, std::min<uint32_t>(chunk, 16384));
    std::shared_ptr<kube32::storage::BlobReader> r;
    const char* medium = "";
    for (kube32::storage::BlobStore* store : kube32::storage::node_blob_stores()) {
        int32_t err = 0;
        r = store->open(name, err);
        if (r) { medium = store->medium(); break; }
    }
    char l[160];
    int ll;
    if (!r) {
        ll = snprintf(l, sizeof(l), "K32_ERR no blob %s\n", name.c_str());
    } else {
        uint32_t sum = 0;
        uint64_t bytes = 0;
        const int64_t t0 = esp_timer_get_time();
        if (r->view()) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(r->view());
            const size_t words = static_cast<size_t>(r->size() / 4);
            for (size_t i = 0; i < words; ++i) sum += w[i];
            bytes = words * 4;
        } else if (uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA))) {
            for (uint64_t off = 0; off < r->size();) {
                const int32_t n = r->read(off, buf, chunk);
                if (n <= 0) break;
                for (int32_t i = 0; i + 4 <= n; i += 4) sum += *reinterpret_cast<const uint32_t*>(buf + i);
                off += static_cast<uint64_t>(n);
                bytes += static_cast<uint64_t>(n);
            }
            heap_caps_free(buf);
        }
        const int64_t us = esp_timer_get_time() - t0;
        ll = snprintf(l, sizeof(l), "K32_BLOBREAD %s %s %s chunk=%u bytes=%llu us=%lld kB_per_s=%llu sum=%08x\n", name.c_str(),
                      medium, r->view() ? "mapped" : "read", r->view() ? 0u : (unsigned)chunk, (unsigned long long)bytes,
                      (long long)us, (unsigned long long)(us > 0 ? bytes * 1000 / static_cast<uint64_t>(us) : 0), (unsigned)sum);
    }
    con.write(l, static_cast<size_t>(ll));
}

// Records the microphone for args ms (100..10000) and prints its level per
// 250 ms: the mean (DC offset), the RMS and the peak around that mean
static void console_mic(Console& con, const std::string& args) {
    if (!s_board) {
        con.write("K32_ERR no microphone\n", 22);
        return;
    }
    int ms = std::atoi(args.c_str());
    if (ms < 100) ms = 100;
    if (ms > 10000) ms = 10000;
    int16_t* buf = static_cast<int16_t*>(heap_caps_malloc(500 * sizeof(int16_t), MALLOC_CAP_INTERNAL));
    if (!buf) {
        con.write("K32_ERR no memory\n", 18);
        return;
    }
    const uint32_t window = 4000;   // 250 ms at 16 kHz
    const uint32_t total = static_cast<uint32_t>(ms) * 16;
    uint32_t got_total = 0, in_window = 0, w = 0;
    int64_t sum = 0, sumsq = 0, wsum = 0, wsumsq = 0;
    int32_t wmin = 32767, wmax = -32768;
    int32_t err = 0;
    char l[160];
    // The decimator settles in its first few milliseconds: not counted
    const int32_t first = s_board->mic_read(buf, 500, 200);
    if (first <= 0) {
        char e[64];
        con.write(e, static_cast<size_t>(snprintf(e, sizeof(e), "K32_MIC first read: %d\n", (int)first)));
    }
    while (got_total < total) {
        const int32_t n = s_board->mic_read(buf, std::min<uint32_t>(500, total - got_total), 200);
        if (n <= 0) { err = n; break; }
        for (int32_t i = 0; i < n; ++i) {
            const int32_t v = buf[i];
            wsum += v;
            wsumsq += static_cast<int64_t>(v) * v;
            wmin = std::min(wmin, v);
            wmax = std::max(wmax, v);
            if (++in_window == window) {
                const double mean = static_cast<double>(wsum) / window;
                const double var = static_cast<double>(wsumsq) / window - mean * mean;
                const double peak = std::max(wmax - mean, mean - wmin);
                int ll = snprintf(l, sizeof(l), "K32_MIC window=%u mean=%.1f rms=%.1f peak=%.0f\n", (unsigned)w++, mean,
                                  var > 0 ? std::sqrt(var) : 0.0, peak);
                con.write(l, static_cast<size_t>(ll));
                sum += wsum;
                sumsq += wsumsq;
                wsum = wsumsq = 0;
                wmin = 32767;
                wmax = -32768;
                in_window = 0;
            }
        }
        got_total += static_cast<uint32_t>(n);
    }
    s_board->mic_read(buf, 0, 0);
    const uint32_t counted = got_total - in_window;
    const double mean = counted ? static_cast<double>(sum) / counted : 0.0;
    const double var = counted ? static_cast<double>(sumsq) / counted - mean * mean : 0.0;
    int ll = err ? snprintf(l, sizeof(l), "K32_ERR mic error=%d after %u samples\n", (int)err, (unsigned)got_total)
                 : snprintf(l, sizeof(l), "K32_OK mic samples=%u mean=%.1f rms=%.1f\n", (unsigned)got_total, mean,
                            var > 0 ? std::sqrt(var) : 0.0);
    con.write(l, static_cast<size_t>(ll));
    heap_caps_free(buf);
}

// The kernels the bench pod runs, run natively on the console task, for
// comparison with the pod's timings
static void console_bench(Console& con, const std::string& args) {
    char l[160];
    int ll;
    const size_t c1 = args.find(':');
    const std::string kernel = args.substr(0, c1);
    const size_t c2 = c1 == std::string::npos ? std::string::npos : args.find(':', c1 + 1);
    const uint32_t n = c1 == std::string::npos ? 1 : static_cast<uint32_t>(std::strtoul(args.c_str() + c1 + 1, nullptr, 10));
    const uint32_t runs = c2 == std::string::npos ? 1 : static_cast<uint32_t>(std::strtoul(args.c_str() + c2 + 1, nullptr, 10));
    if (kernel == "hostcall") {
        volatile int64_t sink = 0;
        const int64_t t0 = esp_timer_get_time();
        for (uint32_t i = 0; i < n; i++) sink = sink + esp_timer_get_time();
        ll = snprintf(l, sizeof(l), "K32_BENCH hostcall calls=%u us=%lld\n", (unsigned)n, (long long)(esp_timer_get_time() - t0));
        con.write(l, static_cast<size_t>(ll));
        return;
    }
    const int index = bench_kernel_index(kernel.data(), static_cast<uint32_t>(kernel.size()));
    if (index < 0) {
        con.write("K32_ERR bench kernel\n", 21);
        return;
    }
    // The workspace only while the kernel runs
    std::unique_ptr<uint32_t[]> work(new (std::nothrow) uint32_t[BENCH_WORK_BYTES / 4]);
    if (!work) {
        con.write("K32_ERR bench workspace\n", 24);
        return;
    }
    int64_t total = 0, best = INT64_MAX;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < (runs ? runs : 1); i++) {
        const int64_t t0 = esp_timer_get_time();
        sum = bench_kernel_run(index, n, work.get());
        const int64_t us = esp_timer_get_time() - t0;
        total += us;
        if (us < best) best = us;
    }
    ll = snprintf(l, sizeof(l), "K32_BENCH kernel=%s n=%u runs=%u us_min=%lld us_total=%lld sum=%u\n", bench_kernel_name(index), (unsigned)n,
                  (unsigned)(runs ? runs : 1), (long long)best, (long long)total, (unsigned)sum);
    con.write(l, static_cast<size_t>(ll));
}

#if CONFIG_HEAP_USE_HOOKS
// Heap watch (CONFIG_HEAP_USE_HOOKS builds): while armed, every internal
// allocation is recorded with the task that made it, until it is freed. The
// moment the recorded bytes peak (the heap's low point when the watch
// started at a quiet moment) is kept per task, with a size histogram, for
// K32_CMD:HEAPWATCH:dump. The table exists only while armed; disarmed, a
// hook is one load and a branch.
namespace heapwatch {
constexpr size_t kSlots = 640;
constexpr size_t kTasks = 24;
constexpr uintptr_t kTomb = 1;
struct Slot { uintptr_t ptr; uint16_t size; uint8_t task; };
struct TaskRow { TaskHandle_t h; char name[16]; int32_t bytes, count; int16_t bucket[4]; };
struct Watch {
    Slot slots[kSlots];
    TaskRow tasks[kTasks], peak[kTasks];
    size_t ntasks, npeak;
    int32_t live, peak_live, dropped;
    int64_t peak_us;
};
Watch* volatile s_watch = nullptr;
Watch s_storage;   // in .bss: arming needs no block of this size from a fragmented heap
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static IRAM_ATTR inline int bucket_of(size_t n) { return n <= 128 ? 0 : n <= 512 ? 1 : n <= 1700 ? 2 : 3; }
static IRAM_ATTR inline size_t hash_of(uintptr_t p) { return (p >> 3) * 2654435761u % kSlots; }
}  // namespace heapwatch

extern "C" IRAM_ATTR void esp_heap_trace_alloc_hook(void* ptr, size_t size, uint32_t caps) {
    using namespace heapwatch;
    Watch* w = s_watch;
    if (!w || !ptr || !esp_ptr_internal(ptr)) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    if ((w = s_watch) == nullptr) { portEXIT_CRITICAL_SAFE(&s_mux); return; }
    TaskHandle_t h = xTaskGetCurrentTaskHandle();
    size_t t = 0;
    while (t < w->ntasks && w->tasks[t].h != h) ++t;
    if (t == w->ntasks && t < kTasks) {
        TaskRow& r = w->tasks[w->ntasks++];
        r.h = h;
        const char* n = h ? pcTaskGetName(h) : "-";
        size_t i = 0;
        for (; i < sizeof(r.name) - 1 && n[i]; ++i) r.name[i] = n[i];
        r.name[i] = 0;
    }
    size_t i = hash_of(reinterpret_cast<uintptr_t>(ptr)), probes = 0;
    while (w->slots[i].ptr > kTomb && probes < kSlots) { i = (i + 1) % kSlots; ++probes; }
    if (t < kTasks && probes < kSlots) {
        w->slots[i] = Slot{reinterpret_cast<uintptr_t>(ptr), static_cast<uint16_t>(size > 65535 ? 65535 : size), static_cast<uint8_t>(t)};
        TaskRow& r = w->tasks[t];
        r.bytes += static_cast<int32_t>(size);
        r.count++;
        r.bucket[bucket_of(size)]++;
        w->live += static_cast<int32_t>(size);
        if (w->live > w->peak_live) {
            w->peak_live = w->live;
            w->peak_us = esp_timer_get_time();
            for (size_t k = 0; k < w->ntasks; ++k) w->peak[k] = w->tasks[k];
            w->npeak = w->ntasks;
        }
    } else {
        w->dropped++;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

extern "C" IRAM_ATTR void esp_heap_trace_free_hook(void* ptr) {
    using namespace heapwatch;
    Watch* w = s_watch;
    if (!w || !ptr) return;
    portENTER_CRITICAL_SAFE(&s_mux);
    if ((w = s_watch) == nullptr) { portEXIT_CRITICAL_SAFE(&s_mux); return; }
    size_t i = hash_of(reinterpret_cast<uintptr_t>(ptr));
    for (size_t probes = 0; probes < kSlots && w->slots[i].ptr; ++probes, i = (i + 1) % kSlots) {
        if (w->slots[i].ptr != reinterpret_cast<uintptr_t>(ptr)) continue;
        TaskRow& r = w->tasks[w->slots[i].task];
        r.bytes -= w->slots[i].size;
        r.count--;
        r.bucket[bucket_of(w->slots[i].size)]--;
        w->live -= w->slots[i].size;
        w->slots[i].ptr = kTomb;
        break;
    }
    portEXIT_CRITICAL_SAFE(&s_mux);
}

// A heap-hooks build arms the watch at boot, after Wi-Fi: K32_CMD:HEAPWATCH:live
// then shows what the cluster services and the first pods keep
static void heapwatch_arm_at_boot() {
    using namespace heapwatch;
    std::memset(&s_storage, 0, sizeof(s_storage));
    s_watch = &s_storage;
}

static void console_heapwatch(Console& con, const std::string& arg) {
    using namespace heapwatch;
    char l[160];
    int ll;
    if (arg == "on") {
        if (!s_watch) {
            std::memset(&s_storage, 0, sizeof(s_storage));
            s_watch = &s_storage;
        }
        heap_caps_monitor_local_minimum_free_size_start();   // min in the dump: since now
        ll = snprintf(l, sizeof(l), "K32_OK heapwatch on (%u bytes)\n", (unsigned)sizeof(Watch));
        con.write(l, static_cast<size_t>(ll));
        return;
    }
    Watch* w = s_watch;
    if (!w) { con.write("K32_ERR heapwatch is off\n", 25); return; }
    // Printed outside the critical section, from a copy of the rows
    struct Rows { TaskRow tasks[kTasks], peak[kTasks]; size_t ntasks, npeak; int32_t live, peak_live, dropped; int64_t peak_us; };
    std::unique_ptr<Rows> rows(new (std::nothrow) Rows);
    if (!rows) { con.write("K32_ERR heapwatch: no memory\n", 29); return; }
    Rows& copy = *rows;
    portENTER_CRITICAL(&s_mux);
    copy.ntasks = w->ntasks; copy.npeak = w->npeak; copy.live = w->live; copy.peak_live = w->peak_live;
    copy.dropped = w->dropped; copy.peak_us = w->peak_us;
    std::memcpy(copy.tasks, w->tasks, sizeof(copy.tasks));
    std::memcpy(copy.peak, w->peak, sizeof(copy.peak));
    portEXIT_CRITICAL(&s_mux);
    ll = snprintf(l, sizeof(l), "K32_HEAPWATCH peak %d bytes at %lld ms, now %d, not recorded %d, internal free %u min %u\n", (int)copy.peak_live,
                  (long long)(copy.peak_us / 1000), (int)copy.live, (int)copy.dropped,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    con.write(l, static_cast<size_t>(ll));
    for (size_t k = 0; k < copy.ntasks; ++k) {
        const TaskRow& p = k < copy.npeak ? copy.peak[k] : TaskRow{};
        const TaskRow& c = copy.tasks[k];
        ll = snprintf(l, sizeof(l), "K32_HEAPWATCH %-16s at_peak %6d in %3d (<=128 %d, <=512 %d, <=1700 %d, more %d)  now %6d in %3d\n", c.name,
                      (int)p.bytes, (int)p.count, p.bucket[0], p.bucket[1], p.bucket[2], p.bucket[3], (int)c.bytes, (int)c.count);
        con.write(l, static_cast<size_t>(ll));
    }
    if (arg == "live") {
        // Every recorded allocation still held, with its task: what was
        // allocated since the watch began and outlives its user (in the
        // table's order; sort by address on the host)
        for (size_t i = 0; i < kSlots; ++i) {
            portENTER_CRITICAL(&s_mux);
            const Slot sl = w->slots[i];
            portEXIT_CRITICAL(&s_mux);
            if (sl.ptr <= kTomb) continue;
            ll = snprintf(l, sizeof(l), "K32_HEAPWATCH_LIVE 0x%08x %5u %s\n", (unsigned)sl.ptr, (unsigned)sl.size,
                          sl.task < copy.ntasks ? copy.tasks[sl.task].name : "?");
            con.write(l, static_cast<size_t>(ll));
        }
    }
    if (arg == "off") {
        portENTER_CRITICAL(&s_mux);   // a hook on the other core finishes first
        s_watch = nullptr;
        portEXIT_CRITICAL(&s_mux);
        heap_caps_monitor_local_minimum_free_size_stop();
    }
    con.write("K32_OK heapwatch\n", 17);
}
#endif

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

// Networks in range, at most `max`. The cluster access point alone cannot
// scan, so the radio adds its station interface first (it stays up, idle). A
// station still looking for its network refuses to scan: it stops looking
// for the scan and joins again afterwards.
static esp_err_t scan_networks(std::vector<wifi_ap_record_t>& out, uint16_t max) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_AP) esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    esp_err_t scan = esp_wifi_scan_start(&sc, true);
    bool rejoin = false;
    for (int tries = 0; scan == ESP_ERR_WIFI_STATE && tries < 5; ++tries) {
        kube32::net::hold_station_rejoin(true);
        esp_wifi_disconnect();
        rejoin = true;
        vTaskDelay(pdMS_TO_TICKS(300));
        scan = esp_wifi_scan_start(&sc, true);
    }
    if (scan == ESP_OK) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n > max) n = max;
        // Reading the records, none included, frees the driver's scan list
        out.resize(n ? n : 1);
        if (esp_wifi_scan_get_ap_records(&n, out.data()) != ESP_OK) n = 0;
        out.resize(n);
    }
    if (rejoin) {
        kube32::net::hold_station_rejoin(false);
        esp_wifi_connect();
    }
    return scan;
}

// The station's address while it is associated, "" otherwise
static std::string station_ip() {
    wifi_ap_record_t ap;
    esp_netif_ip_info_t info;
    esp_netif_t* nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK || !nif || esp_netif_get_ip_info(nif, &info) != ESP_OK || info.ip.addr == 0) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return buf;
}

static std::atomic<uint32_t> s_join_ip{0};
// K32_CMD:THROW: the housekeeping loop raises std::bad_alloc on its next pass
static std::atomic<bool> s_cluster_throw{false};

// Joins `ssid` as a station beside the cluster access point and waits up to
// `timeout_ms` for an address. Returns the address, or "" after putting the
// station back on the network it had.
static std::string join_station(const std::string& ssid, const std::string& pass, uint32_t timeout_ms) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    const bool had_station = mode != WIFI_MODE_AP;
    wifi_config_t before = {};
    if (had_station) esp_wifi_get_config(WIFI_IF_STA, &before);
    else esp_wifi_set_mode(WIFI_MODE_APSTA);
    // The configuration the node's network manager uses: the strongest
    // access point with that name
    wifi_config_t cfg = {};
    std::memcpy(cfg.sta.ssid, ssid.data(), std::min(ssid.size(), sizeof(cfg.sta.ssid)));
    std::memcpy(cfg.sta.password, pass.data(), std::min(pass.size(), sizeof(cfg.sta.password)));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.failure_retry_cnt = 2;
    // The station takes a configuration once it has left its network
    auto configure = [](wifi_config_t& c) {
        kube32::net::hold_station_rejoin(true);
        esp_wifi_disconnect();
        for (int i = 0; esp_wifi_set_config(WIFI_IF_STA, &c) == ESP_ERR_WIFI_STATE && i < 30; ++i) vTaskDelay(pdMS_TO_TICKS(100));
        kube32::net::hold_station_rejoin(false);
    };
    esp_event_handler_t got_ip = [](void*, esp_event_base_t, int32_t, void* data) {
        s_join_ip.store(static_cast<ip_event_got_ip_t*>(data)->ip_info.ip.addr);
    };
    s_join_ip.store(0);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, nullptr);
    configure(cfg);
    // Joins again after a failed attempt (a station beside the access point
    // alone is not rejoined by the network manager)
    for (uint32_t waited = 0; s_join_ip.load() == 0 && waited < timeout_ms; waited += 250) {
        if (waited % 4000 == 0) esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip);
    const uint32_t addr = s_join_ip.load();
    if (addr != 0) {
        esp_ip4_addr_t ip = {addr};
        char buf[16];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip));
        return buf;
    }
    if (had_station) {
        configure(before);
        esp_wifi_connect();
    } else {
        esp_wifi_disconnect();
    }
    return "";
}

void write_nvs_wifi(const std::string& ssid, const std::string& pass) {
    nvs_handle_t h;
    if (nvs_open("k32", NVS_READWRITE, &h) != ESP_OK) return;
    if (ssid.empty()) { nvs_erase_key(h, "wifi_ssid"); nvs_erase_key(h, "wifi_pass"); }
    else { nvs_set_str(h, "wifi_ssid", ssid.c_str()); nvs_set_str(h, "wifi_pass", pass.c_str()); }
    nvs_commit(h);
    nvs_close(h);
}

// Improv Wi-Fi over the console (include/app/improv_serial.hpp): a browser
// installer asks for the node's state and information, scans, and sends
// Wi-Fi credentials. The node joins the network beside its cluster access
// point, answers with the API server's URL (a control plane), stores the
// credentials and restarts on that network, as after K32_CMD:WIFI.
static void improv_send(Console& con, const std::string& packet) {
    // A client reads a packet only at the start of a line
    const std::string out = "\n" + packet;
    con.write(out.data(), out.size());
}

static void improv_command(Console& con, const NodeIdentity& ident, const std::string& packet) {
    namespace improv = kube32::app::improv;
    improv::Rpc rpc;
    const improv::Error err = improv::parse_rpc(reinterpret_cast<const uint8_t*>(packet.data()), packet.size(), rpc);
    if (err != improv::kNoError) {
        improv_send(con, improv::error_packet(err));
        return;
    }
    auto urls = [&ident](const std::string& ip) {
        return ident.is_control_plane ? std::vector<std::string>{"http://" + ip + ":6443"} : std::vector<std::string>{};
    };
    switch (rpc.command) {
    case improv::kCurrentStateRequest: {
        // Provisioned: joined to the network the node has credentials for
        std::string ssid, pass;
        const std::string ip = read_nvs_wifi(ssid, pass) ? station_ip() : "";
        improv_send(con, improv::state_packet(ip.empty() ? improv::kReady : improv::kProvisioned));
        if (!ip.empty()) improv_send(con, improv::rpc_result(improv::kCurrentStateRequest, urls(ip)));
        break;
    }
    case improv::kDeviceInfo:
        improv_send(con, improv::rpc_result(improv::kDeviceInfo, {"kube32", kube32::api::firmware_version(), kube32::hal::chip_display_name(), ident.name}));
        break;
    case improv::kScanNetworks: {
        // One result per network name, the strongest first; an empty one ends the list
        std::vector<wifi_ap_record_t> aps;
        scan_networks(aps, 20);
        for (size_t i = 0; i < aps.size(); ++i) {
            const char* name = reinterpret_cast<const char*>(aps[i].ssid);
            bool seen = !name[0];
            for (size_t k = 0; k < i && !seen; ++k) seen = std::strcmp(name, reinterpret_cast<const char*>(aps[k].ssid)) == 0;
            if (seen) continue;
            improv_send(con, improv::rpc_result(improv::kScanNetworks,
                                                {name, std::to_string(aps[i].rssi), aps[i].authmode == WIFI_AUTH_OPEN ? "NO" : "YES"}));
        }
        improv_send(con, improv::rpc_result(improv::kScanNetworks, {}));
        break;
    }
    case improv::kWifiSettings: {
        improv_send(con, improv::state_packet(improv::kProvisioning));
        const std::string ip = join_station(rpc.ssid, rpc.password, 20000);
        if (ip.empty()) {
            improv_send(con, improv::error_packet(improv::kUnableToConnect));
            improv_send(con, improv::state_packet(improv::kReady));
            break;
        }
        write_nvs_wifi(rpc.ssid, rpc.password);
        improv_send(con, improv::state_packet(improv::kProvisioned));
        improv_send(con, improv::rpc_result(improv::kWifiSettings, urls(ip)));
        printf("[NET] joined %s at %s: wifi stored, rebooting\n", rpc.ssid.c_str(), ip.c_str());
        // The installer shows the address before a restart on the native USB
        // port drops its connection
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
        break;
    }
    default:
        improv_send(con, improv::error_packet(improv::kUnknownRpc));
    }
}

// K32_CMD:IPC: the channel transport's counters and settings, then one line
// a peer: how it is reached and how many topics it subscribes to. Runs on a
// stack of its own (console_run_deep): printf of 64-bit values is deep
static void console_ipc(Console& con) {
    auto& m = kube32::ipc::ChannelManager::instance();
    auto& s = m.stats();
    char l[480];
    int ll = snprintf(l, sizeof(l),
                      "K32_IPC tx_frames=%llu send_errors=%llu retried=%llu published=%llu publish_errors=%llu fanout_skipped=%llu "
                      "rx_frames=%llu crc=%llu rx_messages=%llu reassembled=%llu incomplete=%llu nomem=%llu rxq_full=%llu malformed=%llu "
                      "dispatch_dropped=%llu subs_rx=%llu paced_ms=%llu held=%u budget=%u rx_rate=%u gap_us=%u rxq=%u route=%u\n",
                      (unsigned long long)s.frames_sent.load(), (unsigned long long)s.send_errors.load(), (unsigned long long)s.frames_retried.load(),
                      (unsigned long long)s.msgs_published.load(), (unsigned long long)s.publish_errors.load(), (unsigned long long)s.fanout_skipped.load(),
                      (unsigned long long)s.frames_received.load(), (unsigned long long)s.crc_errors.load(), (unsigned long long)s.rx_messages.load(),
                      (unsigned long long)s.reassembled_msgs.load(), (unsigned long long)s.rx_incomplete.load(), (unsigned long long)s.rx_nomem.load(),
                      (unsigned long long)s.rx_queue_full.load(), (unsigned long long)s.rx_malformed.load(), (unsigned long long)s.dispatch_dropped.load(),
                      (unsigned long long)s.subs_rx.load(), (unsigned long long)(s.paced_us.load() / 1000), (unsigned)m.rx_held_bytes(),
                      (unsigned)m.rx_budget_bytes(), (unsigned)m.rx_rate_bytes(), (unsigned)m.fragment_gap_us(),
                      (unsigned)m.rx_queue_frames(), (unsigned)m.route());
    con.write(l, static_cast<size_t>(ll));
    const uint64_t now = kube32::osal::System::get_uptime_ms();
    for (const auto& p : m.peers()) {
        ll = snprintf(l, sizeof(l), "K32_IPC_PEER %s espnow_age_ms=%lld udp_age_ms=%lld subs=%u subs_age_ms=%lld\n", p.node_name.c_str(),
                      p.espnow_seen_ms ? (long long)(now - p.espnow_seen_ms) : -1LL, p.udp_seen_ms ? (long long)(now - p.udp_seen_ms) : -1LL,
                      (unsigned)p.subs.size(), p.subs_ms ? (long long)(now - p.subs_ms) : -1LL);
        con.write(l, static_cast<size_t>(ll));
    }
    con.write("K32_OK ipc\n", 11);
}

// Console protocol (line oriented, binary-safe payloads):
//   K32_RESET, K32_OPEN, K32_CLOSE: concurrent streamed HTTP (include/app/console_relay.hpp)
//   K32_HTTP <port> <len> [crc32hex]\n<len bytes of HTTP request> -> K32_RESP <len>\n<bytes>
//   IMPROV...: Improv Wi-Fi serial packets (include/app/improv_serial.hpp)
//   A CRC mismatch answers K32_ERR crc so the gateway can resend.
//   K32_CMD:STATUS                                       -> telemetry line
//   K32_CMD:STORE                                        -> object store journal figures
//   K32_CMD:WIFI:<ssid>:<password>                       -> stored in NVS, reboot
//   K32_CMD:NAME:<node name>|auto                        -> stored in NVS, reboot
//   K32_CMD:TOKEN:<API bearer token>|off                 -> stored in NVS, reboot
//   K32_CMD:APPASS:<cluster access point password>       -> stored in NVS, reboot
//   K32_CMD:APPASS                                       -> K32_APPASS <password>
//   K32_CMD:SYSRESERVE:<size>|auto                       -> PSRAM kept back for the system, stored in NVS, reboot
//   K32_CMD:PODCORES:<1|2>                               -> pod workers on a board with PSRAM, stored in NVS, reboot
//   K32_CMD:CERT                                         -> the API server's TLS certificate (PEM)
//   K32_CMD:FIRMWARE                                     -> the running image and its slot (and what is staged)
//   K32_CMD:BENCH:<kernel>:<n>:<runs>                    -> the benchmark kernels run natively (docs/benchmarks.md)
//   K32_CMD:BENCH:hostcall:<calls>                       -> <calls> native esp_timer_get_time calls
//   K32_CMD:THROW                                        -> the housekeeping loop raises std::bad_alloc once
//   K32_CMD:IPC                                          -> channel counters, settings and peers (K32_IPC lines)
//   K32_CMD:IPCGAP:<us>                                  -> pause between two frames of one message (until reboot)
//   K32_CMD:IPCRXQ:<frames>                              -> ESP-NOW frames waiting for the dispatcher (until reboot)
//   K32_CMD:IPCROUTE:<auto|udp|espnow>                   -> how messages reach peers (until reboot)
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
    // A payload the heap has no room for is read and dropped (`fits` false),
    // so the bytes after it are still read as frames.
    auto read_payload = [&](int& i, int n, size_t len, bool& fits) {
        std::string data;
        fits = true;
        try {
            data.reserve(len);
        } catch (const std::bad_alloc&) {
            fits = false;
        }
        size_t got = 0;
        int j = i + 1;
        while (j < n && got < len) {
            if (fits) data.push_back(rx[j]);
            ++j;
            ++got;
        }
        i = j - 1;
        uint32_t waited = 0;
        while (got < len && waited < 5000) {
            size_t want = len - got;
            int m = con.read(rx, want < sizeof(rx) ? want : sizeof(rx), 100);
            if (m > 0) {
                if (fits) data.append(rx, static_cast<size_t>(m));
                got += static_cast<size_t>(m);
            } else {
                waited += 100;
            }
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
                if (line.size() == kube32::app::improv::kHeaderSize && line == kube32::app::improv::kHeader) {
                    // An Improv packet: binary, its length in the third byte
                    // after the header
                    bool fits = true;
                    std::string packet = line + read_payload(i, n, kube32::app::improv::kPrefixSize - line.size(), fits);
                    if (packet.size() == kube32::app::improv::kPrefixSize) {
                        packet += read_payload(i, n, kube32::app::improv::remaining_after_prefix(reinterpret_cast<const uint8_t*>(packet.data())), fits);
                    }
                    improv_command(con, ident, packet);
                    line.clear();
                }
                continue;
            }
            if (line.empty()) continue;
            if (line == "K32_RESET") {
                s_relay->reset();
            } else if (line.rfind("K32_OPEN ", 0) == 0) {
                unsigned id = 0, port = 0, len = 0, crc = 0;
                if (sscanf(line.c_str() + 9, "%u %u %u %x", &id, &port, &len, &crc) == 4 && len > 0 && len < 64 * 1024) {
                    bool fits = true;
                    std::string req = read_payload(i, n, len, fits);
                    if (fits) s_relay->open(id, static_cast<uint16_t>(port), std::move(req), crc);
                    else s_relay->refuse(id, "503 Service Unavailable\r\nRetry-After: 1");
                }
            } else if (line.rfind("K32_CLOSE ", 0) == 0) {
                s_relay->close(static_cast<uint32_t>(strtoul(line.c_str() + 10, nullptr, 10)));
            } else if (line.rfind("K32_HTTP ", 0) == 0) {
                unsigned port = 0, len = 0, crc = 0;
                int fields = sscanf(line.c_str() + 9, "%u %u %x", &port, &len, &crc);
                if (fields >= 2 && len > 0 && len < 64 * 1024) {
                    bool fits = true;
                    std::string req = read_payload(i, n, len, fits);
                    if (!fits) {
                        con.write("K32_ERR memory\n", 15);
                        line.clear();
                        continue;
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
                    (unsigned)(s_runtime ? s_runtime->container_count() : 0),
                    kube32::hal::storage_medium_name(kube32::hal::node_storage_info().medium), s_api_token_set ? "token" : "off",
                    s_tls ? "on" : "off");
                con.write(buf, static_cast<size_t>(l));
            } else if (line == "K32_CMD:STORE") {
                kube32::storage::PersistenceStats ps;
                char buf[448];
                int l;
                if (s_store && s_store->persistence_stats(ps)) {
                    l = snprintf(buf, sizeof(buf),
                        "K32_STORE journal=%s keys=%u live_bytes=%u capacity_bytes=%u sectors=%u/%u pending=%u records=%llu bytes=%llu moved=%llu via_ram=%llu unchanged=%llu errors=%llu refused_full=%llu refused_no_space=%llu refused_io=%llu refused_oom=%llu last_error=%s revision=%llu\n",
                        s_journal_location.c_str(), (unsigned)ps.live_keys, (unsigned)ps.live_bytes,
                        (unsigned)ps.capacity_bytes, (unsigned)ps.used_sectors, (unsigned)ps.sector_count, (unsigned)ps.pending_keys,
                        (unsigned long long)ps.records_written, (unsigned long long)ps.bytes_written, (unsigned long long)ps.records_moved,
                        (unsigned long long)ps.records_through_ram, (unsigned long long)ps.unchanged_skipped,
                        (unsigned long long)ps.write_errors, (unsigned long long)ps.refused_full,
                        (unsigned long long)ps.refused_no_space, (unsigned long long)ps.refused_io,
                        (unsigned long long)ps.refused_out_of_memory, ps.last_error,
                        (unsigned long long)s_store->current_version());
                } else {
                    l = snprintf(buf, sizeof(buf), "K32_STORE journal=none\n");
                }
                con.write(buf, std::min(static_cast<size_t>(l), sizeof(buf) - 1));
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
                write_nvs_wifi(ssid, pass);
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
                if (!s_board) {
                    con.write("K32_ERR no speaker\n", 19);
                    continue;
                }
                for (int i = 0; i < n; ++i) s_board->tone(880 + (i % 4) * 110, 20, 15);
                char l[64];
                int ll = snprintf(l, sizeof(l), "K32_OK %d tone(s) in %u ms\n", n, (unsigned)(kube32::osal::System::get_uptime_ms() - t0));
                con.write(l, static_cast<size_t>(ll));
            } else if (line.rfind("K32_CMD:ACCESSLOG:", 0) == 0) {
                bool on = line.substr(18) == "on";
                if (s_api_server) s_api_server->set_access_log(on);
                if (s_ingress_server) s_ingress_server->set_access_log(on);
                con.write(on ? "K32_OK access log on\n" : "K32_OK access log off\n", on ? 21 : 22);
            } else if (line.rfind("K32_CMD:SYSRESERVE:", 0) == 0) {
                // PSRAM kept back for the system, from the next boot on:
                // K32_CMD:SYSRESERVE:<size, e.g. 192Ki>, or auto for the default
                const std::string value = line.substr(19);
                const int64_t bytes = value == "auto" ? 0 : kube32::models::parse_quantity(value);
                if (value != "auto" && (bytes < 16 * 1024 || bytes > 1024 * 1024)) {
                    con.write("K32_ERR system reserve must be 16Ki to 1Mi, or auto\n", 52);
                } else {
                    nvs_handle_t h;
                    if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                        if (bytes) nvs_set_u32(h, "sysres", static_cast<uint32_t>(bytes));
                        else nvs_erase_key(h, "sysres");
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    con.write("K32_OK system reserve stored, rebooting\n", 40);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            } else if (line.rfind("K32_CMD:PODCORES:", 0) == 0) {
                // Pod workers from the next boot on: 2 (one a core) or 1
                const std::string value = line.substr(17);
                if (value != "1" && value != "2") {
                    con.write("K32_ERR pod cores must be 1 or 2\n", 32);
                } else {
                    nvs_handle_t h;
                    if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_u32(h, "podcores", static_cast<uint32_t>(value[0] - '0'));
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    con.write("K32_OK pod cores stored, rebooting\n", 35);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
            } else if (line == "K32_CMD:BOOTLOADER") {
                // Reboot into the ROM serial bootloader so a host can flash with
                // "--before no-reset" when the USB auto-reset does not work.
#if defined(RTC_CNTL_FORCE_DOWNLOAD_BOOT) || defined(LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT)
                con.write("K32_OK entering download mode\n", 30);
                vTaskDelay(pdMS_TO_TICKS(100));
#if CONFIG_KUBE32_CONSOLE_USB_OTG
                kube32::hal::usb_otg_console::prepare_download_reset();
#endif
#ifdef RTC_CNTL_FORCE_DOWNLOAD_BOOT
                REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
#else
                REG_SET_BIT(LP_SYSTEM_REG_SYS_CTRL_REG, LP_SYSTEM_REG_FORCE_DOWNLOAD_BOOT);
#endif
                esp_restart();
#else
                con.write("K32_ERR no software entry to download mode on this chip\n", 56);
#endif
            } else if (line.rfind("K32_CMD:SDKHZ:", 0) == 0) {
                // The card's SPI clock in kHz from the next boot on; 0 for the 20 MHz default
                const uint32_t khz = static_cast<uint32_t>(std::strtoul(line.c_str() + 14, nullptr, 10));
                nvs_handle_t h;
                if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                    if (khz) nvs_set_u32(h, "sdkhz", khz);
                    else nvs_erase_key(h, "sdkhz");
                    nvs_commit(h);
                    nvs_close(h);
                }
                con.write("K32_OK sd clock stored, rebooting\n", 34);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            } else if (line == "K32_CMD:BLOBS") {
                console_run_deep(con, [&] { console_blobs(con); });
            } else if (line.rfind("K32_CMD:BLOBREAD:", 0) == 0) {
                console_run_deep(con, [&] { console_blobread(con, line.substr(17)); });
            } else if (line.rfind("K32_CMD:MICGAIN:", 0) == 0) {
                // The microphone's digital gain until the next boot (1 to 64)
                if (s_board) s_board->mic_set_gain(static_cast<uint32_t>(std::strtoul(line.c_str() + 16, nullptr, 10)));
                con.write("K32_OK mic gain\n", 16);
            } else if (line.rfind("K32_CMD:MIC:", 0) == 0) {
                console_run_deep(con, [&] { console_mic(con, line.substr(12)); });
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
                int k = s_board ? s_board->key_read() : -1;
                if (k < 0) k = -1;
                char l[48];
                int ll = snprintf(l, sizeof(l), "K32_OK keypop=%d\n", k);
                con.write(l, static_cast<size_t>(ll));
#if CONFIG_HEAP_USE_HOOKS
            } else if (line.rfind("K32_CMD:HEAPWATCH:", 0) == 0) {
                // on: record internal allocations per task; dump: the peak and
                // now; off: dump and stop
                console_heapwatch(con, line.substr(18));
#endif
            } else if (line == "K32_CMD:HEAPMIN:start" || line == "K32_CMD:HEAPMIN") {
                // The internal heap's lowest free size between start and the
                // next K32_CMD:HEAPMIN (the [MEM] minimum counts from boot)
                const bool start = line.size() > 16;
                char l[96];
                int ll;
                if (start) {
                    heap_caps_monitor_local_minimum_free_size_stop();
                    heap_caps_monitor_local_minimum_free_size_start();
                    ll = snprintf(l, sizeof(l), "K32_OK heapmin start, internal free %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                } else {
                    const unsigned low = (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
                    heap_caps_monitor_local_minimum_free_size_stop();
                    ll = snprintf(l, sizeof(l), "K32_OK heapmin %u, internal free %u, largest %u\n", low,
                                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                }
                con.write(l, static_cast<size_t>(ll));
            } else if (line == "K32_CMD:IPC") {
                console_run_deep(con, [&] { console_ipc(con); });
            } else if (line.rfind("K32_CMD:IPCGAP:", 0) == 0) {
                kube32::ipc::ChannelManager::instance().set_fragment_gap_us(static_cast<uint32_t>(std::strtoul(line.c_str() + 15, nullptr, 10)));
                console_run_deep(con, [&] { console_ipc(con); });
            } else if (line.rfind("K32_CMD:IPCRXQ:", 0) == 0) {
                kube32::ipc::ChannelManager::instance().set_rx_queue_frames(std::strtoul(line.c_str() + 15, nullptr, 10));
                console_run_deep(con, [&] { console_ipc(con); });
            } else if (line.rfind("K32_CMD:IPCROUTE:", 0) == 0) {
                const std::string r = line.substr(17);
                using Route = kube32::ipc::ChannelManager::Route;
                kube32::ipc::ChannelManager::instance().set_route(r == "udp" ? Route::Udp : r == "espnow" ? Route::EspNow : Route::Auto);
                console_run_deep(con, [&] { console_ipc(con); });
            } else if (line == "K32_CMD:HEAP") {
                // Internal heap blocks by size, used and free: what holds the
                // memory on a board without PSRAM
                console_heap_histogram(con);
                con.write("K32_OK heap\n", 12);
            } else if (line.rfind("K32_CMD:BENCH:", 0) == 0) {
                console_bench(con, line.substr(14));
            } else if (line == "K32_CMD:POOL") {
                // The pod pool's small heap, chunk by chunk: where pods' memory lies
                kube32_pool_chunks([](size_t off, size_t size, int used, void* ctx) {
                    char l[64];
                    int ll = snprintf(l, sizeof(l), "K32_POOL %6u %6u %s\n", (unsigned)off, (unsigned)size, used ? "used" : "free");
                    static_cast<Console*>(ctx)->write(l, static_cast<size_t>(ll));
                }, &con);
                con.write("K32_OK pool\n", 12);
            } else if (line == "K32_CMD:WIFISTATS") {
                // The Wi-Fi driver's buffer and RX/TX counters, in its own log
                // format: a buffer type whose alloc column stays at its max
                // has stopped the driver
                console_run_deep(con, [&] { esp_wifi_statis_dump(WIFI_STATIS_BUFFER | WIFI_STATIS_RXTX); });
                con.write("K32_OK wifistats\n", 17);
            } else if (line == "K32_CMD:TCP") {
                // lwIP's TCP connections and the bytes each one holds
                console_tcp_pcbs(con);
                con.write("K32_OK tcp\n", 11);
            } else if (line == "K32_CMD:BTALL") {
                // Backtrace of every task, for a host to addr2line: where each one waits
#if CONFIG_IDF_TARGET_ARCH_XTENSA
                esp_backtrace_print_all_tasks(24);
                con.write("K32_OK btall\n", 13);
#else
                con.write("K32_ERR backtraces of every task need an Xtensa target\n", 55);
#endif
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
            } else if (line == "K32_CMD:THROW") {
                // The housekeeping loop raises std::bad_alloc on its next pass
                // and logs "[CLUSTER] housekeeping pass skipped: std::bad_alloc"
                s_cluster_throw.store(true);
                con.write("K32_OK throw\n", 13);
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
                    if (s_board) s_board->key_inject(c);
                    n++;
                }
                char l[48];
                int ll = snprintf(l, sizeof(l), "K32_OK %d key(s) queued\n", n);
                con.write(l, static_cast<size_t>(ll));
            } else if (line == "K32_CMD:SCAN") {
                // 2.4 GHz networks as seen by this radio (the only band an ESP32 has)
                const int64_t scan_t0 = esp_timer_get_time();
                std::vector<wifi_ap_record_t> aps;
                const esp_err_t scan = scan_networks(aps, 20);
                if (scan == ESP_OK) {
                    for (const wifi_ap_record_t& ap : aps) {
                        char l[96];
                        int ll = snprintf(l, sizeof(l), "K32_AP ssid=\"%s\" rssi=%d ch=%u auth=%d\n", (const char*)ap.ssid, ap.rssi, ap.primary, (int)ap.authmode);
                        con.write(l, static_cast<size_t>(ll));
                    }
                    char l[64];
                    int ll = snprintf(l, sizeof(l), "K32_OK scan done: %u networks in %u ms\n", (unsigned)aps.size(),
                                      (unsigned)((esp_timer_get_time() - scan_t0) / 1000));
                    con.write(l, static_cast<size_t>(ll));
                } else {
                    char l[64];
                    int ll = snprintf(l, sizeof(l), "K32_ERR scan failed: %s\n", esp_err_to_name(scan));
                    con.write(l, static_cast<size_t>(ll));
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
void install_hardware_hooks(kube32::hal::Board* board) {
    kube32::runtime::AppPlatformHooks hooks;
    hooks.random = []() -> uint32_t { return esp_random(); };
    if (board) {
        hooks.display_text = [board](int32_t x, int32_t y, const char* text, size_t len, uint32_t fg, uint32_t bg) -> int32_t {
            return board->display_text(x, y, text, len, static_cast<uint16_t>(fg), static_cast<uint16_t>(bg));
        };
        hooks.display_rect = [board](int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) -> int32_t {
            return board->display_rect(x, y, w, h, static_cast<uint16_t>(color));
        };
        hooks.display_bar = [board](int32_t x, int32_t y, int32_t w, int32_t h, uint32_t pct, uint32_t fg, uint32_t bg) -> int32_t {
            return board->display_bar(x, y, w, h, pct, static_cast<uint16_t>(fg), static_cast<uint16_t>(bg));
        };
        hooks.display_blit = [board](int32_t x, int32_t y, int32_t w, int32_t h, const uint16_t* pixels) -> int32_t {
            return board->display_blit(x, y, w, h, pixels);
        };
        hooks.display_info = [board](kube32::hal::DisplayInfo& info) -> int32_t {
            info = board->display_info();
            return info.width > 0 ? kube32::hal::HAL_OK : kube32::hal::HAL_ERR_NODEV;
        };
        hooks.touch_read = [board](kube32::hal::TouchPoint* out, uint32_t max) -> int32_t { return board->touch_read(out, max); };
        hooks.key_read = [board]() -> int32_t { return board->key_read(); };
        hooks.tone = [board](uint32_t f, uint32_t ms, uint32_t vol) -> int32_t { return board->tone(f, ms, vol); };
        hooks.audio_play = [board](const int16_t* samples, uint32_t count, uint32_t rate, uint32_t vol) -> int32_t {
            return board->audio_play(samples, count, rate, vol);
        };
        hooks.mic_read = [board](int16_t* out, uint32_t max, uint32_t timeout_ms) -> int32_t {
            return board->mic_read(out, max, timeout_ms);
        };
    }
    kube32::runtime::set_app_platform_hooks(hooks);
}

void print_banner(const NodeIdentity& id) {
    printf("\n================================================================\n");
    printf("  KUBE32  Kubernetes control plane + Wasm runtime on %s\n", kube32::hal::chip_display_name().c_str());
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
std::atomic<uint32_t> s_alloc_fail_caps{0};
char s_alloc_fail_task[configMAX_TASK_NAME_LEN];
std::atomic<bool> s_alloc_trace_armed{true};

void on_alloc_failed(size_t size, uint32_t caps, const char* function_name) {
    (void)function_name;
    s_alloc_fail_bytes.store(static_cast<uint32_t>(size));
    s_alloc_fail_caps.store(caps);
    const char* name = pcTaskGetName(nullptr);
    std::strncpy(s_alloc_fail_task, name ? name : "-", sizeof(s_alloc_fail_task) - 1);
    s_alloc_fail_count.fetch_add(1);
    // The first block of 1 KiB or more refused since the last [MEM] line:
    // where it was asked for, and what the heap had then. Not from a task
    // whose stack has no room for printf (the Wi-Fi driver's has ~200 bytes
    // left when its receive buffers cannot be allocated)
    if (size >= 1024 && uxTaskGetStackHighWaterMark(nullptr) >= 2048 && s_alloc_trace_armed.exchange(false)) {
        printf("[MEM] refused %u bytes (caps 0x%x), psram free %u (largest %u)\n", (unsigned)size, (unsigned)caps,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        esp_backtrace_print(12);
    }
}

void log_memory(const char* stage) {
    char failed[96] = "";
    s_alloc_trace_armed.store(true);
    if (const uint32_t n = s_alloc_fail_count.exchange(0)) {
        snprintf(failed, sizeof(failed), "  %u failed alloc(s), last %u bytes (caps 0x%x) in %s", (unsigned)n, (unsigned)s_alloc_fail_bytes.load(),
                 (unsigned)s_alloc_fail_caps.load(), s_alloc_fail_task);
    }
    char large[80] = "";
    const kube32::mem::LargeBlockStats lb = kube32::mem::large_blocks_stats();
    if (lb.size) {
        snprintf(large, sizeof(large), "  large blocks free %6u (min %6u, largest %6u, %u overflowed)", (unsigned)lb.free,
                 (unsigned)lb.min_free, (unsigned)lb.largest, (unsigned)lb.overflows);
    }
    printf("[MEM] %-22s internal free %6u (min %6u, largest %6u)  psram free %7u (min %7u, largest %7u)%s%s\n", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), large, failed);
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
        // Chips with less internal RAM (ESP32-C3) keep a smaller block
        const bool small_ram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 260 * 1024;
        kube32_pool_reserve((small_ram ? 56 : 80) * 1024);
        // One stack for the interpreter, while a contiguous block still
        // exists: every call into a pod (k32_init, k32_handle, k32_tick,
        // k32_event) runs on it, so the threads that make those calls need
        // no large stacks of their own. Xtensa makes no tail calls, so the
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
        // Keep the largest PSRAM block whole for pods (20 Wasm pages plus
        // header) before task stacks and buffers fragment the heap: a big data
        // pod's linear memory from its start, small pods from its end down.
        // The PSRAM besides it is the system's.
        kube32_pool_reserve_external(20 * 64 * 1024 + 4096);   // 20 Wasm pages plus wasm3's memory header
        // A control plane sets 64 KiB of its PSRAM aside, while it is whole,
        // for its large allocations (mem/large_blocks.hpp): lists, object
        // copies and controller working sets of 4 to 60 KB, which its heap
        // stops holding in one piece once small objects are spread over it
        if (ident.is_control_plane && !kube32::mem::large_blocks_reserve(64 * 1024)) printf("[MEM] no PSRAM for the large-block region\n");
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
        // The node's system-reserved PSRAM, which pods' code and structures
        // may not take and allocatable memory leaves out. A control plane
        // allocates there for the store, API lists and watches (a list copies
        // every object it returns), the controllers' working sets, the
        // console relay and firmware staging: its peak above the boot level
        // is 109 KiB with a few pods and over 122 KiB with the node full, and
        // 192 KiB keep half again as much. K32_CMD:SYSRESERVE:<size> overrides.
        size_t system_reserved = ident.is_control_plane ? 192 * 1024 : 64 * 1024;
        {
            nvs_handle_t rh;
            uint32_t stored = 0;
            if (nvs_open("k32", NVS_READONLY, &rh) == ESP_OK) {
                if (nvs_get_u32(rh, "sysres", &stored) == ESP_OK && stored >= 16 * 1024) system_reserved = stored;
                nvs_close(rh);
            }
        }
        kube32_pool_set_psram_reserve(system_reserved);
    }
    printf("[MEM] reserved %u bytes for pod linear memory, %u bytes of PSRAM for the system, %u for its large blocks\n",
           (unsigned)kube32_pool_block_size(), (unsigned)kube32_pool_psram_reserve(), (unsigned)kube32::mem::large_blocks_stats().size);

    // Peripherals of the integrated display board
    bool has_display = false;
    s_board = ident.board;
    if (s_board) has_display = s_board->init();
    // Node storage: the microSD card of the display board (its own SPI bus,
    // FAT at /sd) when one is inserted, otherwise the internal flash
    // "storage" partition (FAT with wear levelling at /data). One of the two.
    {
        // K32_CMD:SDKHZ:<khz> sets the card's SPI clock (20 MHz unless set)
        nvs_handle_t h;
        uint32_t sd_khz = 0;
        if (nvs_open("k32", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_u32(h, "sdkhz", &sd_khz);
            nvs_close(h);
        }
        kube32::hal::sd_set_max_khz(sd_khz);
        bool ok = kube32::hal::node_storage_mount(ident.has_display);
        kube32::hal::NodeStorageInfo si = kube32::hal::node_storage_info();
        if (ok) {
            printf("[STORAGE] %s at %s: %llu KiB total, %llu KiB free; mount took %lld bytes internal RAM, %lld bytes PSRAM\n",
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
    // Blobs for pods (storage/blob_store.hpp): the flash region, mapped whole,
    // and kube32/blobs on the card of a board with a card slot
    {
        static std::unique_ptr<kube32::storage::FlashBlobStore> flash_blobs;
        static kube32::storage::CardBlobStore card_blobs;
        bool app_slot = false;
        const size_t ram0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (auto region = kube32::storage::create_board_blob_region(app_slot)) {
            flash_blobs = std::make_unique<kube32::storage::FlashBlobStore>(std::move(region), app_slot);
            if (app_slot) {
                // The firmware keeps the slot while it holds a staged image or
                // the image an unconfirmed update falls back to
                flash_blobs->set_firmware_claim([]() -> std::string {
                    if (s_firmware_slots && s_firmware_slots->running().pending_verify) {
                        return "the running firmware is unconfirmed: " + s_firmware_slots->passive_slot() + " holds the image it falls back to";
                    }
                    if (s_firmware_api) {
                        const std::string phase = s_firmware_api->staged().phase;
                        if (phase == "Staged" || phase == "Receiving") {
                            return "a firmware image is " + std::string(phase == "Staged" ? "staged" : "being uploaded") + " in " +
                                   s_firmware_slots->passive_slot();
                        }
                    }
                    return std::string();
                });
                kube32::hal::set_passive_slot_tenant([](std::string& why) { return flash_blobs->yield_to_firmware(why); });
            }
            printf("[BLOBS] flash region %s: %llu bytes, %u blob(s), %d bytes of internal RAM\n", flash_blobs->location().c_str(),
                   (unsigned long long)flash_blobs->capacity_bytes(), (unsigned)flash_blobs->list().size(),
                   (int)((long)ram0 - (long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        } else {
            printf("[BLOBS] no flash region: no \"blobs\" partition and no second app slot that maps\n");
        }
        kube32::storage::set_node_blob_stores(flash_blobs.get(), ident.has_display ? &card_blobs : nullptr);
    }
    install_hardware_hooks(has_display ? ident.board : nullptr);
    kube32::app::install_default_platform_hooks();   // event_emit through the channel layer
    // What pods may claim: the chip's pins minus those the board's own
    // peripherals use, and the buses the board has pins for. A bare devkit
    // offers both I2C controllers and UART1/UART2 (defaults: I2C0 on GPIO
    // 8/9, UART1 on 17/18); a board profile describes its own (Board::describe).
    {
        kube32::hal::BoardProfile profile;
        if (ident.board) ident.board->describe(profile);
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
    // the cluster access point. A worker there dials K32_CMD:API:<host>, and
    // follows the address the control plane announces when that one is silent.
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
#if CONFIG_HEAP_USE_HOOKS
    heapwatch_arm_at_boot();
#endif
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
    // The dispatcher queues messages for the pods' event thread and runs no
    // pod code: its stack holds about 3 KB of native frames
    kube32::ipc::ChannelManager::instance().start_background(3000, ident.has_psram ? 8192 : 4096, ident.has_psram);

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
    s_runtime = wasm_runtime.get();
    std::unique_ptr<kube32::storage::Store> store;
    std::unique_ptr<kube32::api::TimestampBackfill> backfill;   // objects created before the clock is set
    std::unique_ptr<kube32::controller::NodeLifecycleController> node_lifecycle;
    std::unique_ptr<kube32::scheduler::Scheduler> scheduler;
    std::unique_ptr<kube32::controller::DeploymentController> deployments;
    std::unique_ptr<kube32::controller::WorkloadController> workloads;
    std::unique_ptr<kube32::controller::FirmwareRolloutController> firmware_rollouts;
    std::unique_ptr<kube32::controller::DeviceController> devices;
    std::unique_ptr<kube32::controller::CheckpointController> checkpoints;
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
    // Without PSRAM pods start one per sync pass: a pass holds what it starts
    kubelet_cfg.max_starts_per_sync = ident.has_psram ? 0 : 1;
    kubelet_cfg.control_plane = ident.is_control_plane;
    kubelet_cfg.core_affinity = 1;
    // K32_CMD:TOKEN: the API server checks it, every kubelet sends it
    const std::string api_token = read_nvs_str("token");
    s_api_token_set = valid_token(api_token);
    if (s_api_token_set) kubelet_cfg.api_token = api_token;
    kubelet_cfg.node_labels["kube32.io/board"] = ident.board ? ident.board->name() : "generic";
    // Devices answering on the board's I2C pins become node labels, and the
    // parts identified among them a node annotation for the Device controller
    static kube32::hal::I2cDiscovery discovery;
    kubelet_cfg.node_label_source = [] {
        discovery.scan();
        return discovery.labels();
    };
    kubelet_cfg.node_annotation_source = [] {
        return std::map<std::string, std::string>{{kube32::models::kAnnotationI2cDevices, discovery.annotation()}};
    };
    kubelet_cfg.node_label_prefix = "i2c.kube32.io/";
    // The interpreter's speed, timed once on the stack and memory pods use
    {
        const uint64_t t0 = kube32::osal::System::get_uptime_ms();
        char mops[16];
        snprintf(mops, sizeof(mops), "%.2f", kube32::runtime::measure_interp_mops());
        kubelet_cfg.node_annotations["kube32.io/interp-mops"] = mops;
        printf("[KUBELET] interpreter: %s million wasm3 operations/s (measured in %u ms)\n", mops,
               (unsigned)(kube32::osal::System::get_uptime_ms() - t0));
        // and whether it computes 64-bit integer values right on this build
        const std::string i64 = kube32::runtime::check_interp_i64();
        kubelet_cfg.node_annotations["kube32.io/interp-check"] = i64.empty() ? "ok" : "failed: " + i64;
        if (i64.empty()) printf("[KUBELET] interpreter check: ok\n");
        else printf("[KUBELET] interpreter check FAILED: %s; pods on this node get wrong 64-bit integer values\n", i64.c_str());
    }
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
        // From here on (the journal's objects are an earlier boot's): what
        // is created before the clock is set gets its time once it is
        if (kube32::osal::System::get_unix_time_ms() == 0) backfill = std::make_unique<kube32::api::TimestampBackfill>(*store);
        // A pod bound to another node, or one it is to stop: tell that
        // node's kubelet over the channel fabric, so it lists its pods now
        // and not at its next poll (two seconds on a board). At most every
        // 200 ms a node; the watch runs on the writer's thread.
        store->watch("/registry/pods/", [self = ident.name](const kube32::storage::WatchEvent& ev) {
            static kube32::osal::Mutex mu;
            static std::map<std::string, std::string, std::less<>> bound;   // pod key -> node, pods of other nodes
            static std::map<std::string, uint64_t, std::less<>> last;       // node -> last wake
            std::string_view node;
            if (ev.type != kube32::storage::WatchEventType::Deleted) {
                static constexpr std::string_view kKey = "\"nodeName\":\"";
                const size_t at = ev.json.find(kKey);
                if (at != std::string::npos) {
                    const size_t from = at + kKey.size();
                    const size_t to = ev.json.find('"', from);
                    if (to != std::string::npos && to - from <= 63) node = std::string_view(ev.json.data() + from, to - from);
                }
            }
            const bool deleting = ev.json.find("\"deletionTimestamp\"") != std::string::npos;
            std::string wake;
            {
                kube32::osal::LockGuard guard(mu);
                auto it = bound.find(ev.key);
                if (ev.type == kube32::storage::WatchEventType::Deleted || node.empty() || node == self) {
                    if (it != bound.end()) bound.erase(it);
                    return;
                }
                // Bound here or to another node since the last write, or being deleted
                if (it == bound.end() || it->second != node) bound[ev.key] = std::string(node);
                else if (!deleting) return;
                const uint64_t now = kube32::osal::System::get_uptime_ms();
                auto l = last.find(node);
                if (l != last.end() && now - l->second < 200) return;
                last[std::string(node)] = now;
                wake.assign(node);
            }
            kube32::ipc::ChannelManager::instance().publish("control-plane", kube32::ipc::K32C_KUBELET_SYNC_TOPIC,
                                                            reinterpret_cast<const uint8_t*>(wake.data()), wake.size());
        });

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
        // Devices: driver pods, their Services, Devices from discovery
        kube32::controller::DeviceControllerConfig dev_cfg;
        dev_cfg.reconcile_interval_ms = 2000;
        dev_cfg.stack_in_psram = ident.has_psram;
        devices = std::make_unique<kube32::controller::DeviceController>(*store, scheduler.get(), dev_cfg);
        devices->start();
        // A use of an OnDemand Device, from this node's callers and the activate subresource
        kube32::app::DeviceDemand::Activate activate_device = [dc = devices.get()](const std::string& ns, const std::string& name) {
            dc->activate(ns, name);
            return true;
        };
        // Checkpoints and moves of pods: snapshots go from the kubelets to
        // this node's API server (checkpoint data), freezes wait 10 s at most
        kube32::controller::CheckpointControllerConfig ck_cfg;
        ck_cfg.stack_in_psram = ident.has_psram;
        if (s_api_token_set) ck_cfg.token = api_token;
        ck_cfg.local_node = ident.name;
        checkpoints = std::make_unique<kube32::controller::CheckpointController>(*store, scheduler.get(), ck_cfg);
        checkpoints->start();

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
        api_server->route_handler().set_device_activator([dc = devices.get()](const std::string& ns, const std::string& name) { dc->activate(ns, name); });
        // Node proxy (/api/v1/nodes/<node>/proxy/...): this node's storage
        // in-process, the workers' through their kubelet endpoint
        static std::unique_ptr<kube32::api::NodeProxyHandler> node_proxy;
        node_proxy = std::make_unique<kube32::api::NodeProxyHandler>(*store, api_server->route_handler(), ident.name,
            [](kube32::api::HttpConnection& conn, std::string_view path) {
                if (kube32::app::is_pod_checkpoint_path(path)) return kube32::app::serve_pod_checkpoint(conn, path, std::string());
                if (kube32::app::is_node_blobs_path(path)) return kube32::app::serve_node_blobs(conn, path, std::string());
                if (!kube32::app::is_node_storage_path(path)) return false;
                return kube32::app::serve_node_storage(conn, path, std::string(), 8u * 1024 * 1024);
            });
        if (s_api_token_set) node_proxy->set_node_token(api_token);
        // The staged firmware image (/apis/kube32.io/v1/firmware); everything
        // else goes on to the node proxy
        static std::unique_ptr<kube32::api::FirmwareApi> firmware_api;
        // Checkpoint data (/apis/kube32.io/v1/.../checkpoints/<name>/data), in PSRAM
        static std::unique_ptr<kube32::api::CheckpointApi> checkpoint_api;
        checkpoint_api = std::make_unique<kube32::api::CheckpointApi>(*store, api_server->route_handler(), 256 * 1024, node_proxy.get());
        firmware_api = std::make_unique<kube32::api::FirmwareApi>(*s_firmware_slots, *store, api_server->route_handler(), checkpoint_api.get());
        s_firmware_api = firmware_api.get();

        // k32_http_call: Services and NetworkPolicies read from the store.
        // https:// calls are made here, for this node's pods and, through the
        // egress gateway (/apis/kube32.io/v1/egress), for the workers' pods:
        // at most two TLS sessions at once, their stacks and buffers in PSRAM.
        // Public sites send response heads of several KB (cookies, policies).
        static std::unique_ptr<kube32::osal::TlsClient> tls_client;
        {
            std::string tls_error;
            tls_client = kube32::osal::TlsClient::create("", tls_error);
            if (!tls_client) printf("[EGRESS] no https:// calls: %s\n", tls_error.c_str());
        }
        static std::unique_ptr<kube32::app::ServiceClient> service_client;
        kube32::app::ServiceClientOptions sc_opts;
        sc_opts.pool_size = 4;
        sc_opts.response_head_bytes = 8192;
        sc_opts.tls = tls_client.get();
        sc_opts.tls_sessions = 2;
        sc_opts.tls_stack_bytes = 12288;
        sc_opts.tls_stack_in_psram = ident.has_psram;
        sc_opts.activate_device = activate_device;
        sc_opts.node_name = ident.name;
        service_client = std::make_unique<kube32::app::ServiceClient>(kube32::app::make_store_service_resolver(*store), sc_opts,
                                                                      kube32::app::make_store_egress_source(*store));
        kube32::app::install_service_client(service_client.get());
        static std::unique_ptr<kube32::app::EgressGateway> egress_gateway;
        egress_gateway = std::make_unique<kube32::app::EgressGateway>(*service_client, *store, api_server->route_handler(), firmware_api.get());
        api_server->set_handler(egress_gateway.get());
        if (!api_server->start()) ESP_LOGE(TAG, "API server failed to start");
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
        io.activate_device = activate_device;
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
        // namespaces with its pod sync; https:// calls go to the control
        // plane's egress gateway
        static std::shared_ptr<kube32::kubelet::HttpKubeClient> svc_api;
        svc_api = std::make_shared<kube32::kubelet::HttpKubeClient>(api_host, 6443);
        if (s_api_token_set) svc_api->set_bearer_token(api_token);
        if (!ident.has_psram) svc_api->set_max_body(16 * 1024);
        auto api_get = [](const std::string& path, std::string& body) { return svc_api->get(path, body); };
        kubelet_cfg.network_policies = std::make_shared<kube32::net::NodeNetworkPolicies>();
        kube32::app::ServiceClientOptions sc_opts;
        sc_opts.gateway_host = api_host;
        sc_opts.gateway_port = 6443;
        if (s_api_token_set) sc_opts.gateway_token = api_token;
        // A use of an OnDemand Device: the activate subresource; the answer's body is not kept
        sc_opts.activate_device = [](const std::string& ns, const std::string& name) {
            auto head = [](int, std::string_view) { return true; };
            auto skip = [](const char*, size_t) { return true; };
            return svc_api->request("POST", "/apis/kube32.io/v1/namespaces/" + ns + "/devices/" + name + "/activate", "{}", head, skip) == 200;
        };
        sc_opts.node_name = ident.name;
        static std::unique_ptr<kube32::app::ServiceClient> service_client;
        service_client = std::make_unique<kube32::app::ServiceClient>(kube32::app::make_api_service_resolver(api_get), sc_opts,
                                                                      kube32::app::make_node_egress_source(kubelet_cfg.network_policies, api_get));
        kube32::app::install_service_client(service_client.get());

        kubelet = std::make_unique<kube32::kubelet::Kubelet>(kubelet_cfg, *wasm_runtime);
        kubelet->start();
        // The control plane names this node when it binds or deletes a pod
        // here: list the pods now, not at the next poll
        kube32::ipc::ChannelManager::instance().subscribe(kube32::ipc::K32C_KUBELET_SYNC_TOPIC,
            [k = kubelet.get(), name = ident.name](std::string_view, uint32_t, const uint8_t* data, size_t len) {
                if (len == name.size() && std::memcmp(data, name.data(), len) == 0) k->request_sync();
            });
        printf("[WORKER] kubelet -> http://%s:6443\n", api_host.c_str());
        if (has_display) {
            // The boot screen stays product-neutral on purpose: what runs the
            // board is not announced on the display, the apps draw their own UI.
            constexpr uint16_t kBanner = 0x194B, kGreen = 0x07E0, kWhite = 0xFFFF, kCyan = 0x07FF, kDark = 0x10A2;
            s_board->display_rect(0, 0, s_board->display_info().width, 18, kBanner);
            s_board->display_text(6, 5, "ready", 5, kGreen, kBanner);
            s_board->display_text(6, 28, ip.c_str(), ip.size(), kWhite, kDark);
            std::string app_url = "http://" + api_host + "/";
            s_board->display_text(6, 40, app_url.c_str(), app_url.size(), kCyan, kDark);
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

    // A pod is ticked once per kube32.io/tick-interval (1 s by default); one
    // that returns >0 from k32_tick has background work left (incremental
    // seeding, for example) and is ticked again after a short yield so
    // requests interleave. On a board with PSRAM two pod workers tick the
    // pods and deliver their events, one a core: core 1's on the shared
    // interpreter stack, as the request threads, core 0's on a stack of its
    // own, below Wi-Fi and the system's tasks there. That stack comes from
    // the pod block, so pods' allocatable memory pays for the second core
    // and the system's PSRAM stays as it is. K32_CMD:PODCORES:1 keeps one.
    size_t pod_workers = 0;
    if (ident.has_psram && kube32_interp_stack_active() && read_nvs_u32("podcores", 2) >= 2) {
        std::vector<kube32::runtime::PodWorkerOptions> workers(2);
        workers[0].core = 1;
        workers[0].pinned = true;
        workers[0].stack_bytes = 8192;
        workers[0].stack_in_psram = true;
        workers[0].priority = 4;
        // The interpreter's recursion peaks near 56 KB: the shared stack's
        // 64 KiB and room for the worker's own frames
        workers[1].core = 0;
        workers[1].pinned = true;
        workers[1].stack_bytes = 68 * 1024;
        workers[1].stack_buffer = kube32_pool_system_alloc(workers[1].stack_bytes);
        workers[1].own_interpreter_stack = true;
        workers[1].priority = 3;
        if (!workers[1].stack_buffer) workers.pop_back();
        pod_workers = kube32::runtime::AppPodHost::instance().start_workers(workers);
        printf("[POD] %u pod worker(s)\n", (unsigned)pod_workers);
    }
    if (pod_workers == 0) {
        // Pod ticks run the interpreter, so they get a thread with a stack
        // sized for it rather than this 9 KB task
        kube32::osal::Thread::Config t_cfg;
        t_cfg.name = "k32_tick";
        t_cfg.stack_size = kube32_interp_stack_active() ? (ident.has_psram ? 8192 : 4096) : 65536;
        t_cfg.stack_in_psram = ident.has_psram;
        t_cfg.priority = 4;
        t_cfg.core_affinity = 1;
        kube32::osal::Thread::create([](void*) {
            while (true) {
                uint32_t wait = 1000;   // out of memory: the next pass is a second away
                try {
                    wait = kube32::runtime::AppPodHost::instance().tick_due(kube32::osal::System::get_uptime_ms());
                } catch (const std::exception& e) {
                    printf("[POD] tick pass skipped: %s\n", e.what());
                }
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
        // Out of memory ends the pass, not the task: the next one starts a
        // second later, when pods or requests have let go of some
        try {
            if (s_cluster_throw.exchange(false)) throw std::bad_alloc();   // K32_CMD:THROW
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
                // Objects made before the clock was set get their timestamps once
                // it is; one that could not be written is tried again a minute later
                static bool backfilled = false;
                if (backfill && kube32::osal::System::get_unix_time_ms() != 0 && (!backfilled || iter % 60 == 0)) {
                    backfilled = true;
                    const size_t n = backfill->run();
                    if (n) printf("[CLOCK] timestamps set on %u object(s) created before the clock synchronized\n", (unsigned)n);
                    if (backfill->pending() == 0) backfill.reset();
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
            if (!ident.is_control_plane && kubelet && !ident.has_psram && iter % 10 == 0) {
                // A worker without PSRAM whose kubelet gets no answer while
                // its heap has no large block left restarts, HELLOs or not
                // (kubelet::stalled_on_heap)
                static const uint64_t kubelet_since = kube32::osal::System::get_uptime_ms();
                const uint64_t now = kube32::osal::System::get_uptime_ms();
                const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
                const uint64_t last_ok = kubelet->last_sync_ok_ms();
                if (kube32::kubelet::stalled_on_heap(now, last_ok, kubelet_since, largest)) {
                    log_memory("before reboot");
                    printf("[NET] no answer from the API server for %u s and the largest free block is %u B: restarting\n",
                           (unsigned)((now - std::max(last_ok, kubelet_since)) / 1000), (unsigned)largest);
                    fflush(stdout);
                    esp_restart();
                }
            }
            if (!ident.is_control_plane && kubelet && iter % 10 == 0) {
                // A station can stay associated with an access point that no
                // longer knows it: a control plane that restarts its AP within
                // the beacon timeout drops the worker's frames while its beacons
                // keep the link up. Join again when the API server has not
                // answered a pod sync for a minute on a link that looks up.
                // When that does not help within another minute (an AP that
                // restarted answers a station still holding its 802.11w keys
                // with SA queries and handshakes the station never finishes,
                // linked or not), restart the Wi-Fi driver, at most once a minute.
                // After five restarts without an answer the board reboots: a
                // station whose heap fragmented can associate and lose the link
                // again every few seconds until it does. By then the control
                // plane has marked the node NotReady and evicts its pods.
                static uint64_t quiet_since = kube32::osal::System::get_uptime_ms();
                static bool rejoined = false;
                static int restarts = 0;
                const uint64_t now = kube32::osal::System::get_uptime_ms();
                // The control plane's HELLOs still arrive over the access
                // point: the link carries traffic, and joining again cannot
                // make an API server that is busy or restarting answer
                const uint64_t hello = kube32::ipc::ChannelManager::instance().last_ip_hello_ms(api_host);
                const bool link_carries = hello && now - hello < 15000;
                if (link_carries) quiet_since = std::max(quiet_since, hello);
                if (kubelet->last_sync_ok_ms() > quiet_since || link_carries) {   // answered since
                    rejoined = false;
                    restarts = 0;
                }
                const uint64_t heard = std::max(kubelet->last_sync_ok_ms(), quiet_since);
                // A control plane that announces another address than the one
                // this worker dials (it joined another network, or its DHCP
                // lease changed): store that address and start again with it
                const std::string announced = kube32::ipc::ChannelManager::instance().announced_api();
                if (now - heard > 30000 && !announced.empty() && announced != api_host) {
                    nvs_handle_t h;
                    if (nvs_open("k32", NVS_READWRITE, &h) == ESP_OK) {
                        nvs_set_str(h, "api", announced.c_str());
                        nvs_commit(h);
                        nvs_close(h);
                    }
                    printf("[NET] no answer from %s; the control plane announces %s, restarting with it\n", api_host.c_str(), announced.c_str());
                    fflush(stdout);
                    esp_restart();
                }
                if (now - heard > 60000 && !rejoined && net_mgr->station_has_ip()) {
                    printf("[NET] no answer from the control plane for %u s on a connected link, joining the access point again\n",
                           (unsigned)((now - heard) / 1000));
                    net_mgr->rejoin();
                    rejoined = true;
                    quiet_since = now;
                } else if (now - heard > 60000 && (rejoined || !net_mgr->station_has_ip())) {
                    if (++restarts > 5) {
                        log_memory("before reboot");
                        printf("[NET] no answer from the control plane after 5 Wi-Fi restarts, rebooting\n");
                        fflush(stdout);
                        esp_restart();
                    }
                    printf("[NET] no answer from the control plane for %u s, restarting Wi-Fi\n", (unsigned)((now - heard) / 1000));
                    net_mgr->restart_radio();
                    quiet_since = now;
                }
            }
            if (ident.is_control_plane && uplink && iter % 10 == 0) {
                // Workers on this network learn the API server's address from HELLO
                kube32::ipc::ChannelManager::instance().set_announced_api(net_mgr->station_has_ip() ? net_mgr->get_ip_address() : std::string());
                // A control plane whose station has had no address for a minute
                // restarts its Wi-Fi driver, at most once a minute: a join can
                // stall in "connecting" with no event that would retry it.
                static uint64_t addressless_since = 0;
                const uint64_t now = kube32::osal::System::get_uptime_ms();
                if (net_mgr->station_has_ip()) {
                    addressless_since = 0;
                } else if (addressless_since == 0) {
                    addressless_since = now;
                } else if (now - addressless_since > 60000) {
                    printf("[NET] station without an address for %u s, restarting Wi-Fi\n", (unsigned)((now - addressless_since) / 1000));
                    net_mgr->restart_radio();
                    addressless_since = now;
                }
            }
        } catch (const std::exception& e) {
            printf("[CLUSTER] housekeeping pass skipped: %s\n", e.what());
        }
        if (iter % 30 == 0) log_memory("steady state");
        kube32::osal::System::feed_watchdog();
    }
}

} // namespace

extern "C" void app_main() {
#if CONFIG_KUBE32_CONSOLE_USB_OTG
    kube32::hal::usb_otg_console::early_init();
#endif
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // Through the thread trampoline, never as the task's entry function: on
    // Xtensa the unwinder stops at a frame whose return address is zero, and
    // the kernel starts a task with that return address, so a catch written
    // in the entry function itself is never reached (osal/thread_posix.cpp)
    kube32::osal::Thread::Config cluster_cfg;
    cluster_cfg.name = "k32_cluster";
    cluster_cfg.stack_size = 9216;
    cluster_cfg.priority = 5;
    cluster_cfg.core_affinity = 0;
    kube32::osal::Thread::create(cluster_task, nullptr, cluster_cfg);
}

#endif
