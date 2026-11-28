#include "hal/mosaico_modules.hpp"
#include "hal/board.hpp"
#include "hal/capability_firewall.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(KUBE32_TARGET_ESP32) && CONFIG_IDF_TARGET_ESP32S31

#include "bsp/display.h"
#include "bsp/subboard.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "mosaico_module_camera.h"
#include "mosaico_module_interact.h"
#include "mosaico_module_mgr.h"

namespace kube32::hal {

namespace {

constexpr const char* TAG = "MosaicoMod";
constexpr uint32_t kDiscoveryWaitMs = 1500;
constexpr uint32_t kDiscoveryPollMs = 100;
constexpr uint32_t kCameraFrameTimeoutMs = 1500;
constexpr uint32_t kCameraCaptureRetries = 6;
constexpr uint32_t kJpegTimeoutMs = 1000;
constexpr size_t kPreviewAlign = 128;
constexpr uint32_t kPreviewCropMax = 640;

std::mutex s_interact_mutex;
std::mutex s_camera_mutex;
std::mutex s_state_mutex;

std::atomic<bool> s_initialized{false};
std::atomic<bool> s_shutting_down{false};
std::atomic<bool> s_shutdown_registered{false};
std::atomic<bool> s_has_camera{false};
std::atomic<bool> s_has_interact{false};

mosaico_interact_handle_t s_interact{nullptr};
mosaico_interact_info_t s_interact_info{};
BaseType_t s_interact_core{0};
mosaico_camera_handle_t s_camera{nullptr};
MosaicoCameraInfo s_cam_info{};
MosaicoSlotStatus s_slots[MOSAICO_MODULE_MGR_SLOT_COUNT]{};

inline uint64_t pin_bit(int p) { return (p >= 0 && p < 64) ? (1ULL << p) : 0ULL; }
inline size_t align_up(size_t v, size_t a) { return (v + a - 1U) & ~(a - 1U); }
inline uint32_t align_down_even(uint32_t v) { return v & ~1U; }

const char* slot_label_type(uint8_t board_type) {
    switch (static_cast<mosaico_board_type_t>(board_type)) {
        case MOSAICO_BOARD_TYPE_CAMERA:     return "camera";
        case MOSAICO_BOARD_TYPE_INTERACT:   return "interact";
        case MOSAICO_BOARD_TYPE_SENSOR:     return "sensor";
        case MOSAICO_BOARD_TYPE_TOF:        return "tof";
        case MOSAICO_BOARD_TYPE_MATRIX_LED: return "matrix-led";
        case MOSAICO_BOARD_TYPE_THERMAL:    return "thermal";
        case MOSAICO_BOARD_TYPE_RELAY:      return "relay";
        case MOSAICO_BOARD_TYPE_IO_EXP:     return "io-exp";
        default:                            return "unknown";
    }
}

void copy_slot_info(const mosaico_module_mgr_info_t& src, MosaicoSlotStatus& dst) {
    dst = {};
    dst.present = (src.presence == MOSAICO_MODULE_PRESENCE_PRESENT);
    dst.descriptor_valid = (src.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID);
    dst.eeprom_addr = src.eeprom_addr;
    if (dst.descriptor_valid) {
        dst.board_type = src.eeprom.board_type;
        dst.board_id = src.eeprom.board_id;
        dst.hw_version = src.eeprom.hw_version;
        std::memcpy(dst.board_name, src.eeprom.board_name, 32);
        dst.board_name[32] = '\0';
        for (int i = 31; i >= 0 && (dst.board_name[i] == ' ' || dst.board_name[i] == '\0' || static_cast<uint8_t>(dst.board_name[i]) == 0xFF); --i) {
            dst.board_name[i] = '\0';
        }
        dst.type_name = mosaico_module_mgr_type_to_name(static_cast<mosaico_board_type_t>(dst.board_type));
    } else if (dst.present) {
        dst.type_name = "unidentified";
    } else {
        dst.type_name = "none";
    }
}

void refresh_slot_snapshots() {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    for (uint32_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
        mosaico_module_mgr_info_t info{};
        if (mosaico_module_mgr_get_info(static_cast<mosaico_module_mgr_slot_t>(i), &info) == ESP_OK) {
            copy_slot_info(info, s_slots[i]);
        }
    }
}

void mosaico_shutdown_hook() {
    MosaicoModules::instance().shutdown();
}

// Acquire one valid UYVY or JPEG frame from an open, streaming camera.
// Caller MUST return the frame with mosaico_camera_return_frame() on ESP_OK.
esp_err_t grab_valid_frame(mosaico_camera_handle_t cam, mosaico_camera_frame_t& out_frame) {
    esp_err_t last_err = ESP_ERR_TIMEOUT;
    for (uint32_t attempt = 0; attempt < kCameraCaptureRetries; ++attempt) {
        mosaico_camera_frame_t frame{};
        esp_err_t ret = mosaico_camera_get_frame(cam, &frame);
        if (ret == ESP_ERR_INVALID_SIZE) {
            last_err = ret;
            continue;
        }
        if (ret != ESP_OK) {
            last_err = ret;
            continue;
        }
        const bool ok = frame.data != nullptr && frame.size > 0 &&
                        (frame.pixel_format == V4L2_PIX_FMT_JPEG ||
                         frame.size >= static_cast<size_t>(frame.width) * frame.height * 2U);
        if (!ok) {
            (void)mosaico_camera_return_frame(cam, &frame);
            last_err = ESP_ERR_INVALID_SIZE;
            continue;
        }
        out_frame = frame;
        return ESP_OK;
    }
    return last_err;
}

void update_cached_camera_info(const mosaico_camera_info_t& info) {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    s_cam_info.present = true;
    s_cam_info.width = info.width;
    s_cam_info.height = info.height;
    s_cam_info.pixel_format = info.pixel_format;
    if (s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].board_name[0] != '\0') {
        std::snprintf(s_cam_info.sensor, sizeof(s_cam_info.sensor), "%.15s",
                      s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].board_name);
    } else if (info.width == 1280 && info.height == 720) {
        std::snprintf(s_cam_info.sensor, sizeof(s_cam_info.sensor), "DVP-720p");
    } else {
        std::snprintf(s_cam_info.sensor, sizeof(s_cam_info.sensor), "DVP");
    }
}

}  // namespace

