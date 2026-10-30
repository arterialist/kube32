#include "kubelet/firmware_agent.hpp"
#include "models/firmware.hpp"
#include "models/kube_models.hpp"
#include "osal/system.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kube32::kubelet {

namespace {

// Notes kept with the slots across reboots
constexpr const char* kNoteTry = "try";    // the image the node switched to, until it confirms itself
constexpr const char* kNoteFail = "fail";  // the image that last failed here
constexpr const char* kNoteWhy = "why";    // why the image rejected itself

std::string short_sha(const std::string& sha) { return sha.substr(0, 12); }

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string escape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out;
}

std::string kib(size_t bytes) { return std::to_string((bytes + 1023) / 1024) + " KiB"; }

} // namespace

FirmwareAgent::FirmwareAgent(hal::FirmwareSlots& slots, KubeApiClient& api, FirmwareAgentConfig config)
    : slots_(slots), api_(api), config_(std::move(config)) {
    running_ = slots_.running();
    boot_ms_ = osal::System::get_uptime_ms();
    failed_ = slots_.note(kNoteFail);
    const std::string tried = slots_.note(kNoteTry);
    bool rolled_back = false;
    if (!tried.empty() && tried != running_.sha256) {
        // The node switched to `tried` and came back without it: the image
        // reset before it confirmed itself, or rejected itself
        const std::string why = slots_.note(kNoteWhy);
        failed_ = tried;
        slots_.set_note(kNoteFail, tried);
        slots_.set_note(kNoteTry, std::string());
        slots_.set_note(kNoteWhy, std::string());
        const std::string msg = "image " + short_sha(tried) + (why.empty() ? std::string(" reset before it confirmed itself") : " " + why) +
                                "; the bootloader went back to " + running_.version + " on " + running_.slot;
        state_ = State::RolledBack;
        set_condition(false, "RolledBack", msg);
        event("FirmwareRolledBack", msg, true);
        printf("[FIRMWARE] %s\n", msg.c_str());
        rolled_back = true;
    } else if (!tried.empty() && !running_.pending_verify) {
        slots_.set_note(kNoteTry, std::string());   // confirmed on an earlier boot
    }
    if (running_.pending_verify) {
        state_ = State::Verifying;
        set_condition(true, "Verifying", running_.version + " boots unconfirmed from " + running_.slot + "; it confirms itself after " +
                                             std::to_string(config_.confirm_after_ms / 1000) + " s of Ready heartbeats");
        printf("[FIRMWARE] %s (%s) on %s is unconfirmed: confirming after %u s of Ready heartbeats, rejecting it after %u s\n",
               running_.version.c_str(), short_sha(running_.sha256).c_str(), running_.slot.c_str(),
               (unsigned)(config_.confirm_after_ms / 1000), (unsigned)(config_.confirm_timeout_ms / 1000));
#if defined(KUBE32_FIRMWARE_FAULT)
        fault_at_ms_ = boot_ms_ + 10000;
#endif
    } else if (!rolled_back) {
        if (!failed_.empty()) {
            state_ = State::Failed;
            set_condition(false, "UpdateFailed", "image " + short_sha(failed_) + " failed on this node");
        } else {
            report_idle();
        }
    }
}

std::map<std::string, std::string> FirmwareAgent::annotations() const {
    std::map<std::string, std::string> a;
    a[models::kAnnotationFirmwareVersion] = running_.version;
    a[models::kAnnotationFirmwareSha256] = running_.sha256;
    a[models::kAnnotationFirmwareSlot] = running_.slot;
    a[models::kAnnotationFirmwareState] = running_.pending_verify ? "Verifying" : "Confirmed";
    if (!failed_.empty()) a[models::kAnnotationFirmwareFailed] = failed_;
    return a;
}

FirmwareConditionView FirmwareAgent::condition() const {
    osal::LockGuard<osal::Mutex> lock(mu_);
    return condition_;
}

