#include "runtime/host_functions.hpp"
#include "runtime/container_lifecycle.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include "wasm3.h"
#include "m3_env.h"
#include "ipc/channel.hpp"

#include <mutex>
#include <map>
#include <iostream>
#include <cstdio>
#include <cstring>

#include "hal/pod_buses.hpp"
#include "hal/pod_rmt.hpp"
#include "runtime/pod_log.hpp"
#include "runtime/k32_app_abi.h"

#if defined(KUBE32_TARGET_ESP32)
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#endif

namespace kube32::runtime {

static std::mutex s_pod_registry_mutex;
static std::map<const void*, std::string> s_module_to_pod;
static std::map<const void*, std::string> s_runtime_to_pod;

static std::string get_pod_id(IM3Runtime runtime, IM3ImportContext _ctx) {
    if (_ctx && _ctx->userdata) {
        return static_cast<const char*>(_ctx->userdata);
    }
    std::lock_guard<std::mutex> lock(s_pod_registry_mutex);
    auto it_r = s_runtime_to_pod.find(runtime);
    if (it_r != s_runtime_to_pod.end()) {
        return it_r->second;
    }
    void* ud = m3_GetUserData(runtime);
    if (ud) {
        return static_cast<const char*>(ud);
    }
    return "default-pod";
}

static_assert(K32_RMT_MAX_DURATIONS == hal::kRmtMaxDurations && K32_RMT_MAX_FRAME_US == hal::kRmtMaxFrameUs &&
                  K32_RMT_RX_QUEUE_FRAMES == hal::kRmtRxQueueFrames && K32_RMT_RX_QUEUE_DURATIONS == hal::kRmtRxQueueDurations,
              "k32_app_abi.h and hal/pod_rmt.hpp disagree on the RMT limits");

// The pod of a host call, by reference: the entry lives as long as the pod's
// runtime, so a call on a hot path copies no string.
static const std::string& get_pod_ref(IM3Runtime runtime) {
    static const std::string kUnknown("default-pod");
    std::lock_guard<std::mutex> lock(s_pod_registry_mutex);
    auto it = s_runtime_to_pod.find(runtime);
    return it != s_runtime_to_pod.end() ? it->second : kUnknown;
}

// -----------------------------------------------------------------------------
// Raw Wasm3 Host Callbacks
// -----------------------------------------------------------------------------

// esp32_gpio_set_mode: signature "i(ii)" -> returns int32_t, args: (pin, mode)
static const void* raw_esp32_gpio_set_mode(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t pin  = static_cast<uint32_t>(_sp[1]);
    uint32_t mode = static_cast<uint32_t>(_sp[2]);
    std::string pod_id = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::GPIO, pin, hal::AccessMode::Config)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

#if defined(KUBE32_TARGET_ESP32)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    if (mode == static_cast<uint32_t>(hal::PinMode::Output)) {
        io_conf.mode = GPIO_MODE_OUTPUT;
    } else {
        io_conf.mode = GPIO_MODE_INPUT;
        if (mode == static_cast<uint32_t>(hal::PinMode::InputPullup)) io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        else if (mode == static_cast<uint32_t>(hal::PinMode::InputPulldown)) io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    }
    esp_err_t err = gpio_config(&io_conf);
    *reinterpret_cast<int32_t*>(_sp) = (err == ESP_OK) ? hal::HAL_OK : hal::HAL_ERR_IO;
#else
    int32_t res = hal::MockHal::instance().gpio_set_mode(pin, static_cast<hal::PinMode>(mode));
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

    return m3Err_none;
}

// esp32_gpio_write: signature "i(ii)" -> returns int32_t, args: (pin, value)
static const void* raw_esp32_gpio_write(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t pin = static_cast<uint32_t>(_sp[1]);
    uint32_t val = static_cast<uint32_t>(_sp[2]);
    std::string pod_id = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::GPIO, pin, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

#if defined(KUBE32_TARGET_ESP32)
    esp_err_t err = gpio_set_level(static_cast<gpio_num_t>(pin), val ? 1 : 0);
    *reinterpret_cast<int32_t*>(_sp) = (err == ESP_OK) ? hal::HAL_OK : hal::HAL_ERR_IO;
#else
    int32_t res = hal::MockHal::instance().gpio_write(pin, val);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

    return m3Err_none;
}