MosaicoModules& MosaicoModules::instance() {
    static MosaicoModules inst;
    return inst;
}

bool MosaicoModules::init() {
    if (s_initialized.load()) return true;
    s_shutting_down.store(false);

    if (!s_shutdown_registered.exchange(true)) {
        esp_register_shutdown_handler(mosaico_shutdown_hook);
    }

    esp_err_t ret = mosaico_module_mgr_init(nullptr);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mosaico_module_mgr_init failed: %s (expansion modules unavailable)", esp_err_to_name(ret));
        return false;
    }

    // Wait for initial debounce (3 x 250ms = ~750ms) and EEPROM descriptor read
    // so Board::describe() and Node registration see both attached modules on boot.
    const TickType_t start = xTaskGetTickCount();
    const TickType_t budget = pdMS_TO_TICKS(kDiscoveryWaitMs);
    while ((xTaskGetTickCount() - start) < budget) {
        bool all_settled = true;
        for (uint32_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            mosaico_module_mgr_info_t info{};
            if (mosaico_module_mgr_get_info(static_cast<mosaico_module_mgr_slot_t>(i), &info) != ESP_OK) {
                all_settled = false;
                break;
            }
            if (info.presence == MOSAICO_MODULE_PRESENCE_UNKNOWN) {
                all_settled = false;
                break;
            }
            if (info.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
                info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_UNKNOWN) {
                all_settled = false;
                break;
            }
        }
        if (all_settled) break;
        vTaskDelay(pdMS_TO_TICKS(kDiscoveryPollMs));
    }

    refresh_slot_snapshots();

    bool found_camera = false;
    bool found_interact = false;
    mosaico_module_mgr_slot_t interact_slot = MOSAICO_MODULE_MGR_SLOT_AUTO;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        for (uint32_t i = 0; i < MOSAICO_MODULE_MGR_SLOT_COUNT; ++i) {
            const auto& sl = s_slots[i];
            ESP_LOGI(TAG, "Slot %s (EEPROM 0x%02X): present=%d valid=%d type=%s(0x%02X) name='%s'",
                     mosaico_module_mgr_slot_to_name(static_cast<mosaico_module_mgr_slot_t>(i)),
                     sl.eeprom_addr, sl.present ? 1 : 0, sl.descriptor_valid ? 1 : 0,
                     sl.type_name, sl.board_type, sl.board_name);
            if (sl.present && sl.descriptor_valid) {
                if (i == MOSAICO_MODULE_MGR_SLOT_LEFT && sl.board_type == MOSAICO_BOARD_TYPE_CAMERA) {
                    found_camera = true;
                }
                if (sl.board_type == MOSAICO_BOARD_TYPE_INTERACT) {
                    found_interact = true;
                    interact_slot = static_cast<mosaico_module_mgr_slot_t>(i);
                }
            }
        }
    }

    // 1. Interaction Module (right slot on standard kit, zero conflict with ES8311 I2S)
    if (found_interact) {
        std::lock_guard<std::mutex> lock(s_interact_mutex);
        mosaico_interact_config_t cfg = MOSAICO_INTERACT_DEFAULT_CONFIG();
        cfg.slot = interact_slot;
        cfg.discovery_timeout_ms = 1000;
        cfg.button_mode = MOSAICO_INTERACT_BUTTON_MODE_AUTO;
        cfg.led_brightness = kMosaicoLedSafeBrightness;
        esp_err_t iret = mosaico_interact_open(&cfg, &s_interact);
        if (iret == ESP_OK && s_interact != nullptr) {
            s_interact_core = xPortGetCoreID();
            (void)mosaico_interact_get_info(s_interact, &s_interact_info);
            (void)mosaico_interact_led_clear(s_interact);
            s_has_interact.store(true);
            ESP_LOGI(TAG, "Interaction module ready: slot=%s leds=%u btn_mode=%s brightness_cap=%u/255",
                     mosaico_module_mgr_slot_to_name(s_interact_info.slot),
                     s_interact_info.led_count,
                     s_interact_info.button_mode == MOSAICO_INTERACT_BUTTON_MODE_TOUCH ? "touch" : "gpio",
                     kMosaicoLedSafeBrightness);
        } else {
            ESP_LOGW(TAG, "Interaction module detected but open failed: %s", esp_err_to_name(iret));
            if (s_interact) {
                (void)mosaico_interact_close(s_interact);
                s_interact = nullptr;
            }
        }
    }

    // 2. Camera Module (left slot only). Register /dev/video2 and leave sensor
    // asleep (PWDN=1) with zero frame buffers allocated until capture/preview.
    if (found_camera) {
        std::lock_guard<std::mutex> lock(s_camera_mutex);
        mosaico_camera_config_t ccfg = MOSAICO_CAMERA_DEFAULT_CONFIG();
        ccfg.slot = MOSAICO_MODULE_MGR_SLOT_LEFT;
        ccfg.width = 0;
        ccfg.height = 0;
        ccfg.pixel_format = V4L2_PIX_FMT_UYVY;
        ccfg.buffer_count = 2;
        ccfg.frame_timeout_ms = kCameraFrameTimeoutMs;
        ccfg.discovery_timeout_ms = 1000;
        ccfg.apply_module_tuning = true;
        ccfg.allow_unidentified = false;
        esp_err_t cret = mosaico_camera_new(&ccfg, &s_camera);
        if (cret == ESP_OK && s_camera != nullptr) {
            s_has_camera.store(true);
            {
                std::lock_guard<std::mutex> slock(s_state_mutex);
                s_cam_info.present = true;
                s_cam_info.pixel_format = V4L2_PIX_FMT_UYVY;
                if (s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].board_name[0] != '\0') {
                    std::snprintf(s_cam_info.sensor, sizeof(s_cam_info.sensor), "%.15s",
                                  s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].board_name);
                }
            }
            ESP_LOGI(TAG, "Camera module registered on left slot (sensor asleep until capture)");
        } else {
            ESP_LOGW(TAG, "Camera module detected but mosaico_camera_new failed: %s", esp_err_to_name(cret));
            if (s_camera) {
                (void)mosaico_camera_del(s_camera);
                s_camera = nullptr;
            }
        }
    }

    s_initialized.store(true);
    return true;
}

