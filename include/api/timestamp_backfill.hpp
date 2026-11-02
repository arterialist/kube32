#pragma once

// Objects created before the wall clock is set (SNTP has not answered yet)
// carry no metadata.creationTimestamp, and Events no firstTimestamp or
// lastTimestamp: the API server omits times it does not know rather than
// writing 1970. TimestampBackfill notes, on the uptime clock, when each such
// object was created (and when an Event was last updated). Once the clock is
// set it stamps them with that moment in wall-clock time: now minus the
// uptime that has passed since. Objects it did not see created (they come
// from an earlier boot) stay without a timestamp; kubectl shows their AGE
// as <unknown>.

#include "storage/store.hpp"
#include "osal/mutex.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace kube32::api {

class TimestampBackfill {
public:
    using Clock = std::function<uint64_t()>;

    // Watches the objects created in `store` from now on, at most
    // `max_objects` of them (a note takes 24 bytes; more objects stay
    // unstamped). The clocks default to the system's.
    explicit TimestampBackfill(storage::Store& store, size_t max_objects = 256, Clock unix_ms = nullptr, Clock uptime_ms = nullptr);
    ~TimestampBackfill();
    TimestampBackfill(const TimestampBackfill&) = delete;
    TimestampBackfill& operator=(const TimestampBackfill&) = delete;

    // Stamps the objects noted, once the clock is set. Returns the number of
    // objects updated; 0 while the clock is not set. An object that could
    // not be written stays noted for the next run.
    size_t run();
    // Objects noted and not stamped yet
    size_t pending() const;

private:
    struct Note {
        uint64_t key_hash;
        uint64_t created_ms;    // uptime
        uint64_t updated_ms;    // uptime of the last update before the clock was set
    };
    void on_change(const storage::WatchEvent& event);

    storage::Store& store_;
    size_t max_objects_;
    Clock unix_ms_;
    Clock uptime_ms_;
    uint32_t watch_id_{0};
    mutable osal::Mutex mutex_;
    std::vector<Note> notes_;
};

} // namespace kube32::api