// esp32_gpio_read: signature "i(i)" -> returns int32_t, args: (pin)
static const void* raw_esp32_gpio_read(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t pin = static_cast<uint32_t>(_sp[1]);
    std::string pod_id = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::GPIO, pin, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

#if defined(KUBE32_TARGET_ESP32)
    int level = gpio_get_level(static_cast<gpio_num_t>(pin));
    *reinterpret_cast<int32_t*>(_sp) = level;
#else
    int32_t res = hal::MockHal::instance().gpio_read(pin);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

    return m3Err_none;
}

// esp32_adc_read: signature "i(i)" -> returns int32_t, args: (channel)
static const void* raw_esp32_adc_read(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t channel = static_cast<uint32_t>(_sp[1]);
    std::string pod_id = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::ADC1, channel, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

#if defined(KUBE32_TARGET_ESP32)
    *reinterpret_cast<int32_t*>(_sp) = esp32_adc_read(channel);
#else
    int32_t res = hal::MockHal::instance().adc1_read(channel);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

    return m3Err_none;
}

// esp32_i2c_write: signature "i(iiii)" -> returns int32_t, args: (bus, addr, data_offset, len)
// Bytes written, or a negative error. len 0 probes the address: 0 when a
// device acknowledges, -19 (ENODEV) when none does.
static const void* raw_esp32_i2c_write(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t bus         = static_cast<uint32_t>(_sp[1]);
    uint32_t addr        = static_cast<uint32_t>(_sp[2]);
    uint32_t data_offset = static_cast<uint32_t>(_sp[3]);
    uint32_t len         = static_cast<uint32_t>(_sp[4]);
    std::string pod_id   = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::I2C, bus, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(data_offset) + len > mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }

    const uint8_t* host_data = static_cast<const uint8_t*>(_mem) + data_offset;
    *reinterpret_cast<int32_t*>(_sp) = (addr > 0x7f) ? hal::HAL_ERR_INVAL : hal::i2c_bus_write(bus, static_cast<uint8_t>(addr), host_data, len);
    return m3Err_none;
}

// esp32_i2c_read: signature "i(iiii)" -> returns int32_t, args: (bus, addr, buf_offset, len)
static const void* raw_esp32_i2c_read(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t bus        = static_cast<uint32_t>(_sp[1]);
    uint32_t addr       = static_cast<uint32_t>(_sp[2]);
    uint32_t buf_offset = static_cast<uint32_t>(_sp[3]);
    uint32_t len        = static_cast<uint32_t>(_sp[4]);
    std::string pod_id  = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::I2C, bus, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(buf_offset) + len > mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }

    uint8_t* host_buf = static_cast<uint8_t*>(_mem) + buf_offset;
    *reinterpret_cast<int32_t*>(_sp) = (addr > 0x7f) ? hal::HAL_ERR_INVAL : hal::i2c_bus_read(bus, static_cast<uint8_t>(addr), host_buf, len);
    return m3Err_none;
}

// esp32_uart_write: signature "i(iii)" -> returns int32_t, args: (port, data_offset, len)
static const void* raw_esp32_uart_write(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t port        = static_cast<uint32_t>(_sp[1]);
    uint32_t data_offset = static_cast<uint32_t>(_sp[2]);
    uint32_t len         = static_cast<uint32_t>(_sp[3]);
    std::string pod_id   = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::UART, port, hal::AccessMode::Write)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(data_offset) + len > mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }

    const uint8_t* host_data = static_cast<const uint8_t*>(_mem) + data_offset;
    *reinterpret_cast<int32_t*>(_sp) = hal::uart_port_write(port, host_data, len);
    return m3Err_none;
}

