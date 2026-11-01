#pragma once

// When a control loop runs a pass, as client-go's informers and work queues
// decide it: after a change under the store prefixes the loop reads, at a
// time the loop asked for (a retry, a back-off), and once per resync period,
// which repairs what a change it missed would have left. Passes are at
// least min_interval_ms apart, so a burst of changes costs one pass.
//
// The store calls the watches on the thread that wrote, after the write:
// they only note the change. A watch with a fingerprint makes the change
// count when the fingerprint of the object differs from the last one seen
// (or the object came or went); it is worked out on the loop's thread, from
// the stored object, so a Node heartbeat that moves no field the loop reads
// costs one parse there and no pass.

#include "models/kube_models.hpp"
#include "osal/condition_variable.hpp"
#include "osal/mutex.hpp"
#include "storage/store.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace kube32::controller {

struct ReconcileTriggerConfig {
    uint32_t min_interval_ms{1000};    // shortest time between two passes
    uint32_t resync_interval_ms{30000};
};

class ReconcileTrigger {
public:
    using Filter = std::function<bool(const storage::WatchEvent& event)>;
    using Fingerprint = std::function<uint64_t(const models::JsonValue& object)>;

    // Why a pass is due (bits of due()'s result)
    enum Reason : uint32_t { kNone = 0, kChange = 1, kTimer = 2, kResync = 4 };

    ReconcileTrigger(storage::Store& store, ReconcileTriggerConfig config = {});
    ~ReconcileTrigger();
    ReconcileTrigger(const ReconcileTrigger&) = delete;
    ReconcileTrigger& operator=(const ReconcileTrigger&) = delete;

    // A change under `prefix` (one `filter` accepts, when given) is due
    void watch(const std::string& prefix, Filter filter = nullptr);
    // A change under `prefix` is due when `fingerprint` of the object changed
    void watch_fields(const std::string& prefix, Fingerprint fingerprint);

    // A pass is due at `due_ms` (monotonic clock) at the latest. A pass
    // forgets the times asked for before it: it asks again for those it
    // still needs.
    void wake_at(uint64_t due_ms);
    // Ends a wait() now (stopping the loop)
    void interrupt();

    // The reasons a pass is due at `now_ms`, 0 when none is. When one is, the
    // changes noted so far count as handled: call it before the pass reads
    // the store, so a change made during the pass makes the next one due.
    uint32_t due(uint64_t now_ms);
    // Blocks until a pass may be due (a change, a time asked for, the
    // resync) or interrupt(), at most `max_ms`
    void wait(uint64_t now_ms, uint32_t max_ms);

    // Fingerprint of an object's fields, FNV-1a over `parts` (for Fingerprint functions)
    static uint64_t hash(std::initializer_list<std::string> parts);

private:
    struct FieldWatch {
        std::string prefix;
        Fingerprint fingerprint;
        std::map<std::string, uint64_t> seen;   // key -> fingerprint last seen
    };

    void note_change();
    void note_key(size_t field_watch, const std::string& key);
    // Whether a key noted under the field watches changed its fingerprint
    bool fields_changed();
    void prime_fields();

    storage::Store& store_;
    ReconcileTriggerConfig config_;
    std::vector<uint32_t> watch_ids_;
    // The filters, where the store's copies of a watch callback reach them:
    // a callback holding two pointers is copied without an allocation
    std::vector<std::unique_ptr<Filter>> filters_;
    std::vector<FieldWatch> fields_;              // set before the loop runs
    mutable osal::Mutex mutex_;
    osal::ConditionVariable cv_;
    bool changed_{false};                         // guarded by mutex_
    std::vector<std::set<std::string>> dirty_;    // per field watch, guarded by mutex_
    uint64_t timer_ms_{0};                        // the earliest time asked for, 0 for none; guarded by mutex_
    bool interrupted_{false};                     // guarded by mutex_
    bool primed_{false};
    uint64_t last_pass_ms_{0};
    uint64_t next_resync_ms_{0};
    bool ran_{false};                             // a pass ran: the first one is due at once
};

} // namespace kube32::controller
