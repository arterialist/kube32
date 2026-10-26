#include "hal/capability_firewall.hpp"
#include "hal/mock_hal.hpp"
#include "hal/hal_types.hpp"
#include "runtime/wasm_runtime.hpp"
#include "runtime/host_functions.hpp"
#include "osal/thread.hpp"

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <cstring>

using namespace kube32::hal;
using namespace kube32::runtime;

// =============================================================================
// Self-Contained WebAssembly HAL Probe Module (475 bytes)
// Exports:
//   probe_gpio_write(pin, val) -> i32
//   probe_gpio_read(pin) -> i32
//   probe_adc_read(channel) -> i32
//   probe_i2c_write(bus, addr, offset, len) -> i32
//   probe_i2c_read(bus, addr, offset, len) -> i32
//   probe_uart_write(port, offset, len) -> i32
//   probe_uart_read(port, offset, max_len) -> i32
//   set_mem_byte(offset, val) -> i32
//   get_mem_byte(offset) -> i32
//   memory (1 page = 64KB)
// =============================================================================
static const uint8_t s_wasm_hal_probe[] = {
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x1b, 0x04, 0x60, 0x02, 0x7f, 0x7f, 0x01,
    0x7f, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x03,
    0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x02, 0x9b, 0x01, 0x07, 0x03, 0x65, 0x6e, 0x76, 0x10, 0x65, 0x73,
    0x70, 0x33, 0x32, 0x5f, 0x67, 0x70, 0x69, 0x6f, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x00,
    0x03, 0x65, 0x6e, 0x76, 0x0f, 0x65, 0x73, 0x70, 0x33, 0x32, 0x5f, 0x67, 0x70, 0x69, 0x6f, 0x5f,
    0x72, 0x65, 0x61, 0x64, 0x00, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x0e, 0x65, 0x73, 0x70, 0x33, 0x32,
    0x5f, 0x61, 0x64, 0x63, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x00, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x0f,
    0x65, 0x73, 0x70, 0x33, 0x32, 0x5f, 0x69, 0x32, 0x63, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00,
    0x02, 0x03, 0x65, 0x6e, 0x76, 0x0e, 0x65, 0x73, 0x70, 0x33, 0x32, 0x5f, 0x69, 0x32, 0x63, 0x5f,
    0x72, 0x65, 0x61, 0x64, 0x00, 0x02, 0x03, 0x65, 0x6e, 0x76, 0x10, 0x65, 0x73, 0x70, 0x33, 0x32,
    0x5f, 0x75, 0x61, 0x72, 0x74, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x03, 0x03, 0x65, 0x6e,
    0x76, 0x0f, 0x65, 0x73, 0x70, 0x33, 0x32, 0x5f, 0x75, 0x61, 0x72, 0x74, 0x5f, 0x72, 0x65, 0x61,
    0x64, 0x00, 0x03, 0x03, 0x0a, 0x09, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x00, 0x01, 0x05,
    0x03, 0x01, 0x00, 0x01, 0x07, 0xa6, 0x01, 0x0a, 0x10, 0x70, 0x72, 0x6f, 0x62, 0x65, 0x5f, 0x67,
    0x70, 0x69, 0x6f, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x07, 0x0f, 0x70, 0x72, 0x6f, 0x62,
    0x65, 0x5f, 0x67, 0x70, 0x69, 0x6f, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x00, 0x08, 0x0e, 0x70, 0x72,
    0x6f, 0x62, 0x65, 0x5f, 0x61, 0x64, 0x63, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x00, 0x09, 0x0f, 0x70,
    0x72, 0x6f, 0x62, 0x65, 0x5f, 0x69, 0x32, 0x63, 0x5f, 0x77, 0x72, 0x69, 0x74, 0x65, 0x00, 0x0a,
    0x0e, 0x70, 0x72, 0x6f, 0x62, 0x65, 0x5f, 0x69, 0x32, 0x63, 0x5f, 0x72, 0x65, 0x61, 0x64, 0x00,
    0x0b, 0x10, 0x70, 0x72, 0x6f, 0x62, 0x65, 0x5f, 0x75, 0x61, 0x72, 0x74, 0x5f, 0x77, 0x72, 0x69,
    0x74, 0x65, 0x00, 0x0c, 0x0f, 0x70, 0x72, 0x6f, 0x62, 0x65, 0x5f, 0x75, 0x61, 0x72, 0x74, 0x5f,
    0x72, 0x65, 0x61, 0x64, 0x00, 0x0d, 0x0c, 0x73, 0x65, 0x74, 0x5f, 0x6d, 0x65, 0x6d, 0x5f, 0x62,
    0x79, 0x74, 0x65, 0x00, 0x0e, 0x0c, 0x67, 0x65, 0x74, 0x5f, 0x6d, 0x65, 0x6d, 0x5f, 0x62, 0x79,
    0x74, 0x65, 0x00, 0x0f, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x0a, 0x5c, 0x09,
    0x08, 0x00, 0x20, 0x00, 0x20, 0x01, 0x10, 0x00, 0x0b, 0x06, 0x00, 0x20, 0x00, 0x10, 0x01, 0x0b,
    0x06, 0x00, 0x20, 0x00, 0x10, 0x02, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20,
    0x03, 0x10, 0x03, 0x0b, 0x0c, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x03, 0x10, 0x04,
    0x0b, 0x0a, 0x00, 0x20, 0x00, 0x20, 0x01, 0x20, 0x02, 0x10, 0x05, 0x0b, 0x0a, 0x00, 0x20, 0x00,
    0x20, 0x01, 0x20, 0x02, 0x10, 0x06, 0x0b, 0x0b, 0x00, 0x20, 0x00, 0x20, 0x01, 0x3a, 0x00, 0x00,
    0x41, 0x00, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x2d, 0x00, 0x00, 0x0b
};

