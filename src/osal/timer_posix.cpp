#include "osal/timer.hpp"

#if defined(ESP_PLATFORM)

#include <esp_timer.h>
#include <atomic>

namespace kube32::osal {

struct Timer::Impl {
    Config config;
    esp_timer_handle_t handle{nullptr};
    std::atomic<bool> running{false};
    std::atomic<bool> periodic{false};

    static void esp_timer_callback(void* arg) {
        auto* impl = static_cast<Impl*>(arg);
        if (impl) {
            // For one-shot timers, reset running flag upon callback execution
            if (!impl->periodic.load(std::memory_order_acquire)) {
                impl->running.store(false, std::memory_order_release);
            }
            if (impl->config.callback) {
                impl->config.callback(impl->config.arg);
            }
        }
    }
};

Timer::Timer() : impl_(new Impl()) {}

Timer::Timer(const Config& config) : impl_(new Impl()) {
    init(config);
}

Timer::~Timer() {
    stop();
    if (impl_) {
        if (impl_->handle) {
            esp_timer_delete(impl_->handle);
        }
        delete impl_;
        impl_ = nullptr;
    }
}

Timer::Timer(Timer&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        stop();
        if (impl_ && impl_->handle) esp_timer_delete(impl_->handle);
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool Timer::init(const Config& config) {
    if (!impl_) return false;
    stop();
    if (impl_->handle) {
        esp_timer_delete(impl_->handle);
        impl_->handle = nullptr;
    }
    impl_->config = config;

    esp_timer_create_args_t args;
    args.callback = &Impl::esp_timer_callback;
    args.arg = impl_;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = config.name ? config.name : "k32_timer";
    args.skip_unhandled_events = false;

    return (esp_timer_create(&args, &impl_->handle) == ESP_OK);
}

bool Timer::start(uint64_t period_ms, bool periodic) {
    if (!impl_ || !impl_->handle) return false;
    stop();
    uint64_t period_us = period_ms * 1000ULL;
    esp_err_t err = periodic ? esp_timer_start_periodic(impl_->handle, period_us)
                             : esp_timer_start_once(impl_->handle, period_us);
    if (err == ESP_OK) {
        impl_->periodic.store(periodic, std::memory_order_release);
        impl_->running.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

bool Timer::stop() {
    if (impl_ && impl_->handle && impl_->running.load(std::memory_order_acquire)) {
        esp_timer_stop(impl_->handle);
        impl_->running.store(false, std::memory_order_release);
    }
    return true;
}

bool Timer::is_running() const {
    return impl_ && impl_->running.load(std::memory_order_acquire);
}

uint64_t Timer::get_time_ms() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

uint64_t Timer::get_time_us() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

} // namespace kube32::osal

#else // Host POSIX implementation

#include <thread>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <atomic>

namespace kube32::osal {

struct Timer::Impl {
    Config config;
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> running{false};
    std::atomic<bool> stop_requested{false};
    uint64_t period_ms{0};
    bool periodic{false};

    void run() {
        std::unique_lock<std::mutex> lock(mtx);
        while (!stop_requested.load()) {
            if (cv.wait_for(lock, std::chrono::milliseconds(period_ms), [this] {
                return stop_requested.load();
            })) {
                break; // Stop requested
            }
            if (stop_requested.load()) break;

            if (config.callback) {
                // Execute callback without holding timer mutex
                auto cb = config.callback;
                void* arg = config.arg;
                lock.unlock();
                cb(arg);
                lock.lock();
            }

            if (!periodic) {
                running.store(false);
                break;
            }
        }
    }
};

Timer::Timer() : impl_(new Impl()) {}

Timer::Timer(const Config& config) : impl_(new Impl()) {
    init(config);
}

Timer::~Timer() {
    stop();
    delete impl_;
    impl_ = nullptr;
}

Timer::Timer(Timer&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        stop();
        delete impl_;
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool Timer::init(const Config& config) {
    if (!impl_) return false;
    stop();
    impl_->config = config;
    return true;
}

bool Timer::start(uint64_t period_ms, bool periodic) {
    if (!impl_ || !impl_->config.callback) return false;
    stop();
    impl_->period_ms = period_ms;
    impl_->periodic = periodic;
    impl_->stop_requested.store(false);
    impl_->running.store(true);
    impl_->worker = std::thread(&Impl::run, impl_);
    return true;
}

bool Timer::stop() {
    if (!impl_) return false;
    if (impl_->running.load() || impl_->worker.joinable()) {
        impl_->stop_requested.store(true);
        impl_->cv.notify_all();
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        impl_->running.store(false);
    }
    return true;
}

bool Timer::is_running() const {
    return impl_ && impl_->running.load();
}

uint64_t Timer::get_time_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t Timer::get_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace kube32::osal

#endif