void MosaicoModules::shutdown() {
    if (s_shutting_down.exchange(true)) return;
    ESP_LOGI(TAG, "Pre-reset module cleanup (stopping camera DVP DMA & clearing LEDs)");

    // Camera teardown: MUST stop stream, close, and delete before esp_restart()
    // to avoid wedging DVP DMA across soft resets.
    {
        std::lock_guard<std::mutex> lock(s_camera_mutex);
        if (s_camera) {
            (void)mosaico_camera_stop_stream(s_camera);
            (void)mosaico_camera_close(s_camera);
            (void)mosaico_camera_del(s_camera);
            s_camera = nullptr;
        }
        s_has_camera.store(false);
    }

    // Interaction teardown: turn LEDs off and release resources cleanly.
    {
        std::lock_guard<std::mutex> lock(s_interact_mutex);
        if (s_interact) {
            (void)mosaico_interact_led_clear(s_interact);
            (void)mosaico_interact_close(s_interact);
            s_interact = nullptr;
        }
        s_has_interact.store(false);
    }
}

bool MosaicoModules::initialized() const { return s_initialized.load(); }
bool MosaicoModules::has_camera() const { return s_has_camera.load() && !s_shutting_down.load(); }
bool MosaicoModules::has_interact() const { return s_has_interact.load() && !s_shutting_down.load(); }

MosaicoSlotStatus MosaicoModules::slot_status(uint32_t slot) const {
    if (slot >= MOSAICO_MODULE_MGR_SLOT_COUNT) return {};
    std::lock_guard<std::mutex> lock(s_state_mutex);
    return s_slots[slot];
}