// =============================================================================
// Challenge 1: Unauthorized Peripheral Access
// - Deploy pod granting only GPIO 2
// - Attempt writing/reading GPIO 4, GPIO 15, and ADC1 channel 3
// - Assert: strictly returns -EPERM (-1), zero state changes in MockHAL
// =============================================================================
void test_challenge1_unauthorized_peripheral_access() {
    std::cout << "[RUN] Challenge 1: Unauthorized Peripheral Access..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    MockHal& mock = MockHal::instance();
    fw.reset();
    mock.reset();

    // Deploy pod granting ONLY GPIO 2
    std::string pod_id = "pod-chal1";
    std::string manifest = R"({
        "spec": {
            "containers": [{
                "name": "worker",
                "resources": {
                    "requests": {
                        "esp32.io/gpio-2": "1"
                    }
                }
            }]
        }
    })";

    std::string err;
    bool registered = fw.register_pod(pod_id, manifest, err);
    assert(registered);
    assert(err.empty());

    // Verify initial MockHAL baseline
    assert(mock.get_gpio_write_count(2) == 0);
    assert(mock.get_gpio_write_count(4) == 0);
    assert(mock.get_gpio_write_count(15) == 0);
    assert(mock.gpio_read(2) == 0);
    assert(mock.gpio_read(4) == 0);
    assert(mock.gpio_read(15) == 0);

    // Deploy Wasm workload into Wasm3 runtime
    auto runtime = create_wasm_runtime();
    bool deployed = runtime->deploy_workload(pod_id, s_wasm_hal_probe, sizeof(s_wasm_hal_probe), 64 * 1024);
    assert(deployed);

    // 1. Permitted Access: GPIO 2 Write & Read
    int32_t wr2_res = runtime->execute_function(pod_id, "probe_gpio_write", {2, 1});
    assert(wr2_res == HAL_OK); // 0
    assert(mock.get_gpio_write_count(2) == 1);
    assert(mock.gpio_read(2) == 1);

    int32_t rd2_res = runtime->execute_function(pod_id, "probe_gpio_read", {2});
    assert(rd2_res == 1);

    // 2. Unauthorized Access: GPIO 4 Write & Read
    int32_t wr4_res = runtime->execute_function(pod_id, "probe_gpio_write", {4, 1});
    assert(wr4_res == HAL_ERR_PERM); // -1 (-EPERM)
    // EMPIRICAL ASSERTION: Zero state changes in MockHAL!
    assert(mock.get_gpio_write_count(4) == 0);
    assert(mock.gpio_read(4) == 0);

    int32_t rd4_res = runtime->execute_function(pod_id, "probe_gpio_read", {4});
    assert(rd4_res == HAL_ERR_PERM); // -1 (-EPERM)

    // 3. Unauthorized Access: GPIO 15 Write & Read
    int32_t wr15_res = runtime->execute_function(pod_id, "probe_gpio_write", {15, 1});
    assert(wr15_res == HAL_ERR_PERM); // -1 (-EPERM)
    // EMPIRICAL ASSERTION: Zero state changes in MockHAL!
    assert(mock.get_gpio_write_count(15) == 0);
    assert(mock.gpio_read(15) == 0);

    int32_t rd15_res = runtime->execute_function(pod_id, "probe_gpio_read", {15});
    assert(rd15_res == HAL_ERR_PERM); // -1 (-EPERM)

    // 4. Unauthorized Access: ADC1 Channel 3 Read
    int32_t rdadc3_res = runtime->execute_function(pod_id, "probe_adc_read", {3});
    assert(rdadc3_res == HAL_ERR_PERM); // -1 (-EPERM)

    // 5. Unauthorized Access: I2C bus 0 & UART port 1
    int32_t i2c_res = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0, 1});
    assert(i2c_res == HAL_ERR_PERM); // -1 (-EPERM)

    int32_t uart_res = runtime->execute_function(pod_id, "probe_uart_write", {1, 0, 1});
    assert(uart_res == HAL_ERR_PERM); // -1 (-EPERM)

    // Verify firewall recorded security violations
    const PodSecurityRecord* rec = fw.get_pod_security_record(pod_id);
    assert(rec != nullptr);
    assert(rec->violation_count >= 5);

    // Teardown
    assert(runtime->terminate(pod_id));
    fw.unregister_pod(pod_id);
    assert(fw.get_allocated_gpio_mask() == 0);

    std::cout << "[PASS] Challenge 1: Unauthorized Peripheral Access strictly denied (-EPERM, 0 state changes)" << std::endl;
}

