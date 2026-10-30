#include "hal/pod_buses.hpp"
#include "hal/hal_types.hpp"
#include "hal/mock_hal.hpp"

#include <cstdio>
#include <mutex>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#endif

namespace kube32::hal {

namespace {

struct I2cSlot {
    std::mutex mutex;
    BusState state;
#if defined(ESP_PLATFORM)
    i2c_master_bus_handle_t bus{nullptr};
    i2c_master_dev_handle_t dev{nullptr};   // the last address talked to
    uint16_t dev_addr{0xffff};
#endif
};

struct UartSlot {
    std::mutex mutex;
    BusState state;
};

I2cSlot s_i2c[2];
UartSlot s_uart[3];   // index = port; 0 is the console and never handed out

#if defined(ESP_PLATFORM)
int32_t esp_to_hal(esp_err_t e) {
    switch (e) {
        case ESP_OK: return HAL_OK;
        case ESP_ERR_NOT_FOUND: return HAL_ERR_NODEV;
        case ESP_ERR_TIMEOUT: return HAL_ERR_TIMEDOUT;
        case ESP_ERR_INVALID_ARG: return HAL_ERR_INVAL;
        case ESP_ERR_NO_MEM: return HAL_ERR_NOMEM;
        default: return HAL_ERR_IO;
    }
}
#endif

#if defined(ESP_PLATFORM)
// A failed transfer can leave a device's I2C logic mid-byte, holding SDA or
// ignoring its address until it sees a STOP: clock the bus free (nine SCL
// pulses, then a STOP) before the next transfer. Slot mutex held.
int32_t i2c_failed(I2cSlot& s, unsigned index, uint8_t addr, esp_err_t e) {
    if (s.bus) i2c_master_bus_reset(s.bus);
    if (++s.state.recoveries <= 3 || s.state.recoveries % 100 == 0) {
        printf("[HAL] I2C%u: %s talking to 0x%02x, bus cleared (%u times)\n", index, esp_err_to_name(e), addr, (unsigned)s.state.recoveries);
    }
    return e == ESP_ERR_TIMEOUT ? HAL_ERR_TIMEDOUT : HAL_ERR_IO;
}
#endif

// Slot mutex held by the caller
bool install_i2c(I2cSlot& s, const std::string& pod_id, const BusAssignment& b, std::string& err) {
#if defined(ESP_PLATFORM)
    i2c_master_bus_config_t cfg = {};
    cfg.i2c_port = static_cast<i2c_port_num_t>(b.index);
    cfg.sda_io_num = static_cast<gpio_num_t>(b.pin_a);
    cfg.scl_io_num = static_cast<gpio_num_t>(b.pin_b);
    cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    cfg.glitch_ignore_cnt = 7;
    cfg.flags.enable_internal_pullup = true;   // keeps an empty bus idle-high; attached devices bring their own pull-ups
    // i2c_failed reports failed transfers; the driver's line for every one
    // floods the console while a pod polls a device that does not answer
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    esp_err_t e = i2c_new_master_bus(&cfg, &s.bus);
    if (e != ESP_OK) {
        s.bus = nullptr;
        err = "cannot install the I2C" + std::to_string(b.index) + " driver: " + esp_err_to_name(e);
        return false;
    }
#endif
    s.state.installed = true;
    s.state.owner = pod_id;
    s.state.pin_a = b.pin_a;
    s.state.pin_b = b.pin_b;
    s.state.speed = b.speed ? b.speed : kDefaultI2cHz;
    s.state.installs++;
    s.state.recoveries = 0;
    printf("[HAL] I2C%u installed for %s: SDA %u, SCL %u, %u Hz\n", b.index, pod_id.c_str(), b.pin_a, b.pin_b, (unsigned)s.state.speed);
    return true;
}

void release_i2c(I2cSlot& s, unsigned index) {
    if (!s.state.installed) return;
#if defined(ESP_PLATFORM)
    if (s.dev) i2c_master_bus_rm_device(s.dev);
    s.dev = nullptr;
    s.dev_addr = 0xffff;
    if (s.bus) i2c_del_master_bus(s.bus);
    s.bus = nullptr;
    gpio_reset_pin(static_cast<gpio_num_t>(s.state.pin_a));
    gpio_reset_pin(static_cast<gpio_num_t>(s.state.pin_b));
#endif
    printf("[HAL] I2C%u released by %s\n", index, s.state.owner.c_str());
    s.state.installed = false;
    s.state.owner.clear();
}

bool install_uart(UartSlot& s, const std::string& pod_id, const BusAssignment& b, std::string& err) {
#if defined(ESP_PLATFORM)
    const uart_port_t port = static_cast<uart_port_t>(b.index);
    if (uart_is_driver_installed(port)) {
        err = "UART" + std::to_string(b.index) + " is in use by the firmware";
        return false;
    }
    uart_config_t uc = {};
    uc.baud_rate = static_cast<int>(b.speed ? b.speed : kDefaultUartBaud);
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    esp_err_t e = uart_driver_install(port, 256, 0, 0, nullptr, 0);   // RX ring of 256 bytes, writes block until sent
    if (e == ESP_OK) e = uart_param_config(port, &uc);
    if (e == ESP_OK) e = uart_set_pin(port, b.pin_a, b.pin_b, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (e != ESP_OK) {
        if (uart_is_driver_installed(port)) uart_driver_delete(port);
        err = "cannot install the UART" + std::to_string(b.index) + " driver: " + esp_err_to_name(e);
        return false;
    }
#endif
    s.state.installed = true;
    s.state.owner = pod_id;
    s.state.pin_a = b.pin_a;
    s.state.pin_b = b.pin_b;
    s.state.speed = b.speed ? b.speed : kDefaultUartBaud;
    s.state.installs++;
    printf("[HAL] UART%u installed for %s: TX %u, RX %u, %u baud\n", b.index, pod_id.c_str(), b.pin_a, b.pin_b, (unsigned)s.state.speed);
    return true;
}

void release_uart(UartSlot& s, unsigned index) {
    if (!s.state.installed) return;
#if defined(ESP_PLATFORM)
    uart_driver_delete(static_cast<uart_port_t>(index));
    gpio_reset_pin(static_cast<gpio_num_t>(s.state.pin_a));
    gpio_reset_pin(static_cast<gpio_num_t>(s.state.pin_b));
#endif
    printf("[HAL] UART%u released by %s\n", index, s.state.owner.c_str());
    s.state.installed = false;
    s.state.owner.clear();
}

} // namespace

bool bus_install(const std::string& pod_id, const std::vector<BusAssignment>& buses, std::string& out_error) {
    std::vector<const BusAssignment*> done;
    for (const auto& b : buses) {
        bool ok = false;
        if (b.kind == BusKind::I2c && b.index < 2) {
            std::lock_guard<std::mutex> lock(s_i2c[b.index].mutex);
            if (s_i2c[b.index].state.installed) out_error = "I2C" + std::to_string(b.index) + " is in use by " + s_i2c[b.index].state.owner;
            else ok = install_i2c(s_i2c[b.index], pod_id, b, out_error);
        } else if (b.kind == BusKind::Uart && b.index >= 1 && b.index <= 2) {
            std::lock_guard<std::mutex> lock(s_uart[b.index].mutex);
            if (s_uart[b.index].state.installed) out_error = "UART" + std::to_string(b.index) + " is in use by " + s_uart[b.index].state.owner;
            else ok = install_uart(s_uart[b.index], pod_id, b, out_error);
        } else {
            out_error = "no such bus";
        }
        if (!ok) {
            for (const BusAssignment* d : done) {
                if (d->kind == BusKind::I2c) {
                    std::lock_guard<std::mutex> lock(s_i2c[d->index].mutex);
                    release_i2c(s_i2c[d->index], d->index);
                } else {
                    std::lock_guard<std::mutex> lock(s_uart[d->index].mutex);
                    release_uart(s_uart[d->index], d->index);
                }
            }
            return false;
        }
        done.push_back(&b);
    }
    return true;
}

void bus_release(const std::string& pod_id) {
    for (unsigned i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> lock(s_i2c[i].mutex);
        if (s_i2c[i].state.installed && s_i2c[i].state.owner == pod_id) release_i2c(s_i2c[i], i);
    }
    for (unsigned p = 1; p <= 2; ++p) {
        std::lock_guard<std::mutex> lock(s_uart[p].mutex);
        if (s_uart[p].state.installed && s_uart[p].state.owner == pod_id) release_uart(s_uart[p], p);
    }
}

void bus_release_all() {
    for (unsigned i = 0; i < 2; ++i) {
        std::lock_guard<std::mutex> lock(s_i2c[i].mutex);
        release_i2c(s_i2c[i], i);
    }
    for (unsigned p = 1; p <= 2; ++p) {
        std::lock_guard<std::mutex> lock(s_uart[p].mutex);
        release_uart(s_uart[p], p);
    }
}

int32_t i2c_bus_write(uint32_t bus, uint8_t addr, const uint8_t* data, size_t len) {
    if (bus >= 2 || addr > 0x7f) return HAL_ERR_INVAL;
    I2cSlot& s = s_i2c[bus];
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.state.installed) return HAL_ERR_NODEV;
#if defined(ESP_PLATFORM)
    if (len == 0) return esp_to_hal(i2c_master_probe(s.bus, addr, 50));
    if (s.dev_addr != addr) {
        if (s.dev) i2c_master_bus_rm_device(s.dev);
        s.dev = nullptr;
        s.dev_addr = 0xffff;
        i2c_device_config_t dc = {};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dc.device_address = addr;
        dc.scl_speed_hz = s.state.speed;
        esp_err_t e = i2c_master_bus_add_device(s.bus, &dc, &s.dev);
        if (e != ESP_OK) { s.dev = nullptr; return esp_to_hal(e); }
        s.dev_addr = addr;
    }
    esp_err_t e = i2c_master_transmit(s.dev, data, len, 100);
    return e == ESP_OK ? static_cast<int32_t>(len) : i2c_failed(s, bus, addr, e);
#else
    static const uint8_t none = 0;
    int32_t r = MockHal::instance().i2c_write(bus, addr, len ? data : &none, len);
    if (len == 0) return r == HAL_ERR_NODEV ? HAL_ERR_NODEV : HAL_OK;
    return r;
#endif
}

int32_t i2c_bus_read(uint32_t bus, uint8_t addr, uint8_t* buf, size_t len) {
    if (bus >= 2 || addr > 0x7f || len == 0) return HAL_ERR_INVAL;
    I2cSlot& s = s_i2c[bus];
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.state.installed) return HAL_ERR_NODEV;
#if defined(ESP_PLATFORM)
    if (s.dev_addr != addr) {
        if (s.dev) i2c_master_bus_rm_device(s.dev);
        s.dev = nullptr;
        s.dev_addr = 0xffff;
        i2c_device_config_t dc = {};
        dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dc.device_address = addr;
        dc.scl_speed_hz = s.state.speed;
        esp_err_t e = i2c_master_bus_add_device(s.bus, &dc, &s.dev);
        if (e != ESP_OK) { s.dev = nullptr; return esp_to_hal(e); }
        s.dev_addr = addr;
    }
    esp_err_t e = i2c_master_receive(s.dev, buf, len, 100);
    return e == ESP_OK ? static_cast<int32_t>(len) : i2c_failed(s, bus, addr, e);
#else
    return MockHal::instance().i2c_read(bus, addr, buf, len);
#endif
}