void FirmwareAgent::set_condition(bool updating, const char* reason, const std::string& message) {
    osal::LockGuard<osal::Mutex> lock(mu_);
    condition_.updating = updating;
    snprintf(condition_.reason, sizeof(condition_.reason), "%s", reason);
    snprintf(condition_.message, sizeof(condition_.message), "%s", message.c_str());
}

void FirmwareAgent::report_idle() {
    set_condition(false, "UpToDate", running_.version + " (" + short_sha(running_.sha256) + ") on " + running_.slot);
}

void FirmwareAgent::event(const char* reason, const std::string& message, bool warning) {
    if (pending_events_.size() >= 8) pending_events_.erase(pending_events_.begin());
    pending_events_.push_back({reason, {message, warning}});
}

void FirmwareAgent::on_heartbeat(bool ok, bool ready, uint64_t now_ms) {
    if (!ok || !ready) healthy_since_ms_.store(0);
    else if (healthy_since_ms_.load() == 0) healthy_since_ms_.store(now_ms ? now_ms : 1);
}

void FirmwareAgent::patch_annotations() {
    // Every key, the failed image as null when there is none: what an
    // earlier boot left on the Node is replaced
    const std::map<std::string, std::string> a = annotations();
    std::string patch = "{\"metadata\":{\"annotations\":{";
    for (const auto& kv : a) patch += "\"" + kv.first + "\":\"" + escape(kv.second) + "\",";
    patch += std::string("\"") + models::kAnnotationFirmwareFailed + "\":" + (failed_.empty() ? "null" : "\"" + failed_ + "\"") + "}}}";
    if (api_.patch_node_status(config_.node_name, patch)) annotations_dirty_ = false;
}

void FirmwareAgent::fail(const std::string& sha, const std::string& why, const char* reason) {
    failed_ = sha;
    slots_.set_note(kNoteFail, sha);
    annotations_dirty_ = true;
    target_.clear();
    state_ = State::Failed;
    const std::string msg = "image " + short_sha(sha) + ": " + why;
    set_condition(false, "UpdateFailed", msg);
    event(reason, msg, true);
    printf("[FIRMWARE] %s\n", msg.c_str());
}