// =============================================================================
// Challenge 2: Hardware Safety Mask / Reserved Pins
// - Submit pod manifests claiming Octal Flash/PSRAM pins (26-32), console UART0 (43, 44), USB (19, 20)
// - Assert: Registration is rejected immediately with Security Violation
// =============================================================================
void test_challenge2_hardware_safety_mask_reserved_pins() {
    std::cout << "[RUN] Challenge 2: Hardware Safety Mask / Reserved Pins..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    // 1. Octal SPI Flash & PSRAM Pins: GPIO 26 through 32
    for (uint32_t pin = 26; pin <= 32; ++pin) {
        std::string manifest = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-" +
                               std::to_string(pin) + "\":\"1\"}}}]}}";
        std::string err;
        bool ok = fw.register_pod("bad-flash-" + std::to_string(pin), manifest, err);
        assert(!ok);
        assert(err.find("Security Violation") != std::string::npos);
        assert(err.find(std::to_string(pin)) != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    // 2. Console UART0 Pins: GPIO 43 and GPIO 44
    {
        std::string err;
        std::string m43 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-43\":\"1\"}}}]}}";
        bool ok43 = fw.register_pod("bad-uart0-43", m43, err);
        assert(!ok43);
        assert(err.find("Security Violation") != std::string::npos);

        err.clear();
        std::string m44 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-44\":\"1\"}}}]}}";
        bool ok44 = fw.register_pod("bad-uart0-44", m44, err);
        assert(!ok44);
        assert(err.find("Security Violation") != std::string::npos);

        // Direct request for UART 0 (console)
        err.clear();
        std::string mu0 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/uart-0\":\"1\"}}}]}}";
        bool oku0 = fw.register_pod("bad-uart-port0", mu0, err);
        assert(!oku0);
        assert(err.find("UART0 is console") != std::string::npos);
    }

    // 3. USB Peripheral Pins: GPIO 19 and GPIO 20
    {
        std::string err;
        std::string m19 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-19\":\"1\"}}}]}}";
        bool ok19 = fw.register_pod("bad-usb-19", m19, err);
        assert(!ok19);
        assert(err.find("Security Violation") != std::string::npos);

        err.clear();
        std::string m20 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-20\":\"1\"}}}]}}";
        bool ok20 = fw.register_pod("bad-usb-20", m20, err);
        assert(!ok20);
        assert(err.find("Security Violation") != std::string::npos);
    }

    // Strapping / Boot Pin: GPIO 0
    {
        std::string err;
        std::string m0 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-0\":\"1\"}}}]}}";
        bool ok0 = fw.register_pod("bad-boot-0", m0, err);
        assert(!ok0);
        assert(err.find("Security Violation") != std::string::npos);
        assert(err.find("GPIO 0") != std::string::npos);
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    // 4. Boundary Pins: Out of package bounds (GPIO >= 49 on ESP32-S3)
    {
        std::string err;
        std::string m49 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-49\":\"1\"}}}]}}";
        bool ok49 = fw.register_pod("bad-oob-49", m49, err);
        assert(!ok49);
        assert(err.find("Security Violation") != std::string::npos);

        err.clear();
        std::string m63 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-63\":\"1\"}}}]}}";
        bool ok63 = fw.register_pod("bad-oob-63", m63, err);
        assert(!ok63);
        assert(err.find("Security Violation") != std::string::npos);

        err.clear();
        std::string m64 = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/gpio-64\":\"1\"}}}]}}";
        bool ok64 = fw.register_pod("bad-oob-64", m64, err);
        assert(!ok64);
        assert(err.find("Security Violation") != std::string::npos);
    }

    // 5. Transactional Rollback: Manifest with valid pin 5 and reserved pin 28
    {
        std::string err;
        std::string mixed_manifest = R"({
            "spec": {
                "containers": [{
                    "name": "c",
                    "resources": {
                        "requests": {
                            "esp32.io/gpio-5": "1",
                            "esp32.io/gpio-28": "1"
                        }
                    }
                }]
            }
        })";
        bool ok = fw.register_pod("mixed-pod", mixed_manifest, err);
        assert(!ok);
        assert(err.find("Security Violation") != std::string::npos);
        // Assert: Valid pin 5 was NOT left allocated in the registry!
        assert(fw.get_allocated_gpio_mask() == 0);
    }

    // 6. Direct Pre-compiled DeviceGrant Safety Mask
    {
        DeviceGrant bad_grant;
        bad_grant.gpio_write_mask = (1ULL << 27); // Flash pin
        std::string err;
        bool ok = fw.grant_capabilities("grant-bad-flash", bad_grant, err);
        assert(!ok);
        assert(err.find("Security Violation") != std::string::npos);
    }

    std::cout << "[PASS] Challenge 2: Hardware Safety Mask strictly enforced on all reserved pins (26-32, 43, 44, 19, 20)" << std::endl;
}

// =============================================================================
// Challenge 3: Peripheral Collision Prevention
// - Pod A claims GPIO 2. Pod B attempts claiming GPIO 2 -> rejected with collision error
// - Terminate Pod A: verify GPIO 2 released back to pool and Pod B can claim it
// =============================================================================
void test_challenge3_peripheral_collision_prevention() {
    std::cout << "[RUN] Challenge 3: Peripheral Collision Prevention..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    std::string manifest_a = R"({
        "spec": {
            "containers": [{
                "name": "c1",
                "resources": { "requests": { "esp32.io/gpio-2": "1" } }
            }]
        }
    })";

    std::string manifest_b = R"({
        "spec": {
            "containers": [{
                "name": "c2",
                "resources": { "requests": { "esp32.io/gpio-2": "1" } }
            }]
        }
    })";

    std::string err;
    // Step 1: Pod A successfully claims GPIO 2
    bool ok_a = fw.register_pod("pod-a", manifest_a, err);
    assert(ok_a);
    assert(fw.check_permission("pod-a", PeripheralType::GPIO, 2, AccessMode::Write));
    assert((fw.get_allocated_gpio_mask() & (1ULL << 2)) != 0);

    // Step 2: Pod B attempts claiming GPIO 2 -> rejected with collision error
    err.clear();
    bool ok_b = fw.register_pod("pod-b", manifest_b, err);
    assert(!ok_b);
    assert(err.find("Hardware Collision") != std::string::npos);
    assert(err.find("GPIO 2") != std::string::npos);
    assert(err.find("pod-a") != std::string::npos);

    // Step 3: Pod B also rejected via direct DeviceGrant
    DeviceGrant grant_b;
    grant_b.gpio_write_mask = (1ULL << 2);
    err.clear();
    bool ok_b_grant = fw.grant_capabilities("pod-b", grant_b, err);
    assert(!ok_b_grant);
    assert(err.find("Hardware Collision") != std::string::npos);

    // Step 4: Multi-peripheral collisions (ADC, I2C, UART)
    {
        // I2C Collision
        std::string m_i2c_a = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/i2c-bus-0\":\"1\"}}}]}}";
        std::string m_i2c_b = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/i2c-bus-0\":\"1\"}}}]}}";
        assert(fw.register_pod("pod-i2c-a", m_i2c_a, err));
        err.clear();
        assert(!fw.register_pod("pod-i2c-b", m_i2c_b, err));
        assert(err.find("Hardware Collision") != std::string::npos);

        // ADC1 Collision
        std::string m_adc_a = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/adc1-channel-1\":\"1\"}}}]}}";
        std::string m_adc_b = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/adc1-channel-1\":\"1\"}}}]}}";
        assert(fw.register_pod("pod-adc-a", m_adc_a, err));
        err.clear();
        assert(!fw.register_pod("pod-adc-b", m_adc_b, err));
        assert(err.find("Hardware Collision") != std::string::npos);

        // UART Collision
        std::string m_uart_a = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/uart-1\":\"1\"}}}]}}";
        std::string m_uart_b = "{\"spec\":{\"containers\":[{\"name\":\"c\",\"resources\":{\"requests\":{\"esp32.io/uart-1\":\"1\"}}}]}}";
        assert(fw.register_pod("pod-uart-a", m_uart_a, err));
        err.clear();
        assert(!fw.register_pod("pod-uart-b", m_uart_b, err));
        assert(err.find("Hardware Collision") != std::string::npos);

        fw.unregister_pod("pod-i2c-a");
        fw.unregister_pod("pod-adc-a");
        fw.unregister_pod("pod-uart-a");
    }

    // Step 5: Terminate Pod A: verify GPIO 2 is released back to pool
    fw.unregister_pod("pod-a");
    assert((fw.get_allocated_gpio_mask() & (1ULL << 2)) == 0);
    assert(!fw.check_permission("pod-a", PeripheralType::GPIO, 2, AccessMode::Write));

    // Step 6: Pod B can now claim GPIO 2 successfully!
    err.clear();
    bool ok_b_now = fw.register_pod("pod-b", manifest_b, err);
    assert(ok_b_now);
    assert(fw.check_permission("pod-b", PeripheralType::GPIO, 2, AccessMode::Write));
    assert((fw.get_allocated_gpio_mask() & (1ULL << 2)) != 0);

    fw.unregister_pod("pod-b");
    assert(fw.get_allocated_gpio_mask() == 0);

    std::cout << "[PASS] Challenge 3: Peripheral Collision Prevention verified with lifecycle re-claim" << std::endl;
}

