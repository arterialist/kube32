#pragma once

// The node's two firmware slots and the bootloader's rollback state machine.
//
// The partition table has two app slots (ota_0, ota_1) and otadata, which
// names the slot that boots and the state of its image. An update writes
// the slot that is not running (the passive slot), checks the image and
// marks the slot to boot next as NEW. The bootloader boots a NEW image once
// as PENDING_VERIFY; the image confirms itself (VALID) when it works. Any
// reset before that makes the bootloader mark it ABORTED and boot the other
// slot again: a panic, a watchdog reset or an image that rejects itself.
//
// EspFirmwareSlots drives ESP-IDF's app_update component; its flash access
// runs through hal::node_storage_call, so callers may have their stack in
// PSRAM. FileFirmwareSlots keeps the slots and otadata as files in a
// directory, for the simulator and the tests: each object is one boot, and
// constructing it runs the bootloader's part of the state machine.
//
// A few short notes survive reboots with the slots (NVS on the board, files
// on the host): the firmware agent records there what it is updating to.

#include "mem/function_ref.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace kube32::hal {

struct FirmwareInfo {
    std::string version;          // the image's KUBE32_FIRMWARE_VERSION
    std::string sha256;           // hex SHA-256 of the image file
    std::string slot;             // "ota_0" or "ota_1"
    bool pending_verify{false};   // booted from an update and not confirmed yet
};

class FirmwareSlots {
public:
    virtual ~FirmwareSlots() = default;

    // The running image. The SHA-256 is computed once, at the first call.
    virtual FirmwareInfo running() = 0;
    // Label of the passive slot and the largest image it takes
    virtual std::string passive_slot() const = 0;
    virtual size_t slot_size() const = 0;

    // Writing the passive slot: begin, write the image in order, finish
    // (checks that it is a bootable image). abort() drops a write.
    virtual bool begin(std::string& why) = 0;
    virtual bool write(const void* data, size_t len, std::string& why) = 0;
    virtual bool finish(std::string& why) = 0;
    virtual void abort() = 0;
    // Bytes [offset, offset + len) of the passive slot
    virtual bool read(size_t offset, void* dst, size_t len) = 0;
    // SHA-256 of the first `size` bytes of the passive slot (hex)
    virtual bool passive_sha256(size_t size, std::string& out_hex);

    // Boot the passive slot next, unconfirmed
    virtual bool activate(std::string& why) = 0;
    // The running image works: it stays. False when otadata could not be
    // written (the image is still unconfirmed)
    virtual bool confirm() = 0;
    // The running image does not work: back to the previous slot. Reboots on
    // the board; false when there is nothing to go back to.
    virtual bool reject() = 0;
    virtual void reboot() = 0;

    virtual std::string note(const char* key) = 0;
    virtual void set_note(const char* key, const std::string& value) = 0;
};

// The board's slots (ESP-IDF app_update); null on the host
std::unique_ptr<FirmwareSlots> create_esp_firmware_slots();

// The version an image file carries ("kube32-version:<version>" in its
// constant data), empty when none is found. `read(offset, buf, len)` reads
// the image, which is `size` bytes long.
std::string find_image_version(size_t size, mem::FunctionRef<bool(size_t, void*, size_t)> read);

// Slots as files in `directory`: ota_0.bin, ota_1.bin and otadata. Before
// any update the node runs `builtin` from ota_0. `on_reboot` is called for
// reboot() and reject() (the simulator restarts the node's kubelet).
class FileFirmwareSlots : public FirmwareSlots {
public:
    FileFirmwareSlots(std::string directory, FirmwareInfo builtin, size_t slot_size = 0x340000);

    FirmwareInfo running() override;
    std::string passive_slot() const override;
    size_t slot_size() const override { return slot_size_; }
    bool begin(std::string& why) override;
    bool write(const void* data, size_t len, std::string& why) override;
    bool finish(std::string& why) override;
    void abort() override;
    bool read(size_t offset, void* dst, size_t len) override;
    bool activate(std::string& why) override;
    bool confirm() override;
    bool reject() override;
    void reboot() override;
    std::string note(const char* key) override;
    void set_note(const char* key, const std::string& value) override;

    void set_reboot_handler(std::function<void()> on_reboot) { on_reboot_ = std::move(on_reboot); }
    // reboot() or reject() was called on this boot
    bool reboot_requested() const { return reboot_requested_; }

    // Bytes a valid image starts with (the ESP image magic), checked by begin/write
    static constexpr uint8_t kImageMagic = 0xE9;

private:
    struct OtaData {
        int boot{0};                 // slot that boots
        int previous{0};             // slot to fall back to
        std::string state{"valid"};  // new, pending, valid, aborted
    };
    std::string path(const std::string& name) const;
    std::string slot_path(int slot) const;
    void load_otadata();
    void store_otadata() const;

    std::string dir_;
    FirmwareInfo builtin_;
    size_t slot_size_;
    OtaData ota_;
    int running_slot_{0};
    bool pending_{false};
    bool written_{false};    // a slot file exists for the running slot
    FirmwareInfo info_;
    bool info_ready_{false};
    // The write in progress
    bool writing_{false};
    size_t wrote_{0};
    bool staged_ok_{false};
    std::function<void()> on_reboot_;
    bool reboot_requested_{false};
};

} // namespace kube32::hal