int32_t uart_port_write(uint32_t port, const uint8_t* data, size_t len) {
    if (port < 1 || port > 2) return HAL_ERR_INVAL;
    UartSlot& s = s_uart[port];
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.state.installed) return HAL_ERR_NODEV;
#if defined(ESP_PLATFORM)
    int n = uart_write_bytes(static_cast<uart_port_t>(port), data, len);
    return n >= 0 ? n : HAL_ERR_IO;
#else
    return MockHal::instance().uart_write(port, data, len);
#endif
}

int32_t uart_port_read(uint32_t port, uint8_t* buf, size_t max_len, uint32_t timeout_ms) {
    if (port < 1 || port > 2) return HAL_ERR_INVAL;
    UartSlot& s = s_uart[port];
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.state.installed) return HAL_ERR_NODEV;
#if defined(ESP_PLATFORM)
    int n = uart_read_bytes(static_cast<uart_port_t>(port), buf, max_len, pdMS_TO_TICKS(timeout_ms));
    return n >= 0 ? n : HAL_ERR_IO;
#else
    (void)timeout_ms;
    return MockHal::instance().uart_read(port, buf, max_len);
#endif
}

BusState i2c_bus_state(uint32_t bus) {
    if (bus >= 2) return BusState{};
    std::lock_guard<std::mutex> lock(s_i2c[bus].mutex);
    return s_i2c[bus].state;
}

BusState uart_port_state(uint32_t port) {
    if (port < 1 || port > 2) return BusState{};
    std::lock_guard<std::mutex> lock(s_uart[port].mutex);
    return s_uart[port].state;
}

} // namespace kube32::hal
