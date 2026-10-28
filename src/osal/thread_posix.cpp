#include "osal/thread.hpp"

#if defined(ESP_PLATFORM)

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>
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
    bool caps_task{false};   // created with xTaskCreatePinnedToCoreWithCaps (PSRAM stack)
    StaticTask_t* static_tcb{nullptr};   // task control block for a caller-provided stack
    EntryFn entry{nullptr};
    void* arg{nullptr};
    Config config;

    static void freertos_trampoline(void* user_ptr) {
        auto* impl = static_cast<Impl*>(user_ptr);
        bool caps = false;
        if (impl && impl->entry) {
            impl->entry(impl->arg);
        }

        if (impl && impl->join_sem) {
            xSemaphoreGive(impl->join_sem);
        }

        if (impl) {
            caps = impl->caps_task;
            impl->is_valid = false;
        }
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
        if (caps) {
            vTaskDeleteWithCaps(nullptr);   // frees the external stack through the timer task
        }
#endif
        vTaskDelete(nullptr);
    }
};

Thread::Thread() : impl_(new Impl()) {}

Thread::~Thread() {
    if (impl_) {
        if (impl_->join_sem) {
            vSemaphoreDelete(impl_->join_sem);
        }
        if (impl_->static_tcb) {
            // vTaskDelete(NULL) in the trampoline runs after join_sem is given;
            // give the kernel a tick to finish removing the task before the
            // control block memory is reused.
            vTaskDelay(pdMS_TO_TICKS(20));
            heap_caps_free(impl_->static_tcb);
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
    BaseType_t rc = pdFAIL;
    if (config.stack_buffer) {
        // Caller-owned stack (reserved at boot): a static task. The control
        // block is kept until this Thread is destroyed, which happens after
        // join(), so the kernel never sees it go away underneath a live task.
        if (!impl_->static_tcb) impl_->static_tcb = static_cast<StaticTask_t*>(heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (impl_->static_tcb) {
            impl_->handle = xTaskCreateStaticPinnedToCore(
                Impl::freertos_trampoline,
                config.name ? config.name : "k32_task",
                static_cast<uint32_t>(config.stack_size),
                impl_,
                config.priority,
                static_cast<StackType_t*>(config.stack_buffer),
                impl_->static_tcb,
                core
            );
            rc = impl_->handle ? pdPASS : pdFAIL;
        }
        if (rc == pdPASS) {
            impl_->is_valid = true;
            impl_->joinable = true;
            return true;
        }
        return false;
    }
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    if (config.stack_in_psram && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > config.stack_size + 4096) {
        // Big worker stacks live in PSRAM so internal SRAM stays for Wi-Fi, lwIP and DMA.
        rc = xTaskCreatePinnedToCoreWithCaps(
            Impl::freertos_trampoline,
            config.name ? config.name : "k32_task",
            static_cast<uint32_t>(config.stack_size),
            impl_,
            config.priority,
            &impl_->handle,
            core,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        impl_->caps_task = (rc == pdPASS);
    }
#endif
    if (rc != pdPASS) {
        rc = xTaskCreatePinnedToCore(
            Impl::freertos_trampoline,
            config.name ? config.name : "k32_task",
            static_cast<uint32_t>(config.stack_size),
            impl_,
            config.priority,
            &impl_->handle,
            core
        );
    }

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
    BaseType_t rc = pdFAIL;
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
    if (config.stack_in_psram && heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > config.stack_size + 4096) {
        // Detached tasks never return (vTaskDelete in the runner), so the
        // externally allocated stack is never reclaimed; that is fine for the
        // long-lived service threads this is used for.
        rc = xTaskCreatePinnedToCoreWithCaps(
            TempContext::runner,
            config.name ? config.name : "k32_task",
            static_cast<uint32_t>(config.stack_size),
            ctx,
            config.priority,
            nullptr,
            core,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
    }
#endif
    if (rc != pdPASS) {
        rc = xTaskCreatePinnedToCore(
            TempContext::runner,
            config.name ? config.name : "k32_task",
            static_cast<uint32_t>(config.stack_size),
            ctx,
            config.priority,
            nullptr,
            core
        );
    }
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
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
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
#include <algorithm>

namespace kube32::osal {

struct Thread::Impl {
    pthread_t handle{0};
    bool is_valid{false};
    bool joinable{false};
    EntryFn entry{nullptr};
    void* arg{nullptr};
    Config config;
    char name_buf[32] = {0};

    static void* posix_trampoline(void* user_ptr) {
        auto* impl = static_cast<Impl*>(user_ptr);
        if (impl && impl->name_buf[0] != '\0') {
#if defined(__APPLE__)
            pthread_setname_np(impl->name_buf);
#elif defined(__linux__)
            pthread_setname_np(pthread_self(), impl->name_buf);
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
    std::memset(impl_->name_buf, 0, sizeof(impl_->name_buf));
    if (config.name) {
        std::strncpy(impl_->name_buf, config.name, sizeof(impl_->name_buf) - 1);
        impl_->name_buf[sizeof(impl_->name_buf) - 1] = '\0';
    }

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (config.stack_size > 0) {
        // POSIX host simulation requires >= 64 KB (65536) to accommodate host TLS,
        // C++ dynamic initialization, and AddressSanitizer/UBSan shadow redzones.
        size_t min_stack = 65536;
#if defined(PTHREAD_STACK_MIN)
        min_stack = std::max(min_stack, static_cast<size_t>(PTHREAD_STACK_MIN));
#endif
        size_t actual_stack = std::max(config.stack_size, min_stack);
        pthread_attr_setstacksize(&attr, actual_stack);
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