// esp32_uart_read: signature "i(iii)" -> returns int32_t, args: (port, buf_offset, max_len)
static const void* raw_esp32_uart_read(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t port       = static_cast<uint32_t>(_sp[1]);
    uint32_t buf_offset = static_cast<uint32_t>(_sp[2]);
    uint32_t max_len    = static_cast<uint32_t>(_sp[3]);
    std::string pod_id  = get_pod_id(runtime, _ctx);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::UART, port, hal::AccessMode::Read)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM;
        return m3Err_none;
    }

    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(buf_offset) + max_len > mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }

    uint8_t* host_buf = static_cast<uint8_t*>(_mem) + buf_offset;
    *reinterpret_cast<int32_t*>(_sp) = hal::uart_port_read(port, host_buf, max_len, 50);
    return m3Err_none;
}

// esp32_rmt_send: signature "i(ii)" -> returns int32_t, args: (durations_offset, count)
// Sends one IR frame on the pod's RMT TX channel: `count` uint16 durations
// in microseconds, mark first. Returns count once the frame is out, or a
// negative error.
static const void* raw_esp32_rmt_send(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    (void)_ctx;
    uint32_t data_offset = static_cast<uint32_t>(_sp[1]);
    uint32_t count       = static_cast<uint32_t>(_sp[2]);
    const std::string& pod_id = get_pod_ref(runtime);
    int32_t& ret = *reinterpret_cast<int32_t*>(_sp);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::RmtTx, 0, hal::AccessMode::Write)) {
        ret = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    if (count == 0 || count > hal::kRmtMaxDurations) {
        ret = hal::HAL_ERR_INVAL;
        return m3Err_none;
    }
    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(data_offset) + 2ULL * count > mem_size) {
        ret = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    if (runtime_stop_requested(runtime)) return m3Err_trapOutOfGas;   // being stopped: end here, no new frame
    ret = hal::rmt_send(pod_id, static_cast<const uint8_t*>(_mem) + data_offset, count);
    return m3Err_none;
}

// esp32_rmt_receive: signature "i(ii)" -> returns int32_t, args: (buf_offset, cap)
// The oldest frame the pod's RMT RX channel captured, without waiting: the
// number of uint16 durations copied, K32_ERR_AGAIN when none is queued,
// K32_ERR_TOOBIG when the frame has more than `cap` (the first `cap` are
// copied, the frame is gone). Frames the receiver dropped since the last
// call are reported in the pod's log.
static const void* raw_esp32_rmt_receive(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    (void)_ctx;
    uint32_t buf_offset = static_cast<uint32_t>(_sp[1]);
    uint32_t cap        = static_cast<uint32_t>(_sp[2]);
    const std::string& pod_id = get_pod_ref(runtime);
    int32_t& ret = *reinterpret_cast<int32_t*>(_sp);

    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::RmtRx, 0, hal::AccessMode::Read)) {
        ret = hal::HAL_ERR_PERM;
        return m3Err_none;
    }
    if (cap > hal::kRmtMaxDurations) cap = hal::kRmtMaxDurations;   // no frame is longer
    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(buf_offset) + 2ULL * cap > mem_size) {
        ret = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }
    hal::RmtDrops drops;
    ret = hal::rmt_receive(pod_id, static_cast<uint8_t*>(_mem) + buf_offset, cap, &drops);
    if (drops.queue_full || drops.too_long) {
        char line[128];
        int n = snprintf(line, sizeof line, "[kube32] RMT receiver dropped");
        if (drops.queue_full) n += snprintf(line + n, sizeof line - n, " %u frame(s) on a full queue", (unsigned)drops.queue_full);
        if (drops.too_long) {
            n += snprintf(line + n, sizeof line - n, "%s %u frame(s) longer than %u durations", drops.queue_full ? "," : "", (unsigned)drops.too_long,
                          (unsigned)hal::kRmtMaxDurations);
        }
        printf("[POD %s] %s\n", pod_id.c_str(), line);
        PodLogStore::runtime().append(pod_id, std::string_view(line, static_cast<size_t>(n)));
    }
    return m3Err_none;
}

// esp32_get_time_us: signature "I()" -> returns int64_t
static const void* raw_esp32_get_time_us(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    (void)runtime;
    (void)_ctx;
    (void)_mem;
    int64_t time_us = static_cast<int64_t>(osal::System::get_uptime_ms() * 1000ULL);
    *reinterpret_cast<int64_t*>(_sp) = time_us;
    return m3Err_none;
}

