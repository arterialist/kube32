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

        Config() : name("k32_task"), stack_size(4096), priority(5), core_affinity(-1) {}
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

    // Static concurrency utilities
    static bool create(EntryFn entry, void* arg, const Config& config = Config());
    static void sleep_ms(uint32_t ms);
    static void sleep_us(uint64_t us);
    static void yield();
    static uint64_t get_time_ms();
    static uint64_t get_time_us();
    static const char* get_current_name();
    static int get_current_core();

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace kube32::osal
