#include "models/migration.hpp"

#include <cstdlib>

namespace kube32::models {

namespace {

// The value in place, empty when absent
std::string_view annotation(const JsonValue& obj, const char* key) {
    const JsonValue* meta = obj.get("metadata");
    const JsonValue* ann = meta ? meta->get("annotations") : nullptr;
    const JsonValue* v = ann ? ann->get(key) : nullptr;
    return v && v->is_string() ? std::string_view(v->str_val) : std::string_view();
}

constexpr std::string_view kHardwarePrefix = "hardware.kube32.io/";

// The followed devices in the order kube32.io/needs lists them: the
// resource name after "hardware.kube32.io/" and the capability names
struct DeviceName {
    uint32_t bit;
    const char* resource;
    const char* cap;
    const char* alias;
};
constexpr DeviceName kDevices[] = {
    {hal::KUBE32_CAP_DISPLAY, "tft-display", "DISPLAY", "TFT_DISPLAY"},
    {hal::KUBE32_CAP_KEYBOARD, "keyboard", "KEYBOARD", nullptr},
    {hal::KUBE32_CAP_TOUCH, "touch", "TOUCH", nullptr},
    {hal::KUBE32_CAP_SPEAKER, "speaker", "SPEAKER", "AUDIO"},
    {hal::KUBE32_CAP_MICROPHONE, "microphone", "MICROPHONE", "MIC"},
    {hal::KUBE32_CAP_LED, "led", "LED", nullptr},
    {hal::KUBE32_CAP_CHIP_TEMP, "chip-temp", "CHIP_TEMP", nullptr},
    {hal::KUBE32_CAP_CAMERA, "camera", "CAMERA", nullptr},
    {hal::KUBE32_CAP_INTERACT, "interact", "INTERACT", nullptr},
    {hal::KUBE32_CAP_IMU, "imu", "IMU", nullptr},
    {hal::KUBE32_CAP_MOTOR, "motor", "MOTOR", nullptr},
};

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// Calls `each` with every trimmed, non-empty entry of a comma-separated list
// until it returns false; false then
template <typename F>
bool for_each_entry(std::string_view list, F each) {
    while (!list.empty()) {
        const size_t comma = list.find(',');
        const std::string_view entry = trim(list.substr(0, comma));
        if (!entry.empty() && !each(entry)) return false;
        list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
    }
    return true;
}

} // namespace

uint32_t device_bit(std::string_view name) {
    if (name.substr(0, kHardwarePrefix.size()) == kHardwarePrefix) name.remove_prefix(kHardwarePrefix.size());
    for (const DeviceName& d : kDevices) {
        if (name == d.resource || name == d.cap || (d.alias && name == d.alias)) return d.bit;
    }
    return 0;
}

bool parse_devices(std::string_view list, uint32_t& out) {
    out = 0;
    return for_each_entry(list, [&out](std::string_view entry) {
        const uint32_t bit = device_bit(entry);
        out |= bit;
        return bit != 0;
    });
}

std::string devices_text(uint32_t devices) {
    std::string out;
    for (const DeviceName& d : kDevices) {
        if (!(devices & d.bit)) continue;
        if (!out.empty()) out += ',';
        out.append(kHardwarePrefix.data(), kHardwarePrefix.size());
        out += d.resource;
    }
    return out;
}

bool follows_hardware(const JsonValue& pod) {
    return migration_policy(pod) == MigrationPolicy::Auto && annotation(pod, kAnnotationPlacement) == "hardware";
}

uint32_t pod_needs(const JsonValue& pod) {
    uint32_t devices = 0;
    parse_devices(annotation(pod, kAnnotationNeeds), devices);
    return devices;
}

uint32_t node_devices(const JsonValue& node) {
    const JsonValue* st = node.get("status");
    const JsonValue* res = st ? st->get("allocatable") : nullptr;
    if (!res || !res->is_object()) res = st ? st->get("capacity") : nullptr;
    uint32_t devices = 0;
    for (size_t i = 0; res && res->is_object() && i < res->obj_val.size(); ++i) {
        const auto& kv = res->obj_val[i];
        // A count the kubelet writes ("8"), a number, or "0" when it has none
        const JsonValue& v = kv.second;
        if (v.is_string() ? !v.str_val.empty() && v.str_val != "0" : v.num_val > 0) devices |= device_bit(kv.first);
    }
    return devices;
}

int preferred_rank(const JsonValue& pod, const JsonValue& node) {
    const JsonValue* meta = node.get("metadata");
    const JsonValue* name = meta ? meta->get("name") : nullptr;
    const JsonValue* labels = meta ? meta->get("labels") : nullptr;
    int rank = 0, found = -1;
    for_each_entry(annotation(pod, kAnnotationPreferNodes), [&](std::string_view entry) {
        const size_t eq = entry.find('=');
        bool match = false;
        if (eq == std::string_view::npos) {
            match = name && name->is_string() && entry == name->str_val;
        } else {
            const std::string_view key = trim(entry.substr(0, eq)), value = trim(entry.substr(eq + 1));
            for (size_t i = 0; labels && labels->is_object() && i < labels->obj_val.size() && !match; ++i) {
                const auto& kv = labels->obj_val[i];
                match = kv.first == key && kv.second.is_string() && kv.second.str_val == value;
            }
        }
        if (match) found = rank;
        ++rank;
        return !match;
    });
    return found;
}

MigrationPolicy migration_policy(const JsonValue& pod) {
    const std::string_view p = annotation(pod, kAnnotationMigration);
    if (p == "Never") return MigrationPolicy::Never;
    if (p == "Auto") return MigrationPolicy::Auto;
    return MigrationPolicy::OnDrain;
}

uint64_t parse_duration_ms(const std::string& text) {
    char* end = nullptr;
    const unsigned long long n = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str()) return 0;
    const std::string unit(end);
    if (unit.empty() || unit == "ms") return n;
    if (unit == "s") return n * 1000ULL;
    if (unit == "m") return n * 60000ULL;
    if (unit == "h") return n * 3600000ULL;
    return 0;
}

std::string checkpoint_json(const std::string& ns, const std::string& name, const std::string& pod, const std::string& action,
                            const std::string& target_node, const std::string& trigger) {
    // Names are DNS labels: nothing in them needs escaping
    const std::string now = now_rfc3339();
    return str_cat({R"({"apiVersion":"kube32.io/v1","kind":"Checkpoint","metadata":{"name":")", name, R"(","namespace":")", ns, R"(","uid":")",
                    generate_uid(), "\"", now.empty() ? "" : R"(,"creationTimestamp":")", now, now.empty() ? "" : "\"",
                    trigger.empty() ? "" : R"(,"labels":{"kube32.io/trigger":")", trigger, trigger.empty() ? "" : "\"}",
                    R"(},"spec":{"podName":")", pod, R"(","action":")", action, "\"", target_node.empty() ? "" : R"(,"target":{"nodeName":")",
                    target_node, target_node.empty() ? "" : "\"}", R"(},"status":{"phase":"Pending"}})"});
}

} // namespace kube32::models
