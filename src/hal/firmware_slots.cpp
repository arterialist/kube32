#include "hal/firmware_slots.hpp"
#include "hal/node_storage.hpp"
#include "osal/sha256.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#if defined(KUBE32_TARGET_ESP32)
#include "api/version_info.hpp"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_image_format.h"
#include "esp_system.h"
#include "nvs.h"
#endif

namespace kube32::hal {

namespace {

constexpr char kVersionTag[] = "kube32-version";   // followed by ':' in the image
constexpr size_t kTagLen = sizeof(kVersionTag) - 1;

bool version_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-';
}

} // namespace

std::string find_image_version(size_t size, mem::FunctionRef<bool(size_t, void*, size_t)> read) {
    // Reads in windows that overlap by the tag and the longest version, so a
    // match across a window boundary is seen whole in the next window
    constexpr size_t kWindow = 1024;
    constexpr size_t kOverlap = kTagLen + 1 + 32;
    char buf[kWindow];
    size_t offset = 0;
    while (offset < size) {
        const size_t len = std::min(kWindow, size - offset);
        if (!read(offset, buf, len)) return std::string();
        for (size_t i = 0; i + kTagLen < len; ++i) {
            if (buf[i] != 'k' || std::memcmp(buf + i, kVersionTag, kTagLen) != 0 || buf[i + kTagLen] != ':') continue;
            size_t j = i + kTagLen + 1;
            std::string version;
            while (j < len && version_char(buf[j]) && version.size() < 31) version.push_back(buf[j++]);
            if (j < len && buf[j] == '\0' && !version.empty()) return version;
            if (j == len && offset + len < size) break;   // cut by the window: read again from here
        }
        if (offset + len >= size) break;
        offset += len > kOverlap ? len - kOverlap : len;
    }
    return std::string();
}

bool FirmwareSlots::passive_sha256(size_t size, std::string& out_hex) {
    if (size == 0 || size > slot_size()) return false;
    osal::Sha256 sha;
    char buf[1024];
    for (size_t off = 0; off < size; off += sizeof(buf)) {
        const size_t len = std::min(sizeof(buf), size - off);
        if (!read(off, buf, len)) return false;
        sha.update(buf, len);
    }
    out_hex = osal::Sha256::hex(sha.finish());
    return true;
}

#if defined(KUBE32_TARGET_ESP32)

namespace {

constexpr const char* kNvsNamespace = "k32fw";

class EspFirmwareSlots : public FirmwareSlots {
public:
    EspFirmwareSlots() {
        node_storage_call([&] {
            running_ = esp_ota_get_running_partition();
            passive_ = esp_ota_get_next_update_partition(nullptr);
            esp_ota_img_states_t state;
            pending_ = running_ && esp_ota_get_state_partition(running_, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
        });
    }

    FirmwareInfo running() override {
        if (!info_ready_ && running_ && node_storage_call_available()) {
            info_.version = api::firmware_version();
            info_.slot = running_->label;
            node_storage_call([&] {
                // The image file: segments, checksum and the SHA-256 the build appended
                esp_image_metadata_t meta{};
                const esp_partition_pos_t pos = {running_->address, running_->size};
                if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len == 0) return;
                osal::Sha256 sha;
                char buf[1024];
                for (size_t off = 0; off < meta.image_len; off += sizeof(buf)) {
                    const size_t len = std::min<size_t>(sizeof(buf), meta.image_len - off);
                    if (esp_partition_read(running_, off, buf, len) != ESP_OK) return;
                    sha.update(buf, len);
                }
                info_.sha256 = osal::Sha256::hex(sha.finish());
            });
            info_ready_ = true;
        }
        info_.pending_verify = pending_;
        return info_;
    }

    // One storage call for the whole slot: the reads and the hash run where
    // flash may be read, without a switch per block
    bool passive_sha256(size_t size, std::string& out_hex) override {
        if (!passive_ || size == 0 || size > passive_->size || !node_storage_call_available()) return false;
        bool ok = true;
        node_storage_call([&] {
            osal::Sha256 sha;
            char buf[1024];
            for (size_t off = 0; off < size && ok; off += sizeof(buf)) {
                const size_t len = std::min(sizeof(buf), size - off);
                ok = esp_partition_read(passive_, off, buf, len) == ESP_OK;
                sha.update(buf, len);
            }
            if (ok) out_hex = osal::Sha256::hex(sha.finish());
        });
        return ok;
    }