// Waits are cut into slices. A container being stopped leaves the call
// within one slice with the same trap the stop raises in guest code, so its
// thread ends instead of waiting out the call.
static constexpr uint32_t kWaitSliceMs = 20;

// esp32_delay_ms: signature "v(i)" -> returns void, args: (ms)
static const void* raw_esp32_delay_ms(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    (void)_ctx;
    (void)_mem;
    uint32_t ms = static_cast<uint32_t>(_sp[0]);
    while (ms > 0) {
        if (runtime_stop_requested(runtime)) return m3Err_trapOutOfGas;
        uint32_t slice = ms < kWaitSliceMs ? ms : kWaitSliceMs;
        osal::Thread::sleep_ms(slice);
        ms -= slice;
    }
    osal::System::feed_watchdog();
    return m3Err_none;
}

// kube32_channel_open: signature "i(ii)" -> returns int32_t handle, args: (name_offset, flags)
static const void* raw_kube32_channel_open(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    uint32_t name_offset = static_cast<uint32_t>(_sp[1]);
    uint32_t flags       = static_cast<uint32_t>(_sp[2]);
    std::string pod_id   = get_pod_id(runtime, _ctx);

    // 1. Gatekeeper Capability Firewall Check
    if (!hal::CapabilityFirewall::instance().check_permission(pod_id, hal::PeripheralType::IPC, 0, hal::AccessMode::ReadWrite)) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_PERM; // -EPERM
        return m3Err_none;
    }

    // 2. Validate linear memory bounds for channel name string
    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (name_offset >= mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT; // -EFAULT
        return m3Err_none;
    }

    const char* str_start = static_cast<const char*>(_mem) + name_offset;
    size_t max_len = mem_size - name_offset;
    size_t name_len = strnlen(str_start, max_len);
    if (name_len == max_len) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT; // Unterminated string
        return m3Err_none;
    }

    std::string channel_name(str_start, name_len);
    int32_t handle = ipc::ChannelManager::instance().open(pod_id, channel_name, flags);
    *reinterpret_cast<int32_t*>(_sp) = handle;
    return m3Err_none;
}

// kube32_channel_send: signature "i(iii)" -> returns int32_t bytes sent, args: (handle, data_offset, len)
static const void* raw_kube32_channel_send(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    int32_t  handle      = static_cast<int32_t>(_sp[1]);
    uint32_t data_offset = static_cast<uint32_t>(_sp[2]);
    uint32_t len         = static_cast<uint32_t>(_sp[3]);
    std::string pod_id   = get_pod_id(runtime, _ctx);

    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(data_offset) + len > mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }

    const uint8_t* data = static_cast<const uint8_t*>(_mem) + data_offset;
    int32_t sent = ipc::ChannelManager::instance().send(pod_id, handle, data, len);
    *reinterpret_cast<int32_t*>(_sp) = sent;
    return m3Err_none;
}

// kube32_channel_recv: signature "i(iiii)" -> returns int32_t bytes received, args: (handle, buf_offset, max_len, timeout_ms)
static const void* raw_kube32_channel_recv(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    int32_t  handle      = static_cast<int32_t>(_sp[1]);
    uint32_t buf_offset  = static_cast<uint32_t>(_sp[2]);
    uint32_t max_len     = static_cast<uint32_t>(_sp[3]);
    uint32_t timeout_ms  = static_cast<uint32_t>(_sp[4]);
    std::string pod_id   = get_pod_id(runtime, _ctx);

    size_t mem_size = m3_GetMemorySizeAt(_mem);
    if (static_cast<uint64_t>(buf_offset) + max_len > mem_size) {
        *reinterpret_cast<int32_t*>(_sp) = hal::HAL_ERR_FAULT;
        return m3Err_none;
    }

    uint8_t* buf = static_cast<uint8_t*>(_mem) + buf_offset;
    int32_t recvd = ipc::ChannelManager::instance().recv(pod_id, handle, buf, max_len, timeout_ms < kWaitSliceMs ? timeout_ms : kWaitSliceMs);
    uint32_t waited = kWaitSliceMs;
    while (recvd == ipc::IPC_ERR_TIMEDOUT && waited < timeout_ms) {
        if (runtime_stop_requested(runtime)) return m3Err_trapOutOfGas;
        uint32_t slice = (timeout_ms - waited) < kWaitSliceMs ? (timeout_ms - waited) : kWaitSliceMs;
        recvd = ipc::ChannelManager::instance().recv(pod_id, handle, buf, max_len, slice);
        waited += slice;
    }
    *reinterpret_cast<int32_t*>(_sp) = recvd;
    return m3Err_none;
}

