#include "runtime/host_functions.hpp"
#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include "osal/system.hpp"
#include "osal/thread.hpp"

#include "wasm3.h"
#include "m3_env.h"

#include <mutex>
#include <map>
#include <iostream>

#if defined(KUBE32_TARGET_ESP32)
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/i2c.h"
#include "driver/uart.h"
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
    // ESP32 ADC oneshot read stub/driver
    *reinterpret_cast<int32_t*>(_sp) = 1850;
#else
    int32_t res = hal::MockHal::instance().adc1_read(channel);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

    return m3Err_none;
}

// esp32_i2c_write: signature "i(iiii)" -> returns int32_t, args: (bus, addr, data_offset, len)
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

#if defined(KUBE32_TARGET_ESP32)
    esp_err_t err = i2c_master_write_to_device(static_cast<i2c_port_t>(bus), static_cast<uint8_t>(addr), host_data, len, pdMS_TO_TICKS(100));
    *reinterpret_cast<int32_t*>(_sp) = (err == ESP_OK) ? static_cast<int32_t>(len) : hal::HAL_ERR_IO;
#else
    int32_t res = hal::MockHal::instance().i2c_write(bus, static_cast<uint8_t>(addr), host_data, len);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

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

#if defined(KUBE32_TARGET_ESP32)
    esp_err_t err = i2c_master_read_from_device(static_cast<i2c_port_t>(bus), static_cast<uint8_t>(addr), host_buf, len, pdMS_TO_TICKS(100));
    *reinterpret_cast<int32_t*>(_sp) = (err == ESP_OK) ? static_cast<int32_t>(len) : hal::HAL_ERR_IO;
#else
    int32_t res = hal::MockHal::instance().i2c_read(bus, static_cast<uint8_t>(addr), host_buf, len);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

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

#if defined(KUBE32_TARGET_ESP32)
    int bytes = uart_write_bytes(static_cast<uart_port_t>(port), host_data, len);
    *reinterpret_cast<int32_t*>(_sp) = (bytes >= 0) ? bytes : hal::HAL_ERR_IO;
#else
    int32_t res = hal::MockHal::instance().uart_write(port, host_data, len);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

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

#if defined(KUBE32_TARGET_ESP32)
    int bytes = uart_read_bytes(static_cast<uart_port_t>(port), host_buf, max_len, pdMS_TO_TICKS(50));
    *reinterpret_cast<int32_t*>(_sp) = (bytes >= 0) ? bytes : hal::HAL_ERR_IO;
#else
    int32_t res = hal::MockHal::instance().uart_read(port, host_buf, max_len);
    *reinterpret_cast<int32_t*>(_sp) = res;
#endif

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

// esp32_delay_ms: signature "v(i)" -> returns void, args: (ms)
static const void* raw_esp32_delay_ms(IM3Runtime runtime, IM3ImportContext _ctx, uint64_t* _sp, void* _mem) {
    (void)runtime;
    (void)_ctx;
    (void)_mem;
    uint32_t ms = static_cast<uint32_t>(_sp[0]);
    osal::Thread::sleep_ms(ms);
    osal::System::feed_watchdog();
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
    m3_LinkRawFunction(module, "env", "esp32_get_time_us",   "I()",     &raw_esp32_get_time_us);
    m3_LinkRawFunction(module, "env", "esp32_delay_ms",      "v(i)",    &raw_esp32_delay_ms);

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

int32_t esp32_adc_read(uint32_t channel) {
#if defined(KUBE32_TARGET_ESP32)
    return 1850;
#else
    return hal::MockHal::instance().adc1_read(channel);
#endif
}

int32_t esp32_i2c_write(uint32_t bus, uint8_t addr, const uint8_t* data, uint32_t len) {
#if defined(KUBE32_TARGET_ESP32)
    esp_err_t err = i2c_master_write_to_device(static_cast<i2c_port_t>(bus), addr, data, len, pdMS_TO_TICKS(100));
    return (err == ESP_OK) ? static_cast<int32_t>(len) : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().i2c_write(bus, addr, data, len);
#endif
}

int32_t esp32_i2c_read(uint32_t bus, uint8_t addr, uint8_t* buf, uint32_t len) {
#if defined(KUBE32_TARGET_ESP32)
    esp_err_t err = i2c_master_read_from_device(static_cast<i2c_port_t>(bus), addr, buf, len, pdMS_TO_TICKS(100));
    return (err == ESP_OK) ? static_cast<int32_t>(len) : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().i2c_read(bus, addr, buf, len);
#endif
}

int32_t esp32_uart_write(uint32_t port, const uint8_t* data, uint32_t len) {
#if defined(KUBE32_TARGET_ESP32)
    int bytes = uart_write_bytes(static_cast<uart_port_t>(port), data, len);
    return (bytes >= 0) ? bytes : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().uart_write(port, data, len);
#endif
}

int32_t esp32_uart_read(uint32_t port, uint8_t* buf, uint32_t max_len) {
#if defined(KUBE32_TARGET_ESP32)
    int bytes = uart_read_bytes(static_cast<uart_port_t>(port), buf, max_len, pdMS_TO_TICKS(50));
    return (bytes >= 0) ? bytes : hal::HAL_ERR_IO;
#else
    return hal::MockHal::instance().uart_read(port, buf, max_len);
#endif
}

int64_t esp32_get_time_us() {
    return static_cast<int64_t>(osal::System::get_uptime_ms() * 1000ULL);
}

void esp32_delay_ms(uint32_t ms) {
    osal::Thread::sleep_ms(ms);
    osal::System::feed_watchdog();
}

} // extern "C"

} // namespace kube32::runtime