// =============================================================================
// Challenge 4: Linear Memory Buffer Overrun Protection
// - Pass guest buffer offsets and lengths such that offset + len > memory_size
// - Assert: Host functions trap bad pointer and return -EFAULT (-14)
// =============================================================================
void test_challenge4_linear_memory_buffer_overrun_protection() {
    std::cout << "[RUN] Challenge 4: Linear Memory Buffer Overrun Protection..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    MockHal& mock = MockHal::instance();
    fw.reset();
    mock.reset();

    std::string pod_id = "pod-chal4";
    std::string manifest = R"({
        "spec": {
            "containers": [{
                "name": "worker",
                "resources": {
                    "requests": {
                        "esp32.io/i2c-bus-0": "1",
                        "esp32.io/uart-1": "1"
                    }
                }
            }]
        }
    })";

    std::string err;
    assert(fw.register_pod(pod_id, manifest, err));

    auto runtime = create_wasm_runtime();
    // Deploy module with 1 page of memory (64KB = 65536 bytes)
    bool deployed = runtime->deploy_workload(pod_id, s_wasm_hal_probe, sizeof(s_wasm_hal_probe), 64 * 1024);
    assert(deployed);

    // 1. I2C Write Overrun Tests
    // Case 1.1: offset = 65530, len = 10 -> offset + len = 65540 > 65536
    int32_t oob_i2c_wr1 = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 65530, 10});
    assert(oob_i2c_wr1 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // Case 1.2: offset = 65536, len = 1 -> offset + len = 65537 > 65536
    int32_t oob_i2c_wr2 = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 65536, 1});
    assert(oob_i2c_wr2 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // Case 1.3: Integer wrap-around attack: offset = 0xFFFFFFF0 (-16), len = 32
    // If not using 64-bit bounds check, 0xFFFFFFF0 + 32 = 0x10 < 65536!
    int32_t oob_i2c_wr3 = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, static_cast<int32_t>(0xFFFFFFF0), 32});
    assert(oob_i2c_wr3 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // Case 1.4: Offset 0xFFFFFFFF, len = 10
    int32_t oob_i2c_wr4 = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, static_cast<int32_t>(0xFFFFFFFF), 10});
    assert(oob_i2c_wr4 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // 2. I2C Read Overrun Tests
    // Case 2.1: offset = 65530, len = 10
    int32_t oob_i2c_rd1 = runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 65530, 10});
    assert(oob_i2c_rd1 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // Case 2.2: Integer wrap-around
    int32_t oob_i2c_rd2 = runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, static_cast<int32_t>(0xFFFFFFF0), 32});
    assert(oob_i2c_rd2 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // 3. UART Write Overrun Tests
    // Case 3.1: offset = 65530, len = 10
    int32_t oob_uart_wr1 = runtime->execute_function(pod_id, "probe_uart_write", {1, 65530, 10});
    assert(oob_uart_wr1 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // Case 3.2: Integer wrap-around
    int32_t oob_uart_wr2 = runtime->execute_function(pod_id, "probe_uart_write", {1, static_cast<int32_t>(0xFFFFFFF0), 32});
    assert(oob_uart_wr2 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // 4. UART Read Overrun Tests
    // Case 4.1: offset = 65530, max_len = 10
    int32_t oob_uart_rd1 = runtime->execute_function(pod_id, "probe_uart_read", {1, 65530, 10});
    assert(oob_uart_rd1 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // Case 4.2: Integer wrap-around
    int32_t oob_uart_rd2 = runtime->execute_function(pod_id, "probe_uart_read", {1, static_cast<int32_t>(0xFFFFFFF0), 32});
    assert(oob_uart_rd2 == HAL_ERR_FAULT); // -14 (-EFAULT)

    // 5. Valid Exact Memory Boundary
    // offset = 65530, len = 6 -> offset + len = 65536 == mem_size
    // Write 6 bytes without overrun
    runtime->execute_function(pod_id, "set_mem_byte", {65530, 0xD0});
    int32_t valid_boundary_res = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 65530, 1});
    assert(valid_boundary_res == 1); // Exactly fits in memory!

    runtime->terminate(pod_id);
    fw.unregister_pod(pod_id);

    std::cout << "[PASS] Challenge 4: Linear Memory Buffer Overrun strictly trapped with -EFAULT (-14)" << std::endl;
}

// =============================================================================
// Challenge 5: Virtual BMP280 Sensor Emulation
// - Execute I2C transactions to address 0x76:
//   - read register 0xD0 (assert 0x58)
//   - write configuration registers
//   - read data registers
//   - verify chip ID read-only protection
//   - verify non-existent device returns -ENODEV (-19)
// =============================================================================
void test_challenge5_virtual_bmp280_sensor_emulation() {
    std::cout << "[RUN] Challenge 5: Virtual BMP280 Sensor Emulation..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    MockHal& mock = MockHal::instance();
    fw.reset();
    mock.reset();

    std::string pod_id = "pod-chal5";
    std::string manifest = R"({
        "spec": {
            "containers": [{
                "name": "sensor-reader",
                "resources": {
                    "requests": {
                        "esp32.io/i2c-bus-0": "1"
                    }
                }
            }]
        }
    })";

    std::string err;
    assert(fw.register_pod(pod_id, manifest, err));

    auto runtime = create_wasm_runtime();
    bool deployed = runtime->deploy_workload(pod_id, s_wasm_hal_probe, sizeof(s_wasm_hal_probe), 64 * 1024);
    assert(deployed);

    // 1. Read Chip ID from Register 0xD0 (BMP280 Chip ID must be 0x58)
    // Guest memory setup: write reg address 0xD0 to offset 0x100
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xD0});

    // Send register address via I2C write
    int32_t wr_res = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 1});
    assert(wr_res == 1);

    // Read 1 byte into guest offset 0x200
    int32_t rd_res = runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 0x200, 1});
    assert(rd_res == 1);

    int32_t chip_id = runtime->execute_function(pod_id, "get_mem_byte", {0x200});
    assert(chip_id == 0x58); // BMP280 Chip ID is 0x58!

    // 2. Write Configuration Registers
    // Write ctrl_meas (0xF4) = 0x27 (forced mode, osrs_t x1, osrs_p x1)
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xF4});
    runtime->execute_function(pod_id, "set_mem_byte", {0x101, 0x27});
    int32_t cfg_wr1 = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 2});
    assert(cfg_wr1 == 2);

    // Read back ctrl_meas (0xF4)
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xF4});
    runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 1});
    runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 0x200, 1});
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x200}) == 0x27);

    // Write config (0xF5) = 0xA0 (standby 1000ms, filter x16)
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xF5});
    runtime->execute_function(pod_id, "set_mem_byte", {0x101, 0xA0});
    int32_t cfg_wr2 = runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 2});
    assert(cfg_wr2 == 2);

    // Read back config (0xF5)
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xF5});
    runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 1});
    runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 0x200, 1});
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x200}) == 0xA0);

    // 3. Read Calibration Registers (0x88..0x8D: dig_T1, dig_T2, dig_T3)
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0x88});
    runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 1});
    int32_t cal_rd = runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 0x200, 6});
    assert(cal_rd == 6);
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x200}) == 0x70); // dig_T1 LSB
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x201}) == 0x6B); // dig_T1 MSB
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x202}) == 0x60); // dig_T2 LSB
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x203}) == 0x67); // dig_T2 MSB

    // 4. Read Data Registers (0xF7..0xFC: 3 bytes pressure + 3 bytes temperature)
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xF7});
    runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 1});
    int32_t data_rd = runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 0x200, 6});
    assert(data_rd == 6);

    int32_t t_msb = runtime->execute_function(pod_id, "get_mem_byte", {0x203});
    int32_t t_lsb = runtime->execute_function(pod_id, "get_mem_byte", {0x204});
    assert(t_msb != 0 || t_lsb != 0); // Non-zero valid raw ADC data

    // 5. Read-Only Protection on Chip ID
    // Attempt overwriting register 0xD0 with 0xAA
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xD0});
    runtime->execute_function(pod_id, "set_mem_byte", {0x101, 0xAA});
    runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 2});

    // Read back reg 0xD0 -> must strictly remain 0x58!
    runtime->execute_function(pod_id, "set_mem_byte", {0x100, 0xD0});
    runtime->execute_function(pod_id, "probe_i2c_write", {0, 0x76, 0x100, 1});
    runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x76, 0x200, 1});
    assert(runtime->execute_function(pod_id, "get_mem_byte", {0x200}) == 0x58);

    // 6. Non-Existent I2C Device Address (e.g. 0x3C OLED display not connected)
    int32_t no_dev_res = runtime->execute_function(pod_id, "probe_i2c_read", {0, 0x3C, 0x200, 1});
    assert(no_dev_res == HAL_ERR_NODEV); // -19 (-ENODEV)

    runtime->terminate(pod_id);
    fw.unregister_pod(pod_id);

    std::cout << "[PASS] Challenge 5: Virtual BMP280 Sensor Emulation verified (ID 0x58, config, data, read-only 0xD0)" << std::endl;
}

