// AppPodHost's part in moving pods (docs/migration.md): migration blockers,
// freeze and thaw, snapshots of frozen pods, restores. Runs once per
// checkpoint or move, so it is built for size (src/CMakeLists.txt).

#include "runtime/app_pod.hpp"
#include "runtime/k32_app_abi.h"
#include "runtime/pod_fs.hpp"
#include "ipc/channel.hpp"
#include "osal/system.hpp"

#include <cstdio>

namespace kube32::runtime {

void AppPodHost::set_migration_info(const std::string& pod_id, const DeploySpec& spec) {
    osal::LockGuard guard(mutex_);
    MigrationInfo& m = migration_[pod_id];
    m.container = spec.container_name.empty() ? pod_id.substr(pod_id.find('/') + 1) : spec.container_name;
    m.blockers = spec.migration_blockers;
}

// The entry point of a module, null for a request pod
static const char* entry_point(ContainerLifecycleController& ctrl) {
    for (const char* name : {"_start", "app_main", "main"}) {
        if (ctrl.has_export(name)) return name;
    }
    return nullptr;
}

void AppPodHost::note_entry_point(const std::string& pod_id, ContainerLifecycleController& ctrl) {
    if (const char* entry = entry_point(ctrl)) {
        char line[64];
        snprintf(line, sizeof(line), "entry point %s runs on its own thread", entry);
        osal::LockGuard guard(mutex_);
        migration_[pod_id].blockers.emplace_back(line);
    }
}

bool AppPodHost::container_name(const std::string& pod_id, std::string& out) const {
    osal::LockGuard guard(mutex_);
    auto it = migration_.find(pod_id);
    if (it == migration_.end()) return false;
    out = it->second.container;
    return true;
}

std::vector<std::string> AppPodHost::migration_blockers(const std::string& pod_id) const {
    std::vector<std::string> out;
    {
        osal::LockGuard guard(mutex_);
        auto it = migration_.find(pod_id);
        if (it != migration_.end()) out = it->second.blockers;
    }
    char line[K32_FS_MAX_PATH + 40];
    for (const auto& f : PodFs::instance().open_files(pod_id)) {
        snprintf(line, sizeof(line), "open file %s (descriptor %d)", f.second.c_str(), (int)f.first);
        out.emplace_back(line);
    }
    return out;
}

bool AppPodHost::is_frozen(const std::string& pod_id) const {
    osal::LockGuard guard(mutex_);
    auto it = pods_.find(pod_id);
    return it != pods_.end() && it->second.frozen;
}

int AppPodHost::freeze(const std::string& pod_id, uint32_t timeout_ms, std::string& why) {
    osal::Mutex* cm = nullptr;
    {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        if (!e) return -2;
        if (e->frozen) return 0;
        e->frozen = true;   // calls that have not begun see it once they hold the call lock
        cm = e->call_mutex.get();
    }
    // The call in progress, if any, returns first
    if (!cm->try_lock(timeout_ms ? timeout_ms : 1)) {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        char line[96];
        if (e) {
            e->frozen = false;
            snprintf(line, sizeof(line), "call %s still running after %u ms", e->busy_fn ? e->busy_fn : "into the pod", (unsigned)timeout_ms);
        } else {
            snprintf(line, sizeof(line), "the pod stopped while it was being frozen");
        }
        why = line;
        return -1;
    }
    // Events queued for the pod before it froze are held with the rest
    std::vector<HeldEvent> queued;
    {
        osal::LockGuard guard(event_mutex_);
        std::deque<PendingEvent> others;
        for (auto& ev : event_queue_) {
            if (ev.pod_id() == pod_id) queued.push_back(HeldEvent{std::string(ev.topic()), std::string(ev.data())});
            else others.push_back(std::move(ev));
        }
        event_queue_.swap(others);
    }
    {
        osal::LockGuard guard(mutex_);
        if (Entry* e = find_locked(pod_id)) {
            for (auto& q : queued) {
                if (e->held.size() < kMaxHeldEvents) e->held.push_back(std::move(q));
                else e->held_dropped++;
            }
            e->frozen_pod_time_ms = static_cast<uint64_t>(static_cast<int64_t>(osal::System::get_uptime_ms()) + e->clock_offset_ms);
            e->frozen_unix_ms = osal::System::get_unix_time_ms();
        }
    }
    cm->unlock();
    printf("[POD] %s: frozen\n", pod_id.c_str());
    return 0;
}

void AppPodHost::requeue_events(const std::string& pod_id, std::vector<HeldEvent>&& events) {
    if (events.empty()) return;
    {
        // Ahead of what arrived since, in their order
        osal::LockGuard guard(event_mutex_);
        std::deque<PendingEvent> merged;
        for (auto& e : events) merged.emplace_back(pod_id, e.topic, e.data);
        for (auto& e : event_queue_) merged.push_back(std::move(e));
        event_queue_.swap(merged);
    }
    start_event_thread();
    event_cv_.notify_one();
}

bool AppPodHost::thaw(const std::string& pod_id) {
    std::vector<HeldEvent> held;
    {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        if (!e || !e->frozen) return false;
        e->frozen = false;
        held.swap(e->held);
        if (e->held_dropped) printf("[POD] %s: %u events dropped while frozen\n", pod_id.c_str(), (unsigned)e->held_dropped);
        e->held_dropped = 0;
    }
    printf("[POD] %s: thawed\n", pod_id.c_str());
    requeue_events(pod_id, std::move(held));
    return true;
}

bool AppPodHost::snapshot(const std::string& pod_id, SnapshotWrite out, SnapshotSummary& summary, std::string& why) {
    ContainerLifecycleController* ctrl = nullptr;
    osal::Mutex* cm = nullptr;
    {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        if (!e || !e->frozen) {
            why = e ? "the pod is not frozen" : "no such pod";
            return false;
        }
        ctrl = e->ctrl;
        cm = e->call_mutex.get();
    }
    // Nothing runs in the pod while it is read; unregister_pod waits for this
    if (!cm->try_lock(5000)) {
        why = "the pod is busy";
        return false;
    }
    SnapshotExtras extras;
    std::vector<HeldEvent> held;
    bool frozen = false;
    {
        osal::LockGuard guard(mutex_);
        if (Entry* e = find_locked(pod_id); e && e->frozen) {
            frozen = true;
            extras.pod_time_ms = e->frozen_pod_time_ms;
            extras.unix_ms = e->frozen_unix_ms;
            held = e->held;   // a copy: a thaw still delivers them here
        }
    }
    if (frozen) {
        extras.channels = ipc::ChannelManager::instance().handles_of(pod_id);
        extras.events = &held;
        ctrl->image_sha256(summary.image_sha);
        summary.pod_time_ms = extras.pod_time_ms;
        summary.bytes = write_snapshot(*ctrl, summary.image_sha, extras, out, summary.digest, why);
    } else {
        why = "the pod is no longer frozen";
        summary.bytes = 0;
    }
    cm->unlock();
    return summary.bytes > 0;
}

int AppPodHost::restore_pod(const std::string& pod_id, const SnapshotStream& stream, std::string& why) {
    ContainerLifecycleController* ctrl = nullptr;
    osal::Mutex* cm = nullptr;
    bool has_event = false;
    {
        osal::LockGuard guard(mutex_);
        Entry* e = find_locked(pod_id);
        if (!e) {
            why = std::string(kRestoreErrorPrefix) + "the module exports no request entry point (k32_handle, k32_tick or k32_event)";
            return -1;
        }
        ctrl = e->ctrl;
        cm = e->call_mutex.get();
        has_event = e->has_event;
    }
    if (const char* entry = entry_point(*ctrl)) {
        why = std::string(kRestoreErrorPrefix) + "entry point " + entry + " runs on its own thread";
        return -1;
    }
    const bool has_restored = ctrl->has_export(K32_EXPORT_RESTORED);
    osal::LockGuard call_guard(*cm);
    uint8_t sha[32];
    ctrl->image_sha256(sha);
    SnapshotRestorer r(*ctrl, sha);
    std::string stream_why;
    const bool streamed = stream([&](const uint8_t* d, size_t n) { return r.feed(d, n); }, stream_why);
    if (!r.why().empty() || !streamed || !r.finish()) {
        // A failed transfer explains a refused snapshot (an error page is no K32S)
        why = kRestoreErrorPrefix;
        why += !stream_why.empty() ? stream_why : !r.why().empty() ? r.why() : std::string("the snapshot ended early");
        return -1;
    }
    for (const auto& c : r.channels) {
        if (ipc::ChannelManager::instance().open_as(pod_id, c.second, 0, c.first) != c.first) {
            why = std::string(kRestoreErrorPrefix) + "channel " + c.second + " cannot be opened again under its handle";
            return -1;
        }
    }
    // The pod's clock goes on from the snapshot's
    const int64_t offset = static_cast<int64_t>(r.header.pod_time_ms) - static_cast<int64_t>(osal::System::get_uptime_ms());
    set_pod_clock_offset(ctrl->m3_runtime_handle(), offset);
    {
        osal::LockGuard guard(mutex_);
        if (Entry* e = find_locked(pod_id)) e->clock_offset_ms = offset;
    }
    if (has_restored) {
        const int32_t rc = call_export(pod_id, ctrl, K32_EXPORT_RESTORED);
        if (rc != 0) {
            why = "k32_restored returned " + std::to_string(rc);
            return -2;
        }
    }
    {
        osal::LockGuard guard(mutex_);
        if (Entry* e = find_locked(pod_id)) {
            e->ticking = true;
            e->next_tick_ms = 0;
        }
    }
    if (has_event) requeue_events(pod_id, std::move(r.events));
    return 0;
}

} // namespace kube32::runtime
