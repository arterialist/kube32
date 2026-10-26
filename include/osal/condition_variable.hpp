#pragma once

#include "osal/mutex.hpp"
#include "osal/thread.hpp"

namespace kube32::osal {

class ConditionVariable {
public:
    ConditionVariable();
    ~ConditionVariable();

    ConditionVariable(const ConditionVariable&) = delete;
    ConditionVariable& operator=(const ConditionVariable&) = delete;

    void notify_one();
    void notify_all();

    void wait(UniqueLock<Mutex>& lock);

    template <typename Predicate>
    void wait(UniqueLock<Mutex>& lock, Predicate pred) {
        while (!pred()) {
            wait(lock);
        }
    }

    // Returns false if timeout expired before notification, true otherwise
    bool wait_for(UniqueLock<Mutex>& lock, uint32_t timeout_ms);

    template <typename Predicate>
    bool wait_for(UniqueLock<Mutex>& lock, uint32_t timeout_ms, Predicate pred) {
        uint64_t start_ms = Thread::get_time_ms();
        while (!pred()) {
            uint64_t elapsed_ms = Thread::get_time_ms() - start_ms;
            if (elapsed_ms >= timeout_ms) {
                return pred();
            }
            if (!wait_for(lock, static_cast<uint32_t>(timeout_ms - elapsed_ms))) {
                return pred();
            }
        }
        return true;
    }

private:
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace kube32::osal