uint64_t MosaicoModules::reserved_gpio_mask() const {
    uint64_t mask = 0;
    if (!s_initialized.load()) return 0;
    // Subboard I2C1 (SDA=0, SCL=1) and slot EEPROM address-select lines (14, 39)
    mask |= pin_bit(0) | pin_bit(1) | pin_bit(14) | pin_bit(39);
    if (s_has_camera.load()) {
        // Left-slot DVP camera pins: D0..D7 (16,15,33,4,14,12,18,13),
        // VSYNC=55, DE=19, PCLK=17, RESET=53, PWDN=48, FLASH=34
        const int cam_pins[] = {4, 12, 13, 14, 15, 16, 17, 18, 19, 33, 34, 48, 53, 55};
        for (int p : cam_pins) mask |= pin_bit(p);
    }
    if (s_has_interact.load()) {
        // Right-slot Interaction module pins (or left-slot equivalents via map):
        const bsp_subboard_slot_t slot = static_cast<bsp_subboard_slot_t>(s_interact_info.slot);
        const gpio_num_t canon[] = {GPIO_NUM_53, GPIO_NUM_48, GPIO_NUM_13, GPIO_NUM_12, GPIO_NUM_4, GPIO_NUM_15};
        for (gpio_num_t g : canon) {
            mask |= pin_bit(static_cast<int>(bsp_subboard_map_gpio(slot, g)));
        }
    }
    return mask;
}

uint8_t MosaicoModules::rmt_tx_board_channels() const {
    // 1 RMT TX channel for 6x WS2812 + 1 lazy RMT TX channel for 38 kHz NEC IR TX
    return s_has_interact.load() ? 2 : 0;
}

std::map<std::string, std::string> MosaicoModules::node_labels() const {
    std::map<std::string, std::string> labels;
    if (has_camera()) {
        labels["hardware.kube32.io/camera"] = "true";
    }
    if (has_interact()) {
        labels["hardware.kube32.io/interact"] = "true";
    }
    std::lock_guard<std::mutex> lock(s_state_mutex);
    if (s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].present) {
        labels["mosaico.kube32.io/slot-left"] = s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].descriptor_valid
            ? slot_label_type(s_slots[MOSAICO_MODULE_MGR_SLOT_LEFT].board_type)
            : "unidentified";
    }
    if (s_slots[MOSAICO_MODULE_MGR_SLOT_RIGHT].present) {
        labels["mosaico.kube32.io/slot-right"] = s_slots[MOSAICO_MODULE_MGR_SLOT_RIGHT].descriptor_valid
            ? slot_label_type(s_slots[MOSAICO_MODULE_MGR_SLOT_RIGHT].board_type)
            : "unidentified";
    }
    return labels;
}

int32_t MosaicoModules::led_set(uint32_t index, uint32_t rgb) {
    if (!has_interact()) return HAL_ERR_NODEV;
    std::lock_guard<std::mutex> lock(s_interact_mutex);
    if (!s_interact) return HAL_ERR_NODEV;
    const mosaico_interact_rgb_t c = {
        .r = static_cast<uint8_t>((rgb >> 16) & 0xFFu),
        .g = static_cast<uint8_t>((rgb >> 8) & 0xFFu),
        .b = static_cast<uint8_t>(rgb & 0xFFu),
    };
    esp_err_t ret = ESP_OK;
    if (index == kMosaicoLedAll) {
        ret = mosaico_interact_led_fill(s_interact, c);
    } else if (index < s_interact_info.led_count) {
        ret = mosaico_interact_led_set(s_interact, static_cast<uint8_t>(index), c);
    } else {
        return HAL_ERR_INVAL;
    }
    return ret == ESP_OK ? HAL_OK : HAL_ERR_IO;
}

int32_t MosaicoModules::led_clear() {
    if (!has_interact()) return HAL_ERR_NODEV;
    std::lock_guard<std::mutex> lock(s_interact_mutex);
    if (!s_interact) return HAL_ERR_NODEV;
    return mosaico_interact_led_clear(s_interact) == ESP_OK ? HAL_OK : HAL_ERR_IO;
}

int32_t MosaicoModules::interact_read(MosaicoInteractInputs& out) {
    if (!has_interact()) return HAL_ERR_NODEV;
    std::lock_guard<std::mutex> lock(s_interact_mutex);
    if (!s_interact) return HAL_ERR_NODEV;
    mosaico_interact_inputs_t raw{};
    esp_err_t ret = mosaico_interact_read_inputs(s_interact, &raw);
    if (ret != ESP_OK) return HAL_ERR_IO;
    out.left_pressed = raw.left_pressed;
    out.right_pressed = raw.right_pressed;
    out.motion_detected = raw.motion_detected;
    out.light_raw = raw.light_raw;
    out.light_level = raw.light_level;
    out.led_count = s_interact_info.led_count;
    out.button_mode = static_cast<uint8_t>(s_interact_info.button_mode);
    out.slot = static_cast<uint8_t>(s_interact_info.slot);
    return HAL_OK;
}

