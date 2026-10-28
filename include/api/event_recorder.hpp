#pragma once

// core/v1 Events: recorded by the scheduler, the node lifecycle controller and
// the API server (RegisteredNode), and accepted from kubelets through
// POST /api/v1/namespaces/{ns}/events. Identical events (same object, reason,
// message, component) merge into one record with count/lastTimestamp updated,
// and the store never holds more than `max_events` (oldest dropped first).
// Dedup and the bound are evaluated against the store itself, so several
// recorders on one store (API server, scheduler, node controller) behave as
// one bounded set.

#include "storage/store.hpp"
#include "osal/mutex.hpp"

#include <deque>
#include <map>
#include <string>

namespace kube32::api {

struct EventRef {
    std::string kind;          // "Pod", "Node"
    std::string name;
    std::string namespace_name; // empty for cluster-scoped objects
    std::string uid;            // may be empty when the object has none
    std::string api_version{"v1"};
};

struct EventSpec {
    EventRef involved;
    std::string reason;         // "Scheduled", "Started", "NodeNotReady"
    std::string message;
    bool warning{false};        // type: Warning vs Normal
    std::string component;      // "default-scheduler", "kubelet", "node-controller"
    std::string host;           // node name that observed it (source.host)
};

class EventRecorder {
public:
    EventRecorder(storage::Store& store, size_t max_events = 64);

    // Stores the event (or merges into an identical recent one) and returns
    // the JSON that landed in the store. Events are ephemeral (never flashed).
    std::string record(const EventSpec& spec);

    // Admits a client-submitted core/v1 Event. Missing name/uid/timestamps are
    // filled in; dedup and the bound apply. False when the object is invalid.
    bool admit(const std::string& event_json, std::string& out_stored, std::string& err);

    size_t max_events() const { return max_; }
    size_t count() const;

    // events.k8s.io/v1 projection of a stored core/v1 Event.
    static std::string to_events_v1(const std::string& core_json);

private:
    // Finds an existing event for the same object/reason/message/component.
    bool find_duplicate(const std::string& ns, const EventSpec& spec, std::string& out_key, std::string& out_json) const;
    void enforce_bound();

    storage::Store& store_;
    size_t max_;
    mutable osal::Mutex mutex_;
};

} // namespace kube32::api