    std::string passive_slot() const override { return passive_ ? passive_->label : std::string(); }
    size_t slot_size() const override { return passive_ ? passive_->size : 0; }

    bool begin(std::string& why) override {
        if (!ready(why)) return false;
        abort();
        esp_err_t err = ESP_OK;
        node_storage_call([&] { err = esp_ota_begin(passive_, OTA_WITH_SEQUENTIAL_WRITES, &handle_); });
        if (err != ESP_OK) {
            handle_ = 0;
            why = err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ? "the running firmware has not confirmed itself yet"
                                                            : std::string("esp_ota_begin: ") + esp_err_to_name(err);
            return false;
        }
        wrote_ = 0;
        return true;
    }

    bool write(const void* data, size_t len, std::string& why) override {
        if (!handle_) { why = "no image is being written"; return false; }
        if (wrote_ + len > passive_->size) { why = "the image is larger than the slot"; return false; }
        esp_err_t err = ESP_OK;
        node_storage_call([&] { err = esp_ota_write(handle_, data, len); });
        if (err != ESP_OK) {
            why = err == ESP_ERR_OTA_VALIDATE_FAILED ? "not an ESP32 firmware image" : std::string("esp_ota_write: ") + esp_err_to_name(err);
            abort();
            return false;
        }
        wrote_ += len;
        return true;
    }

    bool finish(std::string& why) override {
        if (!handle_) { why = "no image is being written"; return false; }
        esp_err_t err = ESP_OK;
        node_storage_call([&] { err = esp_ota_end(handle_); });
        handle_ = 0;
        if (err != ESP_OK) {
            why = err == ESP_ERR_OTA_VALIDATE_FAILED ? "the image does not verify (segments or appended SHA-256)"
                                                     : std::string("esp_ota_end: ") + esp_err_to_name(err);
            return false;
        }
        return true;
    }

    void abort() override {
        if (!handle_) return;
        node_storage_call([&] { esp_ota_abort(handle_); });
        handle_ = 0;
    }

    bool read(size_t offset, void* dst, size_t len) override {
        if (!passive_ || offset > passive_->size || len > passive_->size - offset || !node_storage_call_available()) return false;
        // Through a buffer on the internal stack: the flash driver would take
        // a bounce buffer from the heap for a destination in PSRAM
        esp_err_t err = ESP_OK;
        node_storage_call([&] {
            char buf[1024];
            for (size_t done = 0; done < len && err == ESP_OK; done += sizeof(buf)) {
                const size_t n = std::min(sizeof(buf), len - done);
                err = esp_partition_read(passive_, offset + done, buf, n);
                std::memcpy(static_cast<char*>(dst) + done, buf, n);
            }
        });
        return err == ESP_OK;
    }

    bool activate(std::string& why) override {
        if (!ready(why)) return false;
        esp_err_t err = ESP_OK;
        node_storage_call([&] { err = esp_ota_set_boot_partition(passive_); });
        if (err != ESP_OK) {
            why = std::string("esp_ota_set_boot_partition: ") + esp_err_to_name(err);
            return false;
        }
        return true;
    }

    bool confirm() override {
        if (!pending_) return true;
        esp_err_t err = ESP_OK;
        node_storage_call([&] { err = esp_ota_mark_app_valid_cancel_rollback(); });
        if (err == ESP_OK) pending_ = false;
        return err == ESP_OK;
    }

    bool reject() override {
        if (!pending_) return false;
        esp_err_t err = ESP_OK;
        // Marks this image invalid and reboots into the previous slot
        node_storage_call([&] { err = esp_ota_mark_app_invalid_rollback_and_reboot(); });
        return err == ESP_OK;
    }

    void reboot() override { esp_restart(); }

    std::string note(const char* key) override {
        nvs_handle_t h;
        if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return std::string();
        char v[96] = {0};
        size_t vl = sizeof(v);
        const bool ok = nvs_get_str(h, key, v, &vl) == ESP_OK;
        nvs_close(h);
        return ok ? std::string(v) : std::string();
    }

