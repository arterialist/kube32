#include "hal/usb_otg_console.hpp"

#include "sdkconfig.h"

#if CONFIG_KUBE32_CONSOLE_USB_OTG

#include "hal/console_ring.hpp"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "esp_vfs_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"

#if !SOC_USB_UTMI_PHY_NUM
#include "hal/usb_serial_jtag_ll.h"
#endif

#if SOC_USB_UTMI_PHY_NUM
#include "device/dcd.h"
#include "esp_private/periph_ctrl.h"
#include "hal/usb_utmi_ll.h"
#endif

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kube32::hal::usb_otg_console {
namespace {

constexpr const char* kTeePath = "/dev/k32console";
constexpr tinyusb_cdcacm_itf_t kPort = TINYUSB_CDC_ACM_0;
constexpr uint32_t kDrainStackBytes = 2560;
constexpr uint32_t kStallMs = 1000;

char s_ring_storage[CONFIG_KUBE32_CONSOLE_USB_OTG_RING_BYTES];
ConsoleRing s_ring(s_ring_storage, sizeof(s_ring_storage));
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

int s_uart_fd = -1;
TaskHandle_t s_drain_task = nullptr;
SemaphoreHandle_t s_rx_ready = nullptr;
std::atomic<bool> s_host_open{false};
std::atomic<bool> s_mounted{false};
std::atomic<bool> s_line_seen{false};

// Set before a restart the guard asks for: the next boot stays on
// USB-Serial/JTAG. RTC memory, so it survives the restart and not a power cut.
RTC_NOINIT_ATTR uint32_t s_skip_magic;
constexpr uint32_t kSkipMagic = 0x4B333243;

void notify_drain() {
    if (s_drain_task) xTaskNotifyGive(s_drain_task);
}

// Queued for the host when one reads; otherwise kept for the next one, the
// oldest whole lines going first.
void tee_output(const char* data, size_t len) {
    portENTER_CRITICAL(&s_lock);
    if (s_host_open.load(std::memory_order_relaxed)) {
        s_ring.append(data, len);   // a full ring drops this write: logs never wait
    } else {
        s_ring.append_dropping_oldest(data, len);
    }
    portEXIT_CRITICAL(&s_lock);
    notify_drain();
}

// The VFS stdout and stderr write through: the console device as before, and the ring
ssize_t tee_write(int, const void* data, size_t size) {
    if (s_uart_fd >= 0) ::write(s_uart_fd, data, size);
    tee_output(static_cast<const char*>(data), size);
    return static_cast<ssize_t>(size);
}
int tee_open(const char*, int, int) { return 0; }
int tee_close(int) { return 0; }
int tee_fstat(int, struct stat* st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR;
    return 0;
}

void drain_task(void*) {
    char line[96];
    uint32_t stalled_ms = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        while (s_host_open.load(std::memory_order_relaxed)) {
            if (!s_mounted.load(std::memory_order_relaxed)) break;
            // Lost output comes first, as a line of its own
            size_t lost = 0;
            portENTER_CRITICAL(&s_lock);
            lost = s_ring.lost_line(line, sizeof(line));
            portEXIT_CRITICAL(&s_lock);
            if (lost) tinyusb_cdcacm_write_queue(kPort, reinterpret_cast<const uint8_t*>(line), lost);

            const char* span = nullptr;
            size_t n = 0;
            portENTER_CRITICAL(&s_lock);
            n = s_ring.peek(&span);
            portEXIT_CRITICAL(&s_lock);
            if (n == 0) {
                tinyusb_cdcacm_write_flush(kPort, 0);
                break;
            }
            const size_t sent = tinyusb_cdcacm_write_queue(kPort, reinterpret_cast<const uint8_t*>(span), n);
            if (sent) {
                portENTER_CRITICAL(&s_lock);
                s_ring.consume(sent);
                portEXIT_CRITICAL(&s_lock);
                tinyusb_cdcacm_write_flush(kPort, 0);
                stalled_ms = 0;
                continue;
            }
            // TinyUSB's FIFO is full: nobody is reading. A host that has
            // gone quiet stops holding output back; the ring keeps the newest.
            vTaskDelay(pdMS_TO_TICKS(10));
            tinyusb_cdcacm_write_flush(kPort, 0);
            stalled_ms += 10;
            if (stalled_ms >= kStallMs) {
                s_host_open.store(false, std::memory_order_relaxed);
                stalled_ms = 0;
            }
        }
    }
}

void host_activity() {
    if (!s_host_open.exchange(true, std::memory_order_relaxed)) notify_drain();
}

void on_rx(int, cdcacm_event_t*) {
    host_activity();
    if (s_rx_ready) xSemaphoreGive(s_rx_ready);
}

// Any change of the line state or the line coding is a host opening the port.
// DTR says nothing: tools/kube32_serial_gateway.py and the like hold it low.
void on_line_change(int, cdcacm_event_t*) { host_activity(); }

void on_usb_event(tinyusb_event_t* event, void*) {
    if (event->id == TINYUSB_EVENT_ATTACHED) {
        s_mounted.store(true, std::memory_order_relaxed);
    } else if (event->id == TINYUSB_EVENT_DETACHED) {
        s_mounted.store(false, std::memory_order_relaxed);
        s_host_open.store(false, std::memory_order_relaxed);
    }
}

#if SOC_USB_UTMI_PHY_NUM
// A CPU reset leaves the high-speed controller's DMA running: stop it before
// the next image reuses the RAM
void usb_shutdown() {
    dcd_int_disable(0);
    PERIPH_RCC_ATOMIC() {
        usb_utmi_ll_reset_register();
        usb_utmi_ll_enable_bus_clock(false);
    }
}
#else
// On chips where OTG and USB-Serial/JTAG share the one USB PHY, the mapping
// lives in RTC registers and survives a restart
void give_phy_to_usb_serial_jtag() { usb_serial_jtag_ll_phy_enable_external(false); }
#endif

bool should_start() {
    const bool skip = s_skip_magic == kSkipMagic;
    s_skip_magic = 0;
    if (skip) {
        printf("[CONSOLE] no console line came over USB OTG last boot: this boot stays on USB-Serial/JTAG\n");
        return false;
    }
    switch (esp_reset_reason()) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            printf("[CONSOLE] restarted after a crash: this boot stays on USB-Serial/JTAG\n");
            return false;
        default:
            return true;
    }
}