// kube32_channel_close: signature "i(i)" -> returns int32_t status, args: (handle)
static const void* raw_kube32_channel_close(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    (void)runtime;
    (void)_ctx;
    (void)_mem;
    int32_t handle     = static_cast<int32_t>(_sp[1]);
    std::string pod_id = get_pod_id(runtime, _ctx);

    int32_t res = ipc::ChannelManager::instance().close(pod_id, handle);
    *reinterpret_cast<int32_t*>(_sp) = res;
    return m3Err_none;
}

// -----------------------------------------------------------------------------
// link_host_functions implementation
// -----------------------------------------------------------------------------

bool link_host_functions(IM3Module module, const std::string& pod_id) {
    if (!module) return false;

    {
        std::lock_guard<std::mutex> lock(s_pod_registry_mutex);
        s_module_to_pod[module] = pod_id;
        IM3Runtime rt = m3_GetModuleRuntime(module);
        if (rt) {
            s_runtime_to_pod[rt] = pod_id;
        }
    }

    // Link raw functions under "env" module
    m3_LinkRawFunction(module, "env", "esp32_gpio_set_mode", "i(ii)",   &raw_esp32_gpio_set_mode);
    m3_LinkRawFunction(module, "env", "esp32_gpio_write",    "i(ii)",   &raw_esp32_gpio_write);
    m3_LinkRawFunction(module, "env", "esp32_gpio_read",     "i(i)",    &raw_esp32_gpio_read);
    m3_LinkRawFunction(module, "env", "esp32_adc_read",      "i(i)",    &raw_esp32_adc_read);
    m3_LinkRawFunction(module, "env", "esp32_i2c_write",     "i(iiii)", &raw_esp32_i2c_write);
    m3_LinkRawFunction(module, "env", "esp32_i2c_read",      "i(iiii)", &raw_esp32_i2c_read);
    m3_LinkRawFunction(module, "env", "esp32_uart_write",    "i(iii)",  &raw_esp32_uart_write);
    m3_LinkRawFunction(module, "env", "esp32_uart_read",     "i(iii)",  &raw_esp32_uart_read);
    m3_LinkRawFunction(module, "env", "esp32_rmt_send",      "i(ii)",   &raw_esp32_rmt_send);
    m3_LinkRawFunction(module, "env", "esp32_rmt_receive",   "i(ii)",   &raw_esp32_rmt_receive);
    m3_LinkRawFunction(module, "env", "esp32_get_time_us",   "I()",     &raw_esp32_get_time_us);
    m3_LinkRawFunction(module, "env", "esp32_delay_ms",      "v(i)",    &raw_esp32_delay_ms);

    // Link Virtual Channel IPC functions
    m3_LinkRawFunction(module, "env", "kube32_channel_open",  "i(ii)",   &raw_kube32_channel_open);
    m3_LinkRawFunction(module, "env", "kube32_channel_send",  "i(iii)",  &raw_kube32_channel_send);
    m3_LinkRawFunction(module, "env", "kube32_channel_recv",  "i(iiii)", &raw_kube32_channel_recv);
    m3_LinkRawFunction(module, "env", "kube32_channel_close", "i(i)",    &raw_kube32_channel_close);

    return true;
}

// -----------------------------------------------------------------------------
// ABI-Stable C Host Functions
// -----------------------------------------------------------------------------

