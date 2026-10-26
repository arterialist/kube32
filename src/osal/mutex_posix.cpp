#include "osal/mutex.hpp"

#if defined(ESP_PLATFORM)

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace kube32::osal {

struct Mutex::Impl {
    SemaphoreHandle_t sem{nullptr};
};

Mutex::Mutex() : impl_(new Impl()) {
    impl_->sem = xSemaphoreCreateMutex();
}

Mutex::~Mutex() {
    if (impl_) {
        if (impl_->sem) {
            vSemaphoreDelete(impl_->sem);
        }
        delete impl_;
        impl_ = nullptr;
    }
}

void Mutex::lock() {
    if (impl_ && impl_->sem) {
        xSemaphoreTake(impl_->sem, portMAX_DELAY);
    }
}

bool Mutex::try_lock(uint32_t timeout_ms) {
    if (!impl_ || !impl_->sem) return false;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTake(impl_->sem, ticks) == pdTRUE);
}

void Mutex::unlock() {
    if (impl_ && impl_->sem) {
        xSemaphoreGive(impl_->sem);
    }
}

void* Mutex::native_handle() {
    return impl_ ? static_cast<void*>(impl_->sem) : nullptr;
}

// -----------------------------------------------------------------------------
// RecursiveMutex
// -----------------------------------------------------------------------------
struct RecursiveMutex::Impl {
    SemaphoreHandle_t sem{nullptr};
};

RecursiveMutex::RecursiveMutex() : impl_(new Impl()) {
    impl_->sem = xSemaphoreCreateRecursiveMutex();
}

RecursiveMutex::~RecursiveMutex() {
    if (impl_) {
        if (impl_->sem) {
            vSemaphoreDelete(impl_->sem);
        }
        delete impl_;
        impl_ = nullptr;
    }
}

void RecursiveMutex::lock() {
    if (impl_ && impl_->sem) {
        xSemaphoreTakeRecursive(impl_->sem, portMAX_DELAY);
    }
}

bool RecursiveMutex::try_lock(uint32_t timeout_ms) {
    if (!impl_ || !impl_->sem) return false;
    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(impl_->sem, ticks) == pdTRUE);
}

void RecursiveMutex::unlock() {
    if (impl_ && impl_->sem) {
        xSemaphoreGiveRecursive(impl_->sem);
    }
}

void* RecursiveMutex::native_handle() {
    return impl_ ? static_cast<void*>(impl_->sem) : nullptr;
}

} // namespace kube32::osal

#else // Host POSIX implementation

#include <mutex>
#include <chrono>

namespace kube32::osal {

struct Mutex::Impl {
    std::timed_mutex mtx;
};

Mutex::Mutex() : impl_(new Impl()) {}

Mutex::~Mutex() {
    delete impl_;
    impl_ = nullptr;
}

void Mutex::lock() {
    if (impl_) impl_->mtx.lock();
}

bool Mutex::try_lock(uint32_t timeout_ms) {
    if (!impl_) return false;
    if (timeout_ms == 0) {
        return impl_->mtx.try_lock();
    }
    return impl_->mtx.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void Mutex::unlock() {
    if (impl_) impl_->mtx.unlock();
}

void* Mutex::native_handle() {
    return impl_ ? &impl_->mtx : nullptr;
}

// -----------------------------------------------------------------------------
// RecursiveMutex
// -----------------------------------------------------------------------------
struct RecursiveMutex::Impl {
    std::recursive_timed_mutex mtx;
};

RecursiveMutex::RecursiveMutex() : impl_(new Impl()) {}

RecursiveMutex::~RecursiveMutex() {
    delete impl_;
    impl_ = nullptr;
}

void RecursiveMutex::lock() {
    if (impl_) impl_->mtx.lock();
}

bool RecursiveMutex::try_lock(uint32_t timeout_ms) {
    if (!impl_) return false;
    if (timeout_ms == 0) {
        return impl_->mtx.try_lock();
    }
    return impl_->mtx.try_lock_for(std::chrono::milliseconds(timeout_ms));
}

void RecursiveMutex::unlock() {
    if (impl_) impl_->mtx.unlock();
}

void* RecursiveMutex::native_handle() {
    return impl_ ? &impl_->mtx : nullptr;
}

} // namespace kube32::osal

#endif