#if CONFIG_KUBE32_CONSOLE_USB_OTG_GUARD_SECONDS > 0 && !SOC_USB_UTMI_PHY_NUM
void guard_fired(void*) {
    if (s_line_seen.load(std::memory_order_relaxed)) return;
    printf("[CONSOLE] no console line over USB OTG in %d s: restarting on USB-Serial/JTAG\n", CONFIG_KUBE32_CONSOLE_USB_OTG_GUARD_SECONDS);
    s_skip_magic = kSkipMagic;
    give_phy_to_usb_serial_jtag();
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}
#endif

}  // namespace

void early_init() {
    // UART0 as before, and USB-Serial/JTAG on a boot that stays on it
    s_uart_fd = ::open("/dev/console", O_WRONLY);
    static const esp_vfs_fs_ops_t ops = {
        .write = tee_write,
        .open = tee_open,
        .close = tee_close,
        .fstat = tee_fstat,
    };
    if (esp_vfs_register_fs(kTeePath, &ops, ESP_VFS_FLAG_STATIC, nullptr) != ESP_OK) return;
    fflush(stdout);
    fflush(stderr);
    freopen(kTeePath, "w", stdout);
    freopen(kTeePath, "w", stderr);
    setvbuf(stdout, nullptr, _IOLBF, 256);
    setvbuf(stderr, nullptr, _IONBF, 0);
}

bool start() {
    if (!should_start()) {
#if !SOC_USB_UTMI_PHY_NUM
        give_phy_to_usb_serial_jtag();
#endif
        return false;
    }
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_rx_ready = xSemaphoreCreateBinary();
    if (!s_rx_ready) return false;

    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG(on_usb_event, nullptr);
    if (tinyusb_driver_install(&usb) != ESP_OK) {
        printf("[CONSOLE] TinyUSB did not start\n");
        return false;
    }
    tinyusb_config_cdcacm_t cdc = {};
    cdc.cdc_port = kPort;
    cdc.callback_rx = on_rx;
    cdc.callback_line_state_changed = on_line_change;
    cdc.callback_line_coding_changed = on_line_change;
    if (tinyusb_cdcacm_init(&cdc) != ESP_OK) {
        printf("[CONSOLE] the CDC port did not start\n");
        return false;
    }
    if (xTaskCreate(drain_task, "k32_cdc", kDrainStackBytes, nullptr, 6, &s_drain_task) != pdPASS) return false;
#if SOC_USB_UTMI_PHY_NUM
    esp_register_shutdown_handler(usb_shutdown);
#endif
#if CONFIG_KUBE32_CONSOLE_USB_OTG_GUARD_SECONDS > 0 && !SOC_USB_UTMI_PHY_NUM
    static esp_timer_handle_t guard;
    const esp_timer_create_args_t args = {.callback = guard_fired, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK, .name = "k32_cdc_guard", .skip_unhandled_events = true};
    if (esp_timer_create(&args, &guard) == ESP_OK) esp_timer_start_once(guard, CONFIG_KUBE32_CONSOLE_USB_OTG_GUARD_SECONDS * 1000000ULL);
#endif
    printf("[CONSOLE] USB OTG console up (%u bytes of internal RAM, %u byte ring)\n",
           static_cast<unsigned>(internal_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(sizeof(s_ring_storage)));
    return true;
}

int read(char* buf, size_t cap, uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        size_t got = 0;
        if (tinyusb_cdcacm_read(kPort, reinterpret_cast<uint8_t*>(buf), cap, &got) == ESP_OK && got > 0) {
            s_line_seen.store(true, std::memory_order_relaxed);
            return static_cast<int>(got);
        }
        const TickType_t now = xTaskGetTickCount();
        const TickType_t left = deadline > now ? deadline - now : 0;
        if (left == 0) return 0;
        // A host may send bytes before this wait starts: wait for short slices
        xSemaphoreTake(s_rx_ready, left < pdMS_TO_TICKS(10) ? left : pdMS_TO_TICKS(10));
    }
}

void write(const char* data, size_t len) { tee_output(data, len); }

bool write_frame(const char* data, size_t len, uint32_t timeout_ms) {
    if (len > s_ring.capacity()) return false;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        if (!s_host_open.load(std::memory_order_relaxed)) return false;
        bool ok;
        portENTER_CRITICAL(&s_lock);
        ok = s_ring.append(data, len);
        portEXIT_CRITICAL(&s_lock);
        if (ok) {
            notify_drain();
            return true;
        }
        if (xTaskGetTickCount() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void prepare_download_reset() {
#if !SOC_USB_UTMI_PHY_NUM
    // The ROM talks to the host through USB-Serial/JTAG
    give_phy_to_usb_serial_jtag();
#endif
}

}  // namespace kube32::hal::usb_otg_console

#endif  // CONFIG_KUBE32_CONSOLE_USB_OTG