namespace {
struct IrSendPinnedCtx {
    mosaico_interact_handle_t handle;
    uint8_t address;
    uint8_t command;
    esp_err_t ret;
    SemaphoreHandle_t done;
};

void ir_send_pinned_task(void* arg) {
    auto* ctx = static_cast<IrSendPinnedCtx*>(arg);
    ctx->ret = mosaico_interact_ir_send_nec(ctx->handle, ctx->address, ctx->command);
    xSemaphoreGive(ctx->done);
    vTaskDelete(nullptr);
}
}  // namespace

int32_t MosaicoModules::interact_ir_send_nec(uint8_t address, uint8_t command) {
    if (!has_interact()) return HAL_ERR_NODEV;
    std::lock_guard<std::mutex> lock(s_interact_mutex);
    if (!s_interact) return HAL_ERR_NODEV;
    esp_err_t ret = ESP_OK;
    if (xPortGetCoreID() == s_interact_core) {
        ret = mosaico_interact_ir_send_nec(s_interact, address, command);
    } else {
        // RMT group 0's interrupt is pinned to the core where mosaico_interact_open()
        // created the WS2812 channel; run lazy IR TX channel creation + send on that
        // same core so rmt_new_tx_channel() never fails with ESP_ERR_INVALID_STATE.
        IrSendPinnedCtx ctx{s_interact, address, command, ESP_FAIL, xSemaphoreCreateBinary()};
        if (!ctx.done) return HAL_ERR_NOMEM;
        if (xTaskCreatePinnedToCore(ir_send_pinned_task, "k32_ir_tx", 4096, &ctx, 5, nullptr, s_interact_core) != pdPASS) {
            vSemaphoreDelete(ctx.done);
            return HAL_ERR_NOMEM;
        }
        xSemaphoreTake(ctx.done, portMAX_DELAY);
        vSemaphoreDelete(ctx.done);
        ret = ctx.ret;
    }
    if (ret == ESP_ERR_NO_MEM || ret == ESP_ERR_NOT_FOUND) return HAL_ERR_BUSY;
    return ret == ESP_OK ? HAL_OK : HAL_ERR_IO;
}

int32_t MosaicoModules::camera_info(MosaicoCameraInfo& out) {
    if (!has_camera()) return HAL_ERR_NODEV;
    std::lock_guard<std::mutex> lock(s_state_mutex);
    out = s_cam_info;
    return HAL_OK;
}

int32_t MosaicoModules::camera_capture_jpeg(std::vector<uint8_t>& out_jpeg, MosaicoCameraInfo* out_info, uint8_t quality) {
    out_jpeg.clear();
    if (!has_camera()) return HAL_ERR_NODEV;
    if (quality < 10) quality = 10;
    if (quality > 95) quality = 95;

    std::lock_guard<std::mutex> lock(s_camera_mutex);
    if (!s_camera || s_shutting_down.load()) return HAL_ERR_NODEV;

    esp_err_t ret = mosaico_camera_open(s_camera);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "camera open failed: %s", esp_err_to_name(ret));
        return HAL_ERR_IO;
    }

    mosaico_camera_info_t cinfo{};
    (void)mosaico_camera_get_info(s_camera, &cinfo);
    update_cached_camera_info(cinfo);

    ret = mosaico_camera_start_stream(s_camera);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "camera start_stream failed: %s", esp_err_to_name(ret));
        (void)mosaico_camera_close(s_camera);
        return HAL_ERR_IO;
    }

    mosaico_camera_frame_t frame{};
    ret = grab_valid_frame(s_camera, frame);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "camera frame capture failed: %s", esp_err_to_name(ret));
        (void)mosaico_camera_stop_stream(s_camera);
        (void)mosaico_camera_close(s_camera);
        return ret == ESP_ERR_TIMEOUT ? HAL_ERR_TIMEDOUT : HAL_ERR_IO;
    }

    int32_t rc = HAL_ERR_IO;
    if (frame.pixel_format == V4L2_PIX_FMT_JPEG) {
        out_jpeg.assign(static_cast<const uint8_t*>(frame.data),
                        static_cast<const uint8_t*>(frame.data) + frame.size);
        rc = HAL_OK;
    } else if (frame.pixel_format == V4L2_PIX_FMT_UYVY) {
        // Hardware JPEG encode into a temporary PSRAM output buffer (capped to
        // keep peak PSRAM usage modest alongside Wi-Fi + Wasm pods).
        const size_t wanted = std::min<size_t>(std::max<size_t>(frame.size / 3U, 128U * 1024U), 512U * 1024U);
        const jpeg_encode_memory_alloc_cfg_t mem_cfg = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        size_t jpeg_cap = 0;
        uint8_t* jpeg_buf = static_cast<uint8_t*>(jpeg_alloc_encoder_mem(wanted, &mem_cfg, &jpeg_cap));
        jpeg_encoder_handle_t encoder = nullptr;
        if (!jpeg_buf) {
            ESP_LOGE(TAG, "JPEG encoder buffer alloc (%u B) failed", (unsigned)wanted);
            rc = HAL_ERR_NOMEM;
        } else {
            const jpeg_encode_engine_cfg_t eng_cfg = {
                .timeout_ms = kJpegTimeoutMs,
            };
            esp_err_t jret = jpeg_new_encoder_engine(&eng_cfg, &encoder);
            if (jret == ESP_OK && encoder != nullptr) {
                const jpeg_encode_cfg_t jcfg = {
                    .height = frame.height,
                    .width = frame.width,
                    .src_type = JPEG_ENCODE_IN_FORMAT_YUV422,
                    .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
                    .image_quality = quality,
                };
                uint32_t encoded_size = 0;
                jret = jpeg_encoder_process(encoder, &jcfg, static_cast<const uint8_t*>(frame.data),
                                            static_cast<uint32_t>(frame.size), jpeg_buf,
                                            static_cast<uint32_t>(jpeg_cap), &encoded_size);
                if (jret == ESP_OK && encoded_size > 0) {
                    out_jpeg.assign(jpeg_buf, jpeg_buf + encoded_size);
                    rc = HAL_OK;
                } else {
                    ESP_LOGW(TAG, "hardware JPEG encode failed: %s (size=%u)",
                             esp_err_to_name(jret), (unsigned)encoded_size);
                    rc = HAL_ERR_IO;
                }
                (void)jpeg_del_encoder_engine(encoder);
            } else {
                ESP_LOGW(TAG, "create JPEG encoder engine failed: %s", esp_err_to_name(jret));
                rc = HAL_ERR_IO;
            }
            free(jpeg_buf);
        }
    } else {
        ESP_LOGW(TAG, "unsupported camera pixel format 0x%08x", (unsigned)frame.pixel_format);
        rc = HAL_ERR_INVAL;
    }

    // Always return the borrowed frame BEFORE stopping/closing the stream!
    (void)mosaico_camera_return_frame(s_camera, &frame);
    (void)mosaico_camera_stop_stream(s_camera);
    (void)mosaico_camera_close(s_camera);

    if (out_info) {
        std::lock_guard<std::mutex> slock(s_state_mutex);
        *out_info = s_cam_info;
    }
    if (rc == HAL_OK) {
        ESP_LOGI(TAG, "Captured JPEG snapshot: %ux%u -> %u bytes (Q=%u)",
                 (unsigned)cinfo.width, (unsigned)cinfo.height, (unsigned)out_jpeg.size(), (unsigned)quality);
    }
    return rc;
}