    void set_note(const char* key, const std::string& value) override {
        nvs_handle_t h;
        if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
        if (value.empty()) nvs_erase_key(h, key); else nvs_set_str(h, key, value.c_str());
        nvs_commit(h);
        nvs_close(h);
    }

private:
    bool ready(std::string& why) const {
        if (!running_ || !passive_) { why = "the partition table has no second app slot"; return false; }
        if (!node_storage_call_available()) { why = "flash cannot be written from this thread"; return false; }
        return true;
    }

    const esp_partition_t* running_{nullptr};
    const esp_partition_t* passive_{nullptr};
    bool pending_{false};
    esp_ota_handle_t handle_{0};
    size_t wrote_{0};
    FirmwareInfo info_;
    bool info_ready_{false};
};

} // namespace

std::unique_ptr<FirmwareSlots> create_esp_firmware_slots() {
    return std::make_unique<EspFirmwareSlots>();
}

#else

std::unique_ptr<FirmwareSlots> create_esp_firmware_slots() { return nullptr; }

#endif

// -----------------------------------------------------------------------------
// FileFirmwareSlots (simulator and tests)
// -----------------------------------------------------------------------------
#if !defined(KUBE32_TARGET_ESP32)

namespace {

bool file_exists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

size_t file_size(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0 ? static_cast<size_t>(st.st_size) : 0;
}

bool read_file_range(const std::string& p, size_t offset, void* dst, size_t len) {
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fseek(f, static_cast<long>(offset), SEEK_SET) == 0 && std::fread(dst, 1, len, f) == len;
    std::fclose(f);
    return ok;
}

// An image as esptool writes it: the magic byte first and the SHA-256 of
// everything before them in the last 32 bytes
bool image_verifies(const std::string& p) {
    const size_t size = file_size(p);
    if (size < 64) return false;
    uint8_t first = 0;
    if (!read_file_range(p, 0, &first, 1) || first != FileFirmwareSlots::kImageMagic) return false;
    osal::Sha256 sha;
    char buf[4096];
    for (size_t off = 0; off < size - 32; off += sizeof(buf)) {
        const size_t len = std::min(sizeof(buf), size - 32 - off);
        if (!read_file_range(p, off, buf, len)) return false;
        sha.update(buf, len);
    }
    uint8_t tail[32];
    if (!read_file_range(p, size - 32, tail, sizeof(tail))) return false;
    const osal::Sha256::Digest d = sha.finish();
    return std::memcmp(d.data(), tail, sizeof(tail)) == 0;
}

} // namespace

FileFirmwareSlots::FileFirmwareSlots(std::string directory, FirmwareInfo builtin, size_t slot_size)
    : dir_(std::move(directory)), builtin_(std::move(builtin)), slot_size_(slot_size) {
    ::mkdir(dir_.c_str(), 0777);
    load_otadata();
    // The bootloader: a NEW image boots once as pending; an image still
    // pending at the next boot never confirmed itself and is abandoned
    if (ota_.state == "new") {
        ota_.state = "pending";
        store_otadata();
    } else if (ota_.state == "pending") {
        ota_.state = "aborted";
        ota_.boot = ota_.previous;
        store_otadata();
    }
    running_slot_ = ota_.boot;
    pending_ = ota_.state == "pending";
    written_ = file_exists(slot_path(running_slot_));
}

std::string FileFirmwareSlots::path(const std::string& name) const { return dir_ + "/" + name; }

std::string FileFirmwareSlots::slot_path(int slot) const { return path(slot ? "ota_1.bin" : "ota_0.bin"); }

void FileFirmwareSlots::load_otadata() {
    // "boot=<0|1> previous=<0|1> state=<new|pending|valid|aborted|invalid>"
    std::FILE* f = std::fopen(path("otadata").c_str(), "r");
    if (!f) return;
    int boot = 0, previous = 0;
    char state[16] = {0};
    if (std::fscanf(f, "boot=%d previous=%d state=%15s", &boot, &previous, state) == 3) {
        ota_.boot = boot ? 1 : 0;
        ota_.previous = previous ? 1 : 0;
        ota_.state = state;
    }
    std::fclose(f);
}

void FileFirmwareSlots::store_otadata() const {
    const std::string tmp = path("otadata.tmp");
    std::FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "boot=%d previous=%d state=%s\n", ota_.boot, ota_.previous, ota_.state.c_str());
    std::fclose(f);
    std::rename(tmp.c_str(), path("otadata").c_str());
}