extern "C" {

int32_t esp32_gpio_set_mode(uint32_t pin, uint32_t mode) {
#if defined(KUBE32_TARGET_ESP32)
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode = (mode == 1) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    return (gpio_config(&io_conf) == ESP_OK) ? hal::HAL_OK : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().gpio_set_mode(pin, static_cast<hal::PinMode>(mode));
#endif
}

int32_t esp32_gpio_write(uint32_t pin, uint32_t value) {
#if defined(KUBE32_TARGET_ESP32)
    return (gpio_set_level(static_cast<gpio_num_t>(pin), value ? 1 : 0) == ESP_OK) ? hal::HAL_OK : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().gpio_write(pin, value);
#endif
}

int32_t esp32_gpio_read(uint32_t pin) {
#if defined(KUBE32_TARGET_ESP32)
    return gpio_get_level(static_cast<gpio_num_t>(pin));
#else
    return hal::MockHal::instance().gpio_read(pin);
#endif
}

#if defined(KUBE32_TARGET_ESP32)
static adc_oneshot_unit_handle_t s_adc1_handle = nullptr;
static uint16_t s_adc1_configured_mask = 0;
static std::mutex s_adc_mutex;

static bool adc1_prepare(uint32_t channel) {
    std::lock_guard<std::mutex> lock(s_adc_mutex);
    if (!s_adc1_handle) {
        adc_oneshot_unit_init_cfg_t init_cfg = {};
        init_cfg.unit_id = ADC_UNIT_1;
        init_cfg.ulp_mode = ADC_ULP_MODE_DISABLE;
        if (adc_oneshot_new_unit(&init_cfg, &s_adc1_handle) != ESP_OK) return false;
    }
    if (!(s_adc1_configured_mask & (1u << channel))) {
        adc_oneshot_chan_cfg_t ch_cfg = {};
        ch_cfg.atten = ADC_ATTEN_DB_12;
        ch_cfg.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_config_channel(s_adc1_handle, static_cast<adc_channel_t>(channel), &ch_cfg) != ESP_OK) return false;
        s_adc1_configured_mask |= (1u << channel);
    }
    return true;
}
#endif

int32_t esp32_adc_read(uint32_t channel) {
#if defined(KUBE32_TARGET_ESP32)
    if (channel >= 10 || !adc1_prepare(channel)) return hal::HAL_ERR_NODEV;
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc1_handle, static_cast<adc_channel_t>(channel), &raw);
    return (err == ESP_OK) ? raw : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().adc1_read(channel);
#endif
}

int32_t esp32_i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, uint32_t len) {
    return hal::i2c_bus_write(bus, addr, data, len);
}

int32_t esp32_i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, uint32_t len) {
    return hal::i2c_bus_read(bus, addr, buf, len);
}

int32_t esp32_uart_write(uint32_t port, const uint8_t* data, uint32_t len) {
    return hal::uart_port_write(port, data, len);
}

int32_t esp32_uart_read(uint32_t port, uint8_t* buf, uint32_t max_len) {
    return hal::uart_port_read(port, buf, max_len, 50);
}

int64_t esp32_get_time_us() {
    return static_cast<int64_t>(osal::System::get_uptime_ms() * 1000ULL);
}

void esp32_delay_ms(uint32_t ms) {
    osal::Thread::sleep_ms(ms);
    osal::System::feed_watchdog();
}

int32_t kube32_channel_open(const char* channel_name, uint32_t flags) {
    return ipc::ChannelManager::instance().open("native", channel_name ? channel_name : "", flags);
}

int32_t kube32_channel_send(int32_t handle, const uint8_t* data, size_t len) {
    return ipc::ChannelManager::instance().send("native", handle, data, len);
}

int32_t kube32_channel_recv(int32_t handle, uint8_t* out_buf, size_t max_len, uint32_t timeout_ms) {
    return ipc::ChannelManager::instance().recv("native", handle, out_buf, max_len, timeout_ms);
}

int32_t kube32_channel_close(int32_t handle) {
    return ipc::ChannelManager::instance().close("native", handle);
}

} // extern "C"

} // namespace kube32::runtime
