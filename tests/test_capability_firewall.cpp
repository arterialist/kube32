#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace kube32::hal;

void test_device_grant_bitmask() {
    DeviceGrant grant;
    assert(sizeof(DeviceGrant) == 20);

    // GPIO read/write bits
    grant.gpio_read_mask |= (1ULL << 4);
    grant.gpio_write_mask |= (1ULL << 4);
    grant.gpio_read_mask |= (1ULL << 5);

    assert(grant.is_gpio_permitted(4, AccessMode::Read));
    assert(grant.is_gpio_permitted(4, AccessMode::Write));
    assert(grant.is_gpio_permitted(4, AccessMode::ReadWrite));
    assert(grant.is_gpio_permitted(5, AccessMode::Read));
    assert(!grant.is_gpio_permitted(5, AccessMode::Write));
    assert(!grant.is_gpio_permitted(6, AccessMode::Read));

    // ADC1 channel bits
    grant.adc1_channel_mask |= (1U << 3);
    assert(grant.is_adc1_permitted(3));
    assert(!grant.is_adc1_permitted(4));

    // I2C bus bits
    grant.i2c_bus_mask |= (1U << 1);
    assert(grant.is_i2c_permitted(1));
    assert(!grant.is_i2c_permitted(0));

    // UART port bits
    grant.uart_port_mask |= (1U << 2);
    assert(grant.is_uart_permitted(2));
    assert(!grant.is_uart_permitted(1));

    std::cout << "[PASS] test_device_grant_bitmask" << std::endl;
}

void test_specific_gpio_grant_and_firewall() {
    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    std::string manifest = R"({
        "spec": {
            "containers": [{
                "name": "c1",
                "resources": {
                    "requests": {
                        "esp32.io/gpio-2": "1",
                        "esp32.io/adc1-channel-0": "1",
                        "esp32.io/i2c-bus-0": "1",
                        "esp32.io/uart-1": "1"
                    }
                }
            }]
        }
    })";

    std::string err;
    bool ok = fw.register_pod("pod-1", manifest, err);
    assert(ok);
    assert(err.empty());

    // Check permissions
    assert(fw.check_permission("pod-1", PeripheralType::GPIO, 2, AccessMode::Write));
    assert(fw.check_permission("pod-1", PeripheralType::GPIO, 2, AccessMode::Read));
    assert(!fw.check_permission("pod-1", PeripheralType::GPIO, 4, AccessMode::Write)); // Unauthorized pin

    assert(fw.check_permission("pod-1", PeripheralType::ADC1, 0, AccessMode::Read));
    assert(!fw.check_permission("pod-1", PeripheralType::ADC1, 1, AccessMode::Read));

    assert(fw.check_permission("pod-1", PeripheralType::I2C, 0, AccessMode::Write));
    assert(!fw.check_permission("pod-1", PeripheralType::I2C, 1, AccessMode::Write));

    assert(fw.check_permission("pod-1", PeripheralType::UART, 1, AccessMode::Write));
    assert(!fw.check_permission("pod-1", PeripheralType::UART, 2, AccessMode::Write));

    std::cout << "[PASS] test_specific_gpio_grant_and_firewall" << std::endl;
}

void test_reserved_pin_protection() {
    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    // Pin 28 is SPI Flash/PSRAM
    std::string manifest_flash = R"({
        "spec": {
            "containers": [{
                "name": "c1",
                "resources": {
                    "requests": {
                        "esp32.io/gpio-28": "1"
                    }
                }
            }]
        }
    })";

    std::string err;
    bool ok = fw.register_pod("bad-pod-flash", manifest_flash, err);
    assert(!ok);
    assert(err.find("Security Violation") != std::string::npos);

    // Pin 43 is UART0 console
    std::string manifest_uart0 = R"({
        "spec": {
            "containers": [{
                "name": "c1",
                "resources": {
                    "requests": {
                        "esp32.io/gpio-43": "1"
                    }
                }
            }]
        }
    })";

    err.clear();
    ok = fw.register_pod("bad-pod-uart0", manifest_uart0, err);
    assert(!ok);
    assert(err.find("Security Violation") != std::string::npos);

    std::cout << "[PASS] test_reserved_pin_protection" << std::endl;
}

