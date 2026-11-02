#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace kube32::osal {

class Thread {
public:
    using EntryFn = void (*)(void* arg);

    struct Config {
        const char* name;
        size_t stack_size;    // Stack size in bytes
        uint32_t priority;   // FreeRTOS priority (1..24) or POSIX priority
        int core_affinity;   // -1: any/no affinity, 0: Core 0, 1: Core 1
        bool stack_in_psram; // ESP32: allocate the stack from external RAM when present
        void* stack_buffer;  // ESP32: run on this caller-owned stack (stack_size bytes) instead of allocating one

        Config() : name("k32_task"), stack_size(8192), priority(5), core_affinity(-1), stack_in_psram(false), stack_buffer(nullptr) {}
    };

    Thread();
    ~Thread();

    // Movable, non-copyable
    Thread(Thread&& other) noexcept;
    Thread& operator=(Thread&& other) noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    // Start thread execution with specified entry point and configuration
    bool start(EntryFn entry, void* arg, const Config& config = Config());

    // Wait for thread termination (if joinable)
    bool join();

    // Detach thread from this object
    void detach();

    // Check if thread can be joined
    bool is_joinable() const;

    // CPU time this thread has run so far, in microseconds; false once it
    // has ended or where it cannot be measured. The caller keeps the thread
    // from ending during the call. On the ESP32 the figure is the task's
    // FreeRTOS run-time counter, which grows when the task is switched out.
    bool cpu_time_us(uint64_t& out_us) const;
    // CPU time the calling thread has run, in microseconds; false where it
    // cannot be measured. Time spent blocked (locks, sockets, sleeps) is
    // not CPU time. On the ESP32 the call yields once so that the slice
    // running now is counted.
    static bool current_cpu_time_us(uint64_t& out_us);

    // Static concurrency utilities
    static bool create(EntryFn entry, void* arg, const Config& config = Config());
    static void sleep_ms(uint32_t ms);
    static void sleep_us(uint64_t us);
    static void yield();
    static uint64_t get_time_ms();
    static uint64_t get_time_us();
    static const char* get_current_name();
    static int get_current_core();
    // Identifies the calling thread while it runs (its task or pthread)
    static uintptr_t current_id();

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace kube32::osal
