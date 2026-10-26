#pragma once

#include <cstdint>
#include <cstddef>

namespace kube32::osal {

class Mutex {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    Mutex(Mutex&&) = delete;
    Mutex& operator=(Mutex&&) = delete;

    void lock();
    bool try_lock(uint32_t timeout_ms = 0);
    void unlock();

    // Direct access to native handle
    void* native_handle();

private:
    struct Impl;
    Impl* impl_{nullptr};
};

class RecursiveMutex {
public:
    RecursiveMutex();
    ~RecursiveMutex();

    RecursiveMutex(const RecursiveMutex&) = delete;
    RecursiveMutex& operator=(const RecursiveMutex&) = delete;
    RecursiveMutex(RecursiveMutex&&) = delete;
    RecursiveMutex& operator=(RecursiveMutex&&) = delete;

    void lock();
    bool try_lock(uint32_t timeout_ms = 0);
    void unlock();

    void* native_handle();

private:
    struct Impl;
    Impl* impl_{nullptr};
};

// RAII LockGuard Template
template <typename LockableType>
class LockGuard {
public:
    explicit LockGuard(LockableType& m) : mutex_(m) {
        mutex_.lock();
    }
    ~LockGuard() {
        mutex_.unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    LockableType& mutex_;
};

// RAII UniqueLock Template
template <typename LockableType>
class UniqueLock {
public:
    UniqueLock() : mutex_(nullptr), locked_(false) {}
    explicit UniqueLock(LockableType& m) : mutex_(&m), locked_(false) {
        lock();
    }
    ~UniqueLock() {
        if (locked_ && mutex_) {
            mutex_->unlock();
        }
    }

    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;

    UniqueLock(UniqueLock&& other) noexcept : mutex_(other.mutex_), locked_(other.locked_) {
        other.mutex_ = nullptr;
        other.locked_ = false;
    }
    UniqueLock& operator=(UniqueLock&& other) noexcept {
        if (this != &other) {
            if (locked_ && mutex_) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            locked_ = other.locked_;
            other.mutex_ = nullptr;
            other.locked_ = false;
        }
        return *this;
    }

    void lock() {
        if (mutex_ && !locked_) {
            mutex_->lock();
            locked_ = true;
        }
    }

    bool try_lock(uint32_t timeout_ms = 0) {
        if (mutex_ && !locked_) {
            locked_ = mutex_->try_lock(timeout_ms);
            return locked_;
        }
        return false;
    }

    void unlock() {
        if (mutex_ && locked_) {
            mutex_->unlock();
            locked_ = false;
        }
    }

    bool owns_lock() const { return locked_; }
    LockableType* mutex() const { return mutex_; }

private:
    LockableType* mutex_;
    bool locked_;
};

} // namespace kube32::osal
