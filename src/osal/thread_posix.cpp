#include "osal/thread.hpp"

#if defined(ESP_PLATFORM)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include <esp_rom_sys.h>

namespace kube32::osal {

struct Thread::Impl {
    TaskHandle_t handle{nullptr};
    SemaphoreHandle_t join_sem{nullptr};
    bool is_valid{false};
    bool joinable{false};
    EntryFn entry{nullptr};
    void* arg{nullptr};
    Config config;

    static void freertos_trampoline(void* user_ptr) {
        auto* impl = static_cast<Impl*>(user_ptr);
        if (impl && impl->entry) {
            impl->entry(impl->arg);
        }

        if (impl && impl->join_sem) {
            xSemaphoreGive(impl->join_sem);
        }

        if (impl) {
            impl->is_valid = false;
        }
        vTaskDelete(nullptr);
    }
};

Thread::Thread() : impl_(new Impl()) {}

Thread::~Thread() {
    if (impl_) {
        if (impl_->join_sem) {
            vSemaphoreDelete(impl_->join_sem);
        }
        delete impl_;
        impl_ = nullptr;
    }
}

Thread::Thread(Thread&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            if (impl_->join_sem) vSemaphoreDelete(impl_->join_sem);
            delete impl_;
        }
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool Thread::start(EntryFn entry, void* arg, const Config& config) {
    if (!impl_ || impl_->is_valid || !entry) return false;
    impl_->entry = entry;
    impl_->arg = arg;
    impl_->config = config;

    if (!impl_->join_sem) {
        impl_->join_sem = xSemaphoreCreateBinary();
    }

    BaseType_t core = (config.core_affinity < 0) ? tskNO_AFFINITY : config.core_affinity;
    BaseType_t rc = xTaskCreatePinnedToCore(
        Impl::freertos_trampoline,
        config.name ? config.name : "k32_task",
        static_cast<uint32_t>(config.stack_size),
        impl_,
        config.priority,
        &impl_->handle,
        core
    );

    if (rc == pdPASS) {
        impl_->is_valid = true;
        impl_->joinable = true;
        return true;
    }
    return false;
}

bool Thread::join() {
    if (!impl_ || !impl_->is_valid || !impl_->joinable || !impl_->join_sem) return false;
    BaseType_t rc = xSemaphoreTake(impl_->join_sem, portMAX_DELAY);
    if (rc == pdTRUE) {
        impl_->joinable = false;
        impl_->is_valid = false;
        return true;
    }
    return false;
}

void Thread::detach() {
    if (impl_) {
        impl_->joinable = false;
    }
}

bool Thread::is_joinable() const {
    return impl_ && impl_->is_valid && impl_->joinable;
}

bool Thread::create(EntryFn entry, void* arg, const Config& config) {
    BaseType_t core = (config.core_affinity < 0) ? tskNO_AFFINITY : config.core_affinity;
    struct TempContext {
        EntryFn fn;
        void* user_arg;
        static void runner(void* p) {
            auto* c = static_cast<TempContext*>(p);
            if (c && c->fn) {
                c->fn(c->user_arg);
            }
            delete c;
            vTaskDelete(nullptr);
        }
    };
    auto* ctx = new TempContext{entry, arg};
    BaseType_t rc = xTaskCreatePinnedToCore(
        TempContext::runner,
        config.name ? config.name : "k32_task",
        static_cast<uint32_t>(config.stack_size),
        ctx,
        config.priority,
        nullptr,
        core
    );
    if (rc != pdPASS) {
        delete ctx;
        return false;
    }
    return true;
}

void Thread::sleep_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void Thread::sleep_us(uint64_t us) {
    esp_rom_delay_us(static_cast<uint32_t>(us));
}

void Thread::yield() {
    taskYIELD();
    esp_task_wdt_reset();
}

uint64_t Thread::get_time_ms() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

uint64_t Thread::get_time_us() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

const char* Thread::get_current_name() {
    return pcTaskGetName(nullptr);
}

int Thread::get_current_core() {
    return xPortGetCoreID();
}

} // namespace kube32::osal

#else // Host POSIX implementation

#include <pthread.h>
#include <chrono>
#include <thread>
#include <cstring>
#include <unistd.h>

namespace kube32::osal {

struct Thread::Impl {
    pthread_t handle{0};
    bool is_valid{false};
    bool joinable{false};
    EntryFn entry{nullptr};
    void* arg{nullptr};
    Config config;

    static void* posix_trampoline(void* user_ptr) {
        auto* impl = static_cast<Impl*>(user_ptr);
        if (impl && impl->config.name) {
#if defined(__APPLE__)
            pthread_setname_np(impl->config.name);
#elif defined(__linux__)
            pthread_setname_np(pthread_self(), impl->config.name);
#endif
        }
        if (impl && impl->entry) {
            impl->entry(impl->arg);
        }
        return nullptr;
    }
};

Thread::Thread() : impl_(new Impl()) {}

Thread::~Thread() {
    if (impl_) {
        if (impl_->joinable && impl_->is_valid) {
            pthread_detach(impl_->handle);
        }
        delete impl_;
        impl_ = nullptr;
    }
}

Thread::Thread(Thread&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

Thread& Thread::operator=(Thread&& other) noexcept {
    if (this != &other) {
        if (impl_) {
            if (impl_->joinable && impl_->is_valid) {
                pthread_detach(impl_->handle);
            }
            delete impl_;
        }
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

bool Thread::start(EntryFn entry, void* arg, const Config& config) {
    if (!impl_ || impl_->is_valid || !entry) return false;
    impl_->entry = entry;
    impl_->arg = arg;
    impl_->config = config;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (config.stack_size > 0) {
        pthread_attr_setstacksize(&attr, config.stack_size);
    }

    int rc = pthread_create(&impl_->handle, &attr, Impl::posix_trampoline, impl_);
    pthread_attr_destroy(&attr);

    if (rc == 0) {
        impl_->is_valid = true;
        impl_->joinable = true;
        return true;
    }
    return false;
}

bool Thread::join() {
    if (!impl_ || !impl_->is_valid || !impl_->joinable) return false;
    int rc = pthread_join(impl_->handle, nullptr);
    if (rc == 0) {
        impl_->joinable = false;
        impl_->is_valid = false;
        return true;
    }
    return false;
}

void Thread::detach() {
    if (impl_ && impl_->is_valid && impl_->joinable) {
        pthread_detach(impl_->handle);
        impl_->joinable = false;
    }
}

bool Thread::is_joinable() const {
    return impl_ && impl_->is_valid && impl_->joinable;
}

bool Thread::create(EntryFn entry, void* arg, const Config& config) {
    Thread t;
    if (t.start(entry, arg, config)) {
        t.detach();
        return true;
    }
    return false;
}

void Thread::sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void Thread::sleep_us(uint64_t us) {
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void Thread::yield() {
    std::this_thread::yield();
}

uint64_t Thread::get_time_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

uint64_t Thread::get_time_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

const char* Thread::get_current_name() {
    static thread_local char name_buf[32] = {0};
#if defined(__APPLE__)
    pthread_getname_np(pthread_self(), name_buf, sizeof(name_buf));
#elif defined(__linux__)
    pthread_getname_np(pthread_self(), name_buf, sizeof(name_buf));
#endif
    return name_buf;
}

int Thread::get_current_core() {
    return 0; // Host simulation runs in OS scheduler pool
}

} // namespace kube32::osal

#endif
