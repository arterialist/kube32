#include "controller/reconcile_trigger.hpp"

#include <algorithm>

namespace kube32::controller {

ReconcileTrigger::ReconcileTrigger(storage::Store& store, ReconcileTriggerConfig config) : store_(store), config_(config) {}

ReconcileTrigger::~ReconcileTrigger() {
    for (uint32_t id : watch_ids_) store_.unwatch(id);
}

void ReconcileTrigger::watch(const std::string& prefix, Filter filter) {
    filters_.push_back(filter ? std::make_unique<Filter>(std::move(filter)) : nullptr);
    const Filter* f = filters_.back().get();
    watch_ids_.push_back(store_.watch(prefix, [this, f](const storage::WatchEvent& ev) {
        if (!f || (*f)(ev)) note_change();
    }));
}

void ReconcileTrigger::watch_fields(const std::string& prefix, Fingerprint fingerprint) {
    const size_t index = fields_.size();
    fields_.push_back(FieldWatch{prefix, std::move(fingerprint), {}});
    {
        osal::LockGuard guard(mutex_);
        dirty_.resize(fields_.size());
    }
    watch_ids_.push_back(store_.watch(prefix, [this, index](const storage::WatchEvent& ev) { note_key(index, ev.key); }));
}

void ReconcileTrigger::note_change() {
    osal::LockGuard guard(mutex_);
    changed_ = true;
    cv_.notify_all();
}

void ReconcileTrigger::note_key(size_t field_watch, const std::string& key) {
    osal::LockGuard guard(mutex_);
    dirty_[field_watch].insert(key);
    cv_.notify_all();
}

void ReconcileTrigger::wake_at(uint64_t due_ms) {
    osal::LockGuard guard(mutex_);
    if (timer_ms_ == 0 || due_ms < timer_ms_) timer_ms_ = due_ms;
    cv_.notify_all();
}

void ReconcileTrigger::interrupt() {
    osal::LockGuard guard(mutex_);
    interrupted_ = true;
    cv_.notify_all();
}

uint64_t ReconcileTrigger::hash(std::initializer_list<std::string> parts) {
    uint64_t h = 1469598103934665603ull;
    for (const auto& s : parts) {
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
        h ^= 0xFF;
        h *= 1099511628211ull;
    }
    return h;
}

void ReconcileTrigger::prime_fields() {
    primed_ = true;
    for (auto& f : fields_) {
        std::vector<std::string> jsons, keys;
        store_.list_extended(f.prefix, jsons, &keys);
        for (size_t i = 0; i < jsons.size(); ++i) {
            models::JsonValue doc;
            std::string err;
            if (models::parse_json(jsons[i], doc, err) && doc.is_object()) f.seen[keys[i]] = f.fingerprint(doc);
        }
    }
}

bool ReconcileTrigger::fields_changed() {
    std::vector<std::set<std::string>> keys;
    {
        osal::LockGuard guard(mutex_);
        keys.swap(dirty_);
        dirty_.resize(fields_.size());
    }
    bool changed = false;
    for (size_t i = 0; i < keys.size(); ++i) {
        FieldWatch& f = fields_[i];
        for (const auto& key : keys[i]) {
            std::string json;
            models::JsonValue doc;
            std::string err;
            if (!store_.get(key, json).ok()) {
                changed |= f.seen.erase(key) > 0;
                continue;
            }
            if (!models::parse_json(json, doc, err) || !doc.is_object()) continue;
            const uint64_t fp = f.fingerprint(doc);
            auto it = f.seen.find(key);
            if (it == f.seen.end() || it->second != fp) {
                f.seen[key] = fp;
                changed = true;
            }
        }
    }
    return changed;
}

uint32_t ReconcileTrigger::due(uint64_t now_ms) {
    if (!primed_) prime_fields();
    if (ran_ && now_ms < last_pass_ms_ + config_.min_interval_ms) return kNone;
    uint32_t why = ran_ ? kNone : kChange;
    bool fields_dirty = false;
    {
        osal::LockGuard guard(mutex_);
        if (changed_) why |= kChange;
        changed_ = false;
        if (timer_ms_ && timer_ms_ <= now_ms) why |= kTimer;
        for (const auto& d : dirty_) fields_dirty |= !d.empty();
    }
    if (fields_dirty && fields_changed()) why |= kChange;
    if (ran_ && now_ms >= next_resync_ms_) why |= kResync;
    if (why == kNone) return kNone;
    {
        osal::LockGuard guard(mutex_);
        timer_ms_ = 0;
    }
    ran_ = true;
    last_pass_ms_ = now_ms;
    next_resync_ms_ = now_ms + config_.resync_interval_ms;
    return why;
}

void ReconcileTrigger::wait(uint64_t now_ms, uint32_t max_ms) {
    osal::UniqueLock<osal::Mutex> lock(mutex_);
    if (interrupted_) {
        interrupted_ = false;
        return;
    }
    // A change or a time asked for waits for the shortest interval between passes
    const uint64_t earliest = ran_ ? last_pass_ms_ + config_.min_interval_ms : now_ms;
    uint64_t until = now_ms + max_ms;
    if (ran_) until = std::min(until, std::max(next_resync_ms_, earliest));
    if (timer_ms_) until = std::min(until, std::max(timer_ms_, earliest));
    bool pending = changed_ || !ran_;
    for (const auto& d : dirty_) pending |= !d.empty();
    if (pending) until = std::min(until, earliest);
    if (until <= now_ms) return;
    cv_.wait_for(lock, static_cast<uint32_t>(until - now_ms));
    interrupted_ = false;
}

} // namespace kube32::controller
