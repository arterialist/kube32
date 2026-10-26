#pragma once

#include <cstdint>
#include <cstddef>

namespace kube32::osal {

class Timer {
public:
    using CallbackFn = void (*)(void* arg);

    enum class Type {
        OneShot,
        Periodic
    };

    struct Config {
        const char* name;
        CallbackFn callback;
        void* arg;
        Type type;
        uint64_t period_ms;

        Config() : name("k32_timer"), callback(nullptr), arg(nullptr), type(Type::OneShot), period_ms(1000) {}
    };

    Timer();
    explicit Timer(const Config& config);
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    Timer(Timer&& other) noexcept;
    Timer& operator=(Timer&& other) noexcept;

    bool init(const Config& config);
    bool start(uint64_t period_ms, bool periodic = false);
    bool stop();
    bool is_running() const;

    static uint64_t get_time_ms();
    static uint64_t get_time_us();

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace kube32::osal