FirmwareInfo FileFirmwareSlots::running() {
    if (!info_ready_) {
        if (written_) {
            const std::string p = slot_path(running_slot_);
            const size_t size = file_size(p);
            std::string hex;
            osal::Sha256 sha;
            char buf[4096];
            for (size_t off = 0; off < size; off += sizeof(buf)) {
                const size_t len = std::min(sizeof(buf), size - off);
                if (!read_file_range(p, off, buf, len)) break;
                sha.update(buf, len);
            }
            info_.sha256 = osal::Sha256::hex(sha.finish());
            info_.version = find_image_version(size, [&](size_t off, void* dst, size_t len) { return read_file_range(p, off, dst, len); });
        } else {
            info_ = builtin_;
        }
        info_.slot = running_slot_ ? "ota_1" : "ota_0";
        info_ready_ = true;
    }
    info_.pending_verify = pending_;
    return info_;
}

std::string FileFirmwareSlots::passive_slot() const { return running_slot_ ? "ota_0" : "ota_1"; }

bool FileFirmwareSlots::begin(std::string& why) {
    if (pending_) {
        why = "the running firmware has not confirmed itself yet";
        return false;
    }
    std::FILE* f = std::fopen(slot_path(1 - running_slot_).c_str(), "wb");
    if (!f) {
        why = "cannot open " + slot_path(1 - running_slot_);
        return false;
    }
    std::fclose(f);
    writing_ = true;
    staged_ok_ = false;
    wrote_ = 0;
    return true;
}

bool FileFirmwareSlots::write(const void* data, size_t len, std::string& why) {
    if (!writing_) { why = "no image is being written"; return false; }
    if (wrote_ + len > slot_size_) { why = "the image is larger than the slot"; abort(); return false; }
    if (wrote_ == 0 && len > 0 && static_cast<const uint8_t*>(data)[0] != kImageMagic) {
        why = "not an ESP32 firmware image";
        abort();
        return false;
    }
    std::FILE* f = std::fopen(slot_path(1 - running_slot_).c_str(), "ab");
    const bool ok = f && std::fwrite(data, 1, len, f) == len;
    if (f) std::fclose(f);
    if (!ok) { why = "write failed"; abort(); return false; }
    wrote_ += len;
    return true;
}

bool FileFirmwareSlots::finish(std::string& why) {
    if (!writing_) { why = "no image is being written"; return false; }
    writing_ = false;
    if (!image_verifies(slot_path(1 - running_slot_))) {
        why = "the image does not verify (segments or appended SHA-256)";
        return false;
    }
    staged_ok_ = true;
    return true;
}

void FileFirmwareSlots::abort() { writing_ = false; }

bool FileFirmwareSlots::read(size_t offset, void* dst, size_t len) {
    const std::string p = slot_path(1 - running_slot_);
    if (offset + len > file_size(p)) return false;
    return read_file_range(p, offset, dst, len);
}

bool FileFirmwareSlots::activate(std::string& why) {
    if (!image_verifies(slot_path(1 - running_slot_))) {
        why = "the slot holds no valid image";
        return false;
    }
    ota_.previous = running_slot_;
    ota_.boot = 1 - running_slot_;
    ota_.state = "new";
    store_otadata();
    return true;
}

bool FileFirmwareSlots::confirm() {
    if (!pending_) return true;
    ota_.state = "valid";
    store_otadata();
    pending_ = false;
    return true;
}

bool FileFirmwareSlots::reject() {
    if (!pending_) return false;
    ota_.state = "invalid";
    ota_.boot = ota_.previous;
    store_otadata();
    reboot();
    return true;
}

void FileFirmwareSlots::reboot() {
    reboot_requested_ = true;
    if (on_reboot_) on_reboot_();
}

std::string FileFirmwareSlots::note(const char* key) {
    std::FILE* f = std::fopen(path(std::string("note-") + key).c_str(), "r");
    if (!f) return std::string();
    char v[256] = {0};
    const bool ok = std::fgets(v, sizeof(v), f) != nullptr;
    std::fclose(f);
    std::string s = ok ? v : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

void FileFirmwareSlots::set_note(const char* key, const std::string& value) {
    const std::string p = path(std::string("note-") + key);
    if (value.empty()) {
        ::unlink(p.c_str());
        return;
    }
    std::FILE* f = std::fopen(p.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "%s\n", value.c_str());
    std::fclose(f);
}

#endif

} // namespace kube32::hal
