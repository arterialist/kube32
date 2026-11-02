#pragma once

// CPU time of pods, as the kubelet reports it (metrics.k8s.io). A pod runs
// on two kinds of threads: its own thread for an entry point (_start,
// app_main, main), whose CPU time the thread itself counts, and the node's
// threads that call its exports (k32_handle, k32_tick, k32_event, k32_init),
// where a PodCpuRegion counts the CPU time of each call. Time a call spends
// blocked (waiting for the pod's call lock, in a socket, in a sleep) is not
// CPU time and is not counted.

#include <atomic>
#include <cstdint>

namespace kube32::runtime {

// The calling thread's CPU time from construction to finish(). Regions nest
// on one thread (a pod calling a pod of the same node, on the caller's
// thread): each counts its own time only, the callee's goes to the callee.
class PodCpuRegion {
public:
    PodCpuRegion();
    ~PodCpuRegion() { finish(); }
    PodCpuRegion(const PodCpuRegion&) = delete;
    PodCpuRegion& operator=(const PodCpuRegion&) = delete;

    // Ends the region: its CPU time in microseconds, nested regions left out
    // (0 where the platform cannot measure CPU time)
    uint64_t finish();

private:
    PodCpuRegion* parent_;
    uint64_t start_us_{0};
    uint64_t nested_us_{0};
    bool measured_{false};
    bool done_{false};
};

// Called on a pod's own thread: the CPU time of regions that run on it
// outside any other region (calls into other pods) is added to `sink`, so
// the thread's own figure can leave it out. Null clears it.
void set_thread_nested_cpu_sink(std::atomic<uint64_t>* sink);

} // namespace kube32::runtime
