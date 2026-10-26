#include "osal/condition_variable.hpp"

#if defined(ESP_PLATFORM)

#include <pthread.h>
#include <sys/time.h>
#include <cerrno>

namespace kube32::osal {

struct ConditionVariable::Impl {
    pthread_cond_t cond;
    pthread_mutex_t internal_mtx;

    Impl() {
        pthread_cond_init(&cond, nullptr);
        pthread_mutex_init(&internal_mtx, nullptr);
    }
    ~Impl() {
        pthread_cond_destroy(&cond);
        pthread_mutex_destroy(&internal_mtx);
    }
};

ConditionVariable::ConditionVariable() : impl_(new Impl()) {}
ConditionVariable::~ConditionVariable() {
    delete impl_;
    impl_ = nullptr;
}

void ConditionVariable::notify_one() {
    if (impl_) {
        pthread_cond_signal(&impl_->cond);
    }
}

void ConditionVariable::notify_all() {
    if (impl_) {
        pthread_cond_broadcast(&impl_->cond);
    }
}

void ConditionVariable::wait(UniqueLock<Mutex>& lock) {
    if (!impl_) return;
    pthread_mutex_lock(&impl_->internal_mtx);
    lock.unlock();
    pthread_cond_wait(&impl_->cond, &impl_->internal_mtx);
    pthread_mutex_unlock(&impl_->internal_mtx);
    lock.lock();
}

bool ConditionVariable::wait_for(UniqueLock<Mutex>& lock, uint32_t timeout_ms) {
    if (!impl_) return false;
    struct timeval now;
    gettimeofday(&now, nullptr);

    struct timespec ts;
    uint64_t nsec = static_cast<uint64_t>(now.tv_usec) * 1000ULL + static_cast<uint64_t>(timeout_ms % 1000) * 1000000ULL;
    ts.tv_sec = now.tv_sec + (timeout_ms / 1000) + (nsec / 1000000000ULL);
    ts.tv_nsec = nsec % 1000000000ULL;

    pthread_mutex_lock(&impl_->internal_mtx);
    lock.unlock();
    int rc = pthread_cond_timedwait(&impl_->cond, &impl_->internal_mtx, &ts);
    pthread_mutex_unlock(&impl_->internal_mtx);
    lock.lock();

    return (rc == 0);
}

} // namespace kube32::osal

#else // Host POSIX implementation

#include <condition_variable>
#include <chrono>

namespace kube32::osal {

struct ConditionVariable::Impl {
    std::condition_variable_any cv;
};

ConditionVariable::ConditionVariable() : impl_(new Impl()) {}
ConditionVariable::~ConditionVariable() {
    delete impl_;
    impl_ = nullptr;
}

void ConditionVariable::notify_one() {
    if (impl_) impl_->cv.notify_one();
}

void ConditionVariable::notify_all() {
    if (impl_) impl_->cv.notify_all();
}

void ConditionVariable::wait(UniqueLock<Mutex>& lock) {
    if (impl_) impl_->cv.wait(lock);
}

bool ConditionVariable::wait_for(UniqueLock<Mutex>& lock, uint32_t timeout_ms) {
    if (!impl_) return false;
    auto status = impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms));
    return (status == std::cv_status::no_timeout);
}

} // namespace kube32::osal

#endif