int32_t MosaicoModules::camera_preview_to_display(Board& board) {
    if (!has_camera()) return HAL_ERR_NODEV;
    const DisplayInfo dinfo = board.display_info();
    if (dinfo.width <= 0 || dinfo.height <= 0) return HAL_ERR_NODEV;

    std::lock_guard<std::mutex> lock(s_camera_mutex);
    if (!s_camera || s_shutting_down.load()) return HAL_ERR_NODEV;

    esp_err_t ret = mosaico_camera_open(s_camera);
    if (ret != ESP_OK) return HAL_ERR_IO;

    mosaico_camera_info_t cinfo{};
    (void)mosaico_camera_get_info(s_camera, &cinfo);
    update_cached_camera_info(cinfo);

    ret = mosaico_camera_start_stream(s_camera);
    if (ret != ESP_OK) {
        (void)mosaico_camera_close(s_camera);
        return HAL_ERR_IO;
    }

    mosaico_camera_frame_t frame{};
    ret = grab_valid_frame(s_camera, frame);
    if (ret != ESP_OK) {
        (void)mosaico_camera_stop_stream(s_camera);
        (void)mosaico_camera_close(s_camera);
        return ret == ESP_ERR_TIMEOUT ? HAL_ERR_TIMEDOUT : HAL_ERR_IO;
    }

    const uint32_t out_w = static_cast<uint32_t>(dinfo.width);
    const uint32_t out_h = static_cast<uint32_t>(dinfo.height);
    const size_t rgb565_bytes = align_up(static_cast<size_t>(out_w) * out_h * sizeof(uint16_t), kPreviewAlign);
    uint16_t* rgb565 = static_cast<uint16_t*>(
        heap_caps_aligned_calloc(kPreviewAlign, 1, rgb565_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));

    int32_t rc = HAL_ERR_IO;
    if (!rgb565) {
        rc = HAL_ERR_NOMEM;
    } else if (frame.pixel_format == V4L2_PIX_FMT_UYVY) {
        const uint32_t crop = align_down_even(std::min<uint32_t>(kPreviewCropMax, std::min(frame.width, frame.height)));
        const uint32_t bpl = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2U;

        ppa_client_handle_t ppa = nullptr;
        const ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (crop >= 64 && (bpl % 2U) == 0 && ppa_register_client(&ppa_cfg, &ppa) == ESP_OK && ppa != nullptr) {
            (void)esp_cache_msync(const_cast<void*>(frame.data), frame.size,
                                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            (void)esp_cache_msync(rgb565, rgb565_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

            const ppa_srm_oper_config_t op = {
                .in = {
                    .buffer = frame.data,
                    .pic_w = bpl / 2U,
                    .pic_h = frame.height,
                    .block_w = crop,
                    .block_h = crop,
                    .block_offset_x = align_down_even((frame.width - crop) / 2U),
                    .block_offset_y = align_down_even((frame.height - crop) / 2U),
                    .srm_cm = PPA_SRM_COLOR_MODE_YUV422_UYVY,
                    .yuv_range = PPA_COLOR_RANGE_LIMIT,
                    .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
                },
                .out = {
                    .buffer = rgb565,
                    .buffer_size = rgb565_bytes,
                    .pic_w = out_w,
                    .pic_h = out_h,
                    .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
                },
                .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
                .scale_x = static_cast<float>(out_w) / static_cast<float>(crop),
                .scale_y = static_cast<float>(out_h) / static_cast<float>(crop),
                .mirror_y = true,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };
            esp_err_t pret = ppa_do_scale_rotate_mirror(ppa, &op);
            if (pret == ESP_OK) {
                (void)esp_cache_msync(rgb565, rgb565_bytes,
                                      ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
                rc = HAL_OK;
            } else {
                ESP_LOGW(TAG, "PPA UYVY->RGB565 failed: %s", esp_err_to_name(pret));
            }
            (void)ppa_unregister_client(ppa);
        }
    } else {
        rc = HAL_ERR_INVAL;
    }

    // Release DVP DMA before blitting so camera and LCD DMA never contend.
    (void)mosaico_camera_return_frame(s_camera, &frame);
    (void)mosaico_camera_stop_stream(s_camera);
    (void)mosaico_camera_close(s_camera);

    if (rc == HAL_OK && rgb565) {
        rc = board.display_blit(0, 0, static_cast<int32_t>(out_w), static_cast<int32_t>(out_h), rgb565);
    }
    if (rgb565) {
        heap_caps_free(rgb565);
    }
    return rc;
}

void MosaicoModules::describe(BoardProfile& profile) const {
    if (!s_initialized.load()) return;
    profile.reserved_gpio_mask |= reserved_gpio_mask();
    profile.rmt_tx_board_channels = static_cast<uint8_t>(profile.rmt_tx_board_channels + rmt_tx_board_channels());
    // Subboard EEPROMs (0x50 left, 0x51 right) and camera SCCB sensors (OV3640 0x3C, SC101IOT 0x68)
    const uint8_t sub_bus = (profile.i2c_bus_mask & 0x2) ? 1 : 0;
    profile.i2c_reserved[sub_bus].set(0x50);
    profile.i2c_reserved[sub_bus].set(0x51);
    if (has_camera()) {
        profile.onboard_devices |= KUBE32_CAP_CAMERA;
        profile.i2c_reserved[sub_bus].set(0x30);  // SC101IOT SCCB (7-bit 0x30/0x68)
        profile.i2c_reserved[sub_bus].set(0x3C);  // OV3640 SCCB
        profile.i2c_reserved[sub_bus].set(0x68);  // SC101IOT alt SCCB
    }
    if (has_interact()) {
        profile.onboard_devices |= KUBE32_CAP_LED | KUBE32_CAP_INTERACT;
    }
}

void MosaicoModules::print_status() const {
    const MosaicoSlotStatus left = slot_status(0);
    const MosaicoSlotStatus right = slot_status(1);
    printf("K32_MODULES left=%s(0x%02x,present=%d,valid=%d,name=%s) right=%s(0x%02x,present=%d,valid=%d,name=%s) camera=%d interact=%d\n",
           left.type_name, left.eeprom_addr, left.present ? 1 : 0, left.descriptor_valid ? 1 : 0,
           left.board_name[0] ? left.board_name : "-",
           right.type_name, right.eeprom_addr, right.present ? 1 : 0, right.descriptor_valid ? 1 : 0,
           right.board_name[0] ? right.board_name : "-",
           has_camera() ? 1 : 0, has_interact() ? 1 : 0);
}

int32_t MosaicoModules::self_test_interact() {
    if (!has_interact()) {
        printf("K32_INTERACT err=nodev\n");
        return HAL_ERR_NODEV;
    }
    MosaicoInteractInputs in{};
    int32_t rc = interact_read(in);
    if (rc != HAL_OK) {
        printf("K32_INTERACT read_err=%d\n", (int)rc);
        return rc;
    }
    // Brief RGB chase across the 6 LEDs (safe brightness cap enforced), then off
    const uint32_t colors[6] = {0xFF0000, 0xFF8000, 0xFFFF00, 0x00FF00, 0x0080FF, 0x8000FF};
    for (uint32_t i = 0; i < 6; ++i) (void)led_set(i, colors[i]);
    vTaskDelay(pdMS_TO_TICKS(150));
    (void)led_clear();
    const int32_t ir_rc = interact_ir_send_nec(0x00, 0x10);
    printf("K32_INTERACT ok left_btn=%d right_btn=%d pir=%d light_pct=%u light_raw=%d leds=%u mode=%s ir_rc=%d\n",
           in.left_pressed ? 1 : 0, in.right_pressed ? 1 : 0, in.motion_detected ? 1 : 0,
           (unsigned)in.light_level, (int)in.light_raw, (unsigned)in.led_count,
           in.button_mode == 2 ? "touch" : "gpio", (int)ir_rc);
    return ir_rc == HAL_OK ? HAL_OK : ir_rc;
}

int32_t MosaicoModules::self_test_camera(Board* board) {
    if (!has_camera()) {
        printf("K32_CAMERA err=nodev\n");
        return HAL_ERR_NODEV;
    }
    std::vector<uint8_t> jpeg;
    MosaicoCameraInfo info{};
    const int32_t rc = camera_capture_jpeg(jpeg, &info, 75);
    if (rc != HAL_OK) {
        printf("K32_CAMERA capture_err=%d\n", (int)rc);
        return rc;
    }
    int32_t prev_rc = HAL_OK;
    if (board) {
        prev_rc = camera_preview_to_display(*board);
    }
    printf("K32_CAMERA ok sensor=%s res=%ux%u jpeg_bytes=%u soi=%02x%02x preview_rc=%d\n",
           info.sensor, (unsigned)info.width, (unsigned)info.height, (unsigned)jpeg.size(),
           jpeg.size() >= 2 ? jpeg[0] : 0, jpeg.size() >= 2 ? jpeg[1] : 0, (int)prev_rc);
    return prev_rc == HAL_OK ? HAL_OK : prev_rc;
}

bool mosaico_modules_init() { return MosaicoModules::instance().init(); }
void mosaico_modules_shutdown() { MosaicoModules::instance().shutdown(); }
void mosaico_modules_describe(BoardProfile& profile) { MosaicoModules::instance().describe(profile); }
int32_t mosaico_modules_led_set(uint32_t index, uint32_t rgb) { return MosaicoModules::instance().led_set(index, rgb); }
void mosaico_modules_labels(std::map<std::string, std::string>& labels) {
    for (const auto& kv : MosaicoModules::instance().node_labels()) {
        labels[kv.first] = kv.second;
    }
}

}  // namespace kube32::hal

#else  // Non-ESP32-S31 stub (host simulator, ESP32-S3, etc.)

namespace kube32::hal {

MosaicoModules& MosaicoModules::instance() {
    static MosaicoModules inst;
    return inst;
}

bool MosaicoModules::init() { return false; }
void MosaicoModules::shutdown() {}
bool MosaicoModules::initialized() const { return false; }
bool MosaicoModules::has_camera() const { return false; }
bool MosaicoModules::has_interact() const { return false; }
MosaicoSlotStatus MosaicoModules::slot_status(uint32_t) const { return {}; }
uint64_t MosaicoModules::reserved_gpio_mask() const { return 0; }
uint8_t MosaicoModules::rmt_tx_board_channels() const { return 0; }
std::map<std::string, std::string> MosaicoModules::node_labels() const { return {}; }
int32_t MosaicoModules::led_set(uint32_t, uint32_t) { return HAL_ERR_NODEV; }
int32_t MosaicoModules::led_clear() { return HAL_ERR_NODEV; }
int32_t MosaicoModules::interact_read(MosaicoInteractInputs&) { return HAL_ERR_NODEV; }
int32_t MosaicoModules::interact_ir_send_nec(uint8_t, uint8_t) { return HAL_ERR_NODEV; }
int32_t MosaicoModules::camera_info(MosaicoCameraInfo&) { return HAL_ERR_NODEV; }
int32_t MosaicoModules::camera_capture_jpeg(std::vector<uint8_t>&, MosaicoCameraInfo*, uint8_t) { return HAL_ERR_NODEV; }
int32_t MosaicoModules::camera_preview_to_display(Board&) { return HAL_ERR_NODEV; }
void MosaicoModules::describe(BoardProfile&) const {}
void MosaicoModules::print_status() const { printf("K32_MODULES unsupported\n"); }
int32_t MosaicoModules::self_test_interact() { return HAL_ERR_NODEV; }
int32_t MosaicoModules::self_test_camera(Board*) { return HAL_ERR_NODEV; }

bool mosaico_modules_init() { return false; }
void mosaico_modules_shutdown() {}
void mosaico_modules_describe(BoardProfile&) {}
int32_t mosaico_modules_led_set(uint32_t, uint32_t) { return HAL_ERR_NODEV; }
void mosaico_modules_labels(std::map<std::string, std::string>&) {}

}  // namespace kube32::hal

#endif