void test_collision_prevention() {
    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    std::string manifest1 = R"({
        "spec": {
            "containers": [{
                "name": "c1",
                "resources": { "requests": { "esp32.io/gpio-4": "1" } }
            }]
        }
    })";

    std::string manifest2 = R"({
        "spec": {
            "containers": [{
                "name": "c2",
                "resources": { "requests": { "esp32.io/gpio-4": "1" } }
            }]
        }
    })";

    std::string err;
    bool ok1 = fw.register_pod("pod-a", manifest1, err);
    assert(ok1);

    bool ok2 = fw.register_pod("pod-b", manifest2, err);
    assert(!ok2);
    assert(err.find("Hardware Collision") != std::string::npos);

    // After unregistering pod-a, pod-b can register!
    fw.unregister_pod("pod-a");
    err.clear();
    bool ok3 = fw.register_pod("pod-b", manifest2, err);
    assert(ok3);

    std::cout << "[PASS] test_collision_prevention" << std::endl;
}

void test_generic_resource_pools() {
    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    std::string manifest = R"({
        "spec": {
            "containers": [{
                "name": "c1",
                "resources": {
                    "requests": {
                        "esp.io/gpio": "2",
                        "esp.io/i2c": "1"
                    }
                }
            }]
        }
    })";

    std::string err;
    bool ok = fw.register_pod("generic-pod", manifest, err);
    assert(ok);

    uint64_t gpio_mask = fw.get_allocated_gpio_mask();
    uint8_t i2c_mask = fw.get_allocated_i2c_mask();

    // Verify 2 GPIOs and 1 I2C bus were allocated
    int count = 0;
    for (int i = 0; i < 64; ++i) {
        if ((gpio_mask & (1ULL << i)) != 0) count++;
    }
    assert(count == 2);
    assert(i2c_mask != 0);

    fw.unregister_pod("generic-pod");
    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);

    std::cout << "[PASS] test_generic_resource_pools" << std::endl;
}

void test_mock_hal_peripherals() {
    MockHal& hal = MockHal::instance();
    hal.reset();

    // 1. GPIO test
    assert(hal.gpio_set_mode(4, PinMode::Output) == HAL_OK);
    assert(hal.gpio_write(4, 1) == HAL_OK);
    assert(hal.gpio_read(4) == 1);
    assert(hal.get_gpio_write_count(4) == 1);
    assert(hal.gpio_write(4, 0) == HAL_OK);
    assert(hal.gpio_read(4) == 0);
    assert(hal.get_gpio_write_count(4) == 2);

    // 2. ADC1 test
    hal.set_mock_adc_value(2, 2450);
    assert(hal.adc1_read(2) == 2450);

    // 3. I2C BMP280 Sensor Emulation
    // Read chip ID from register 0xD0
    uint8_t reg_addr = 0xD0;
    int32_t wr_res = hal.i2c_write(0, 0x76, &reg_addr, 1);
    assert(wr_res == 1);

    uint8_t chip_id = 0;
    int32_t rd_res = hal.i2c_read(0, 0x76, &chip_id, 1);
    assert(rd_res == 1);
    assert(chip_id == 0x58); // BMP280 Chip ID

    // 4. UART Loopback
    const uint8_t msg[] = {'H', 'e', 'l', 'l', 'o'};
    assert(hal.uart_write(1, msg, 5) == 5);
    auto tx_bytes = hal.get_uart_tx_bytes(1);
    assert(tx_bytes.size() == 5);
    assert(std::memcmp(tx_bytes.data(), msg, 5) == 0);

    hal.inject_uart_rx_bytes(1, msg, 5);
    uint8_t rx_buf[10];
    int32_t read_bytes = hal.uart_read(1, rx_buf, sizeof(rx_buf));
    assert(read_bytes == 5);
    assert(std::memcmp(rx_buf, msg, 5) == 0);

    std::cout << "[PASS] test_mock_hal_peripherals" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Running Capability Firewall Unit Tests " << std::endl;
    std::cout << "========================================" << std::endl;

    test_device_grant_bitmask();
    test_specific_gpio_grant_and_firewall();
    test_reserved_pin_protection();
    test_collision_prevention();
    test_generic_resource_pools();
    test_mock_hal_peripherals();

    std::cout << "All Capability Firewall tests passed successfully!" << std::endl;
    return 0;
}
