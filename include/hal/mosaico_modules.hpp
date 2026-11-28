#pragma once

// ESP-Mosaico expansion module manager bridge for kube32:
//   - Left slot (H2, EEPROM 0x50): Camera Module (OV3640 / SC101IOT DVP via mosaico_module_camera)
//   - Right slot (H1, EEPROM 0x51): Interaction Module (6x WS2812, AS312 PIR, GL5506 LDR,
//     38 kHz NEC IR TX, 2x touch/GPIO buttons via mosaico_module_interact)
//
// Safe on all targets: on non-ESP32-S31 builds (including the host simulator),
// discovery reports no attached modules and operations return HAL_ERR_NODEV.

#include "hal/hal_types.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kube32::hal {

class Board;
struct BoardProfile;

struct MosaicoSlotStatus {
    bool present{false};
    bool descriptor_valid{false};
    uint8_t board_type{0};
    uint16_t board_id{0};
    uint16_t hw_version{0};
    uint8_t eeprom_addr{0};
    char board_name[33]{};
    const char* type_name{"none"};
};

struct MosaicoInteractInputs {
    bool left_pressed{false};
    bool right_pressed{false};
    bool motion_detected{false};
    int32_t light_raw{0};
    uint8_t light_level{0};   // 0..100 filtered relative level
    uint8_t led_count{0};     // 6 when attached
    uint8_t button_mode{0};   // 1 = GPIO, 2 = TOUCH
    uint8_t slot{0};          // 0 = left, 1 = right
};

struct MosaicoCameraInfo {
    bool present{false};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t pixel_format{0}; // V4L2 FOURCC
    char sensor[16]{"unknown"};
};

// Sentinel LED index to fill all LEDs on the Interaction Module at once.
constexpr uint32_t kMosaicoLedAll = 0xFFFFFFFFu;
// Safe brightness cap (0..255) for the 6x WS2812 strip (~19% peak current).
constexpr uint8_t kMosaicoLedSafeBrightness = 48;

class MosaicoModules {
public:
    static MosaicoModules& instance();

    // Starts mosaico_module_mgr, waits briefly for EEPROM debounce on left and
    // right slots, initializes any attached Interaction / Camera modules, and
    // registers an esp_register_shutdown_handler() for pre-reboot DMA cleanup.
    // Always returns true unless the module manager itself could not start;
    // unplugged modules are normal and never fail boot.
    bool init();

    // Stops camera streaming, returns any borrowed frames, closes/deletes the
    // camera DVP pipeline, and clears Interaction LEDs. Safe to call multiple
    // times; invoked automatically before esp_restart().
    void shutdown();

    bool initialized() const;
    bool has_camera() const;
    bool has_interact() const;

    // Slot snapshots from boot/latest scan (0 = left, 1 = right).
    MosaicoSlotStatus slot_status(uint32_t slot) const;

    // GPIOs owned by subboard discovery and any attached modules, to OR into
    // BoardProfile::reserved_gpio_mask.
    uint64_t reserved_gpio_mask() const;

    // RMT TX channels consumed by attached modules (2 when Interaction Module
    // is present: WS2812 + lazy 38 kHz NEC IR TX).
    uint8_t rmt_tx_board_channels() const;

    // Node labels to publish on the Kubernetes Node object:
    //   hardware.kube32.io/camera=true (if present)
    //   hardware.kube32.io/interact=true (if present)
    //   mosaico.kube32.io/slot-left=<type>
    //   mosaico.kube32.io/slot-right=<type>
    std::map<std::string, std::string> node_labels() const;

    // Interaction Module operations (thread-safe).
    // led_set: index 0..5 sets one LED, kMosaicoLedAll fills all 6. rgb = 0xRRGGBB.
    int32_t led_set(uint32_t index, uint32_t rgb);
    int32_t led_clear();
    int32_t interact_read(MosaicoInteractInputs& out);
    int32_t interact_ir_send_nec(uint8_t address, uint8_t command);

    // Camera Module operations (thread-safe; opens/streams on demand and closes
    // immediately after capture so zero DVP DMA and zero ~3.5 MB PSRAM buffers
    // sit idle between snapshots).
    int32_t camera_info(MosaicoCameraInfo& out);
    int32_t camera_capture_jpeg(std::vector<uint8_t>& out_jpeg, MosaicoCameraInfo* out_info = nullptr, uint8_t quality = 75);
    int32_t camera_preview_to_display(Board& board);

    // Diagnostics for serial console (K32_CMD:MODULES / CAMERA / INTERACT).
    void print_status() const;
    int32_t self_test_interact();
    int32_t self_test_camera(Board* board = nullptr);

    // Populates BoardProfile with attached module capabilities, reserved GPIOs,
    // RMT TX channels, and camera SCCB I2C reservations.
    void describe(BoardProfile& profile) const;

private:
    MosaicoModules() = default;
    MosaicoModules(const MosaicoModules&) = delete;
    MosaicoModules& operator=(const MosaicoModules&) = delete;
};

// Free-function hooks for Board (board_kit.cpp) and main_esp32.cpp:
bool mosaico_modules_init();
void mosaico_modules_shutdown();
void mosaico_modules_describe(BoardProfile& profile);
int32_t mosaico_modules_led_set(uint32_t index, uint32_t rgb);
void mosaico_modules_labels(std::map<std::string, std::string>& labels);

}  // namespace kube32::hal