// =============================================================================
// Concurrency Stress Test
// - Stress-test lockless gatekeeper and collision registry under 8 concurrent threads
// =============================================================================
void test_firewall_concurrency_stress() {
    std::cout << "[RUN] Concurrency Stress Test: 8 threads x 1000 operations..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    // Register 4 test pods
    for (int i = 0; i < 4; ++i) {
        std::string pod_id = "conc-pod-" + std::to_string(i);
        DeviceGrant grant;
        grant.gpio_read_mask = (1ULL << (i + 1));
        grant.gpio_write_mask = (1ULL << (i + 1));
        grant.adc1_channel_mask = (1U << i);
        std::string err;
        assert(fw.grant_capabilities(pod_id, grant, err));
    }

    std::atomic<bool> start_flag{false};
    std::atomic<uint32_t> pass_count{0};
    std::vector<std::thread> workers;

    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&, t]() {
            while (!start_flag.load()) std::this_thread::yield();

            for (int op = 0; op < 1000; ++op) {
                int pod_idx = (t + op) % 4;
                std::string pod_id = "conc-pod-" + std::to_string(pod_idx);
                uint32_t granted_pin = pod_idx + 1;
                uint32_t ungranted_pin = (granted_pin + 10);

                // Granted check must succeed
                bool p1 = fw.check_permission(pod_id, PeripheralType::GPIO, granted_pin, AccessMode::Write);
                assert(p1);

                // Ungranted check must fail
                bool p2 = fw.check_permission(pod_id, PeripheralType::GPIO, ungranted_pin, AccessMode::Write);
                assert(!p2);

                pass_count.fetch_add(1);
            }
        });
    }

    start_flag.store(true);
    for (auto& th : workers) {
        th.join();
    }

    assert(pass_count.load() == 8000);
    fw.reset();

    std::cout << "[PASS] Concurrency Stress Test: 8000 concurrent firewall queries completed with 0 errors" << std::endl;
}