void FirmwareAgent::tick(uint64_t now, bool registered) {
#if defined(KUBE32_FIRMWARE_FAULT)
    // Fault build (-DKUBE32_FIRMWARE_FAULT): an image that panics before it
    // confirms itself, to exercise the bootloader's rollback
    if (fault_at_ms_ && now >= fault_at_ms_ && state_ == State::Verifying) {
        printf("[FIRMWARE] fault build: panicking before the image confirms itself\n");
        fflush(stdout);
        abort();
    }
#endif
    if (registered) {
        while (!pending_events_.empty()) {
            const auto& e = pending_events_.front();
            const std::string json =
                "{\"kind\":\"Event\",\"apiVersion\":\"v1\",\"metadata\":{\"namespace\":\"default\"},"
                "\"involvedObject\":{\"kind\":\"Node\",\"name\":\"" + config_.node_name + "\",\"apiVersion\":\"v1\"},"
                "\"reason\":\"" + e.first + "\",\"message\":\"" + escape(e.second.first) + "\",\"type\":\"" +
                (e.second.second ? "Warning" : "Normal") + "\",\"reportingComponent\":\"kubelet\",\"reportingInstance\":\"" +
                config_.node_name + "\",\"source\":{\"component\":\"kubelet\",\"host\":\"" + config_.node_name + "\"}}";
            if (!api_.create_event("default", json)) break;
            pending_events_.erase(pending_events_.begin());
        }
        if (annotations_dirty_) patch_annotations();
    }

    switch (state_.load()) {
        case State::Verifying: {
            const uint64_t healthy = healthy_since_ms_.load();
            if (healthy && now >= healthy && now - healthy >= config_.confirm_after_ms) {
                if (!slots_.confirm()) return;   // tried again next pass
                slots_.set_note(kNoteTry, std::string());
                running_.pending_verify = false;
                annotations_dirty_ = true;
                state_ = State::Idle;
                report_idle();
                const std::string msg = running_.version + " (" + short_sha(running_.sha256) + ") confirmed itself on " + running_.slot;
                event("FirmwareUpdated", msg, false);
                printf("[FIRMWARE] %s\n", msg.c_str());
            } else if (now - boot_ms_ >= config_.confirm_timeout_ms) {
                const std::string why = "did not report Ready long enough within " + std::to_string(config_.confirm_timeout_ms / 1000) +
                                        " s of booting";
                printf("[FIRMWARE] %s %s: rejecting it\n", running_.version.c_str(), why.c_str());
                slots_.set_note(kNoteWhy, why);
                if (!slots_.reject()) {
                    // Nothing to go back to: keep it
                    slots_.set_note(kNoteWhy, std::string());
                    slots_.confirm();
                    running_.pending_verify = false;
                    annotations_dirty_ = true;
                    state_ = State::Idle;
                    report_idle();
                } else {
                    state_ = State::Rebooting;   // the simulator restarts the node
                }
            }
            return;
        }
        case State::Rebooting:
            if (reboot_at_ms_ && now >= reboot_at_ms_) {
                reboot_at_ms_ = 0;
                printf("[FIRMWARE] rebooting into %s\n", slots_.passive_slot().c_str());
                if (config_.before_reboot) config_.before_reboot();
                slots_.reboot();
            }
            return;
        case State::Downloading:
            download_step(now);
            return;
        default:
            break;
    }

    if (!registered || (last_check_ms_ && now - last_check_ms_ < config_.check_interval_ms)) return;
    last_check_ms_ = now;
    std::string target;
    if (api_.get_node_annotation(config_.node_name, models::kAnnotationFirmwareTarget, target) != 200) return;
    target = lower(target);
    if (!failed_.empty() && target != failed_) {
        // The failed image is no longer asked for: it may be tried again later
        failed_.clear();
        slots_.set_note(kNoteFail, std::string());
        annotations_dirty_ = true;
        if (state_ == State::Failed || state_ == State::RolledBack) {
            state_ = State::Idle;
            report_idle();
        }
    }
    if (target.empty() || target == running_.sha256) {
        if (state_ == State::Waiting) {
            state_ = State::Idle;
            report_idle();
        }
        return;
    }
    if (target == failed_ || !osal::Sha256::valid_hex(target)) return;
    start_update(target, now);
}

void FirmwareAgent::start_update(const std::string& target, uint64_t now) {
    std::string body;
    if (api_.get("/apis/kube32.io/v1/firmware", body) != 200) return;
    models::JsonValue doc;
    std::string err;
    const models::JsonValue* st = models::parse_json(body, doc, err) ? doc.get("status") : nullptr;
    if (!st || st->get_string("phase", "") != "Staged" || lower(st->get_string("sha256", "")) != target) {
        if (state_ != State::Waiting) {
            state_ = State::Waiting;
            set_condition(true, "Waiting", "image " + short_sha(target) + " is not staged on the control plane");
        }
        return;
    }
    target_ = target;
    size_ = static_cast<size_t>(st->get_int("size", 0));
    version_ = st->get_string("version", "");
    const std::string label = version_.empty() ? "image " + short_sha(target) : version_ + " (" + short_sha(target) + ")";
    if (size_ == 0 || size_ > slots_.slot_size()) {
        fail(target, "its size (" + std::to_string(size_) + " bytes) does not fit the " + std::to_string(slots_.slot_size()) + "-byte slot",
             "FirmwareUpdateFailed");
        return;
    }
    if (config_.local_staging) {
        // The control plane: the image is in its passive slot already
        std::string hex;
        if (!slots_.passive_sha256(size_, hex) || hex != target) {
            fail(target, "the staged slot " + slots_.passive_slot() + " does not hold it", "FirmwareUpdateFailed");
            return;
        }
        activate(now);
        return;
    }
    std::string why;
    if (!slots_.begin(why)) {
        fail(target, "the update slot cannot be written: " + why, "FirmwareUpdateFailed");
        return;
    }
    sha_.reset();
    offset_ = 0;
    progress_ms_ = now;
    state_ = State::Downloading;
    set_condition(true, "Downloading", label + ": 0 of " + kib(size_) + " to " + slots_.passive_slot());
    event("FirmwareDownloading", "downloading " + label + ", " + std::to_string(size_) + " bytes, to " + slots_.passive_slot(), false);
    printf("[FIRMWARE] downloading %s, %u bytes, to %s\n", label.c_str(), (unsigned)size_, slots_.passive_slot().c_str());
}

void FirmwareAgent::download_step(uint64_t now) {
    // The node still wants this image?
    if (now - last_check_ms_ >= config_.check_interval_ms) {
        last_check_ms_ = now;
        std::string target;
        if (api_.get_node_annotation(config_.node_name, models::kAnnotationFirmwareTarget, target) == 200 && lower(target) != target_) {
            slots_.abort();
            printf("[FIRMWARE] download of image %s stopped: the node's target changed\n", short_sha(target_).c_str());
            target_.clear();
            state_ = State::Idle;
            report_idle();
            last_check_ms_ = 0;
            return;
        }
    }
    const size_t want = std::min(config_.range_bytes, size_ - offset_);
    char path[200];
    snprintf(path, sizeof(path), "/apis/kube32.io/v1/firmware/image?sha256=%s&offset=%u&length=%u", target_.c_str(),
             (unsigned)offset_, (unsigned)want);
    size_t got = 0;
    std::string why;
    bool write_failed = false;
    const int status = api_.stream_get(path, [&](const char* data, size_t len) {
        len = std::min(len, want - got);
        if (!slots_.write(data, len, why)) {
            write_failed = true;
            return false;
        }
        sha_.update(data, len);
        got += len;
        offset_ += len;
        return got < want;
    });
    if (write_failed) {
        fail(target_, "writing " + slots_.passive_slot() + " failed: " + why, "FirmwareUpdateFailed");
        return;
    }
    if (got > 0) {
        progress_ms_ = now;
        const std::string label = version_.empty() ? "image " + short_sha(target_) : version_ + " (" + short_sha(target_) + ")";
        set_condition(true, "Downloading", label + ": " + kib(offset_) + " of " + kib(size_) + " to " + slots_.passive_slot());
    } else if (status == 409) {
        // Another image is staged now
        slots_.abort();
        target_.clear();
        state_ = State::Idle;
        last_check_ms_ = 0;
        return;
    } else if (now - progress_ms_ > config_.stall_timeout_ms) {
        slots_.abort();
        fail(target_, "the download made no progress for " + std::to_string(config_.stall_timeout_ms / 1000) + " s (http " +
                          std::to_string(status) + " at byte " + std::to_string(offset_) + ")", "FirmwareUpdateFailed");
        return;
    }
    if (offset_ < size_) return;

    const std::string digest = osal::Sha256::hex(sha_.finish());
    if (digest != target_) {
        slots_.abort();
        fail(target_, "the downloaded image's SHA-256 is " + short_sha(digest), "FirmwareUpdateFailed");
        return;
    }
    if (!slots_.finish(why)) {
        fail(target_, why, "FirmwareUpdateFailed");
        return;
    }
    activate(now);
}

void FirmwareAgent::activate(uint64_t now) {
    std::string why;
    if (!slots_.activate(why)) {
        fail(target_, "switching the boot slot failed: " + why, "FirmwareUpdateFailed");
        return;
    }
    slots_.set_note(kNoteTry, target_);
    const std::string label = version_.empty() ? "image " + short_sha(target_) : version_ + " (" + short_sha(target_) + ")";
    const std::string msg = label + " is checked in " + slots_.passive_slot() + "; rebooting into it";
    state_ = State::Rebooting;
    set_condition(true, "Rebooting", msg);
    event("FirmwareActivated", msg, false);
    printf("[FIRMWARE] %s\n", msg.c_str());
    reboot_at_ms_ = now + config_.reboot_delay_ms;
    heartbeat_wanted_.store(true);
}

} // namespace kube32::kubelet