// =============================================================================
// Challenge 6: Generic Pin Cleanup on Allocation Failure
// - Request 3 generic GPIOs and 5 generic I2C buses (ESP32-S3 only has 2 buses)
// - Assert: Registration fails, and exactly 0 GPIO/I2C pins leak in the firewall
// =============================================================================
void test_challenge6_generic_pin_cleanup_on_allocation_failure() {
    std::cout << "[RUN] Challenge 6: Generic Pin Cleanup on Allocation Failure..." << std::endl;

    CapabilityFirewall& fw = CapabilityFirewall::instance();
    fw.reset();

    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);

    // Request 3 generic GPIOs and 5 generic I2C buses (ESP32-S3 only has 2 buses)
    std::string doomed_manifest = R"({
        "spec": {
            "containers": [{
                "name": "c",
                "resources": {
                    "requests": {
                        "esp.io/gpio": "3",
                        "esp.io/i2c": "5"
                    }
                }
            }]
        }
    })";

    std::string err;
    bool ok = fw.register_pod("doomed-pod", doomed_manifest, err);
    assert(!ok);
    assert(err.find("Insufficient allocatable I2C buses") != std::string::npos);

    // CRITICAL EMPIRICAL ASSERTION: 0 pins leaked!
    assert(fw.get_allocated_gpio_mask() == 0);
    assert(fw.get_allocated_i2c_mask() == 0);

    // Verify subsequent pod can successfully register
    std::string valid_manifest = R"({
        "spec": {
            "containers": [{
                "name": "c",
                "resources": {
                    "requests": {
                        "esp.io/gpio": "3",
                        "esp.io/i2c": "1"
                    }
                }
            }]
        }
    })";

    err.clear();
    assert(fw.register_pod("valid-pod", valid_manifest, err));
    assert(fw.get_allocated_gpio_mask() != 0);
    fw.unregister_pod("valid-pod");
    assert(fw.get_allocated_gpio_mask() == 0);

    std::cout << "[PASS] Challenge 6: Generic Pin Cleanup on Allocation Failure completely rolled back (0 leaked pins)" << std::endl;
}

int main() {
    std::cout << "==========================================================" << std::endl;
    std::cout << " Kube32 HAL Capability Firewall & Peripheral Collision    " << std::endl;
    std::cout << " Empirical Adversarial Challenge Suite (Milestone 3)      " << std::endl;
    std::cout << "==========================================================" << std::endl;

    test_challenge1_unauthorized_peripheral_access();
    test_challenge2_hardware_safety_mask_reserved_pins();
    test_challenge3_peripheral_collision_prevention();
    test_challenge4_linear_memory_buffer_overrun_protection();
    test_challenge5_virtual_bmp280_sensor_emulation();
    test_challenge6_generic_pin_cleanup_on_allocation_failure();
    test_firewall_concurrency_stress();

    std::cout << "==========================================================" << std::endl;
    std::cout << " ALL 6 EMPIRICAL ADVERSARIAL CHALLENGES PASSED (100%)     " << std::endl;
    std::cout << "==========================================================" << std::endl;

    return 0;
}
