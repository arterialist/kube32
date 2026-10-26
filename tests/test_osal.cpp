#include "osal/osal.hpp"
#include <cassert>
#include <iostream>
#include <atomic>
#include <vector>
#include <cstring>

using namespace kube32::osal;

void test_thread_lifecycle() {
    std::cout << "[RUN] test_thread_lifecycle" << std::endl;
    std::atomic<int> counter{0};

    Thread t;
    bool started = t.start([](void* arg) {
        auto* cnt = static_cast<std::atomic<int>*>(arg);
        (*cnt)++;
    }, &counter);

    assert(started);
    assert(t.is_joinable());
    bool joined = t.join();
    assert(joined);
    assert(!t.is_joinable());
    assert(counter.load() == 1);
    std::cout << "[PASS] test_thread_lifecycle" << std::endl;
}

void test_thread_static_utilities() {
    std::cout << "[RUN] test_thread_static_utilities" << std::endl;
    uint64_t t1 = Thread::get_time_ms();
    Thread::sleep_ms(20);
    uint64_t t2 = Thread::get_time_ms();
    assert(t2 >= t1 + 15);

    Thread::yield(); // Ensure yield executes without crashing
    std::cout << "[PASS] test_thread_static_utilities" << std::endl;
}

void test_mutex_basic() {
    std::cout << "[RUN] test_mutex_basic" << std::endl;
    Mutex mtx;
    mtx.lock();
    mtx.unlock();

    bool locked = mtx.try_lock();
    assert(locked);
    mtx.unlock();

    // Test LockGuard
    {
        LockGuard<Mutex> guard(mtx);
    }

    // Test UniqueLock
    {
        UniqueLock<Mutex> ulock(mtx);
        assert(ulock.owns_lock());
        ulock.unlock();
        assert(!ulock.owns_lock());
        ulock.lock();
        assert(ulock.owns_lock());
    }

    std::cout << "[PASS] test_mutex_basic" << std::endl;
}

void test_recursive_mutex() {
    std::cout << "[RUN] test_recursive_mutex" << std::endl;
    RecursiveMutex rmtx;
    rmtx.lock();
    rmtx.lock(); // Nested lock
    rmtx.unlock();
    rmtx.unlock();

    assert(rmtx.try_lock(10));
    assert(rmtx.try_lock(10));
    rmtx.unlock();
    rmtx.unlock();
    std::cout << "[PASS] test_recursive_mutex" << std::endl;
}

void test_condition_variable() {
    std::cout << "[RUN] test_condition_variable" << std::endl;
    Mutex mtx;
    ConditionVariable cv;
    bool ready = false;

    struct CvContext {
        Mutex* m;
        ConditionVariable* cv;
        bool* flag;
    };
    CvContext ctx{&mtx, &cv, &ready};

    Thread t;
    t.start([](void* arg) {
        auto* c = static_cast<CvContext*>(arg);
        Thread::sleep_ms(30);
        {
            LockGuard<Mutex> lk(*c->m);
            *c->flag = true;
        }
        c->cv->notify_one();
    }, &ctx);

    UniqueLock<Mutex> lock(mtx);
    bool notified = cv.wait_for(lock, 200, [&ready] { return ready; });
    assert(notified);
    assert(ready);
    t.join();
    std::cout << "[PASS] test_condition_variable" << std::endl;
}

void test_timer_oneshot_and_periodic() {
    std::cout << "[RUN] test_timer_oneshot_and_periodic" << std::endl;
    std::atomic<int> callbacks{0};

    Timer timer;
    Timer::Config cfg;
    cfg.name = "unit_timer";
    cfg.callback = [](void* arg) {
        auto* c = static_cast<std::atomic<int>*>(arg);
        (*c)++;
    };
    cfg.arg = &callbacks;

    timer.init(cfg);
    timer.start(25, false); // 25ms one-shot
    Thread::sleep_ms(70);
    timer.stop();
    assert(callbacks.load() == 1);

    // Periodic
    callbacks.store(0);
    timer.start(20, true); // 20ms periodic
    Thread::sleep_ms(75);
    timer.stop();
    assert(callbacks.load() >= 2);
    std::cout << "[PASS] test_timer_oneshot_and_periodic" << std::endl;
}

void test_socket_loopback() {
    std::cout << "[RUN] test_socket_loopback" << std::endl;
    Socket server;
    assert(server.create(SocketType::Tcp));
    assert(server.set_reuse_addr(true));
    assert(server.bind("127.0.0.1", 16443));
    assert(server.listen(5));

    std::atomic<bool> client_done{false};

    Thread client_th;
    client_th.start([](void* arg) {
        auto* done = static_cast<std::atomic<bool>*>(arg);
        Thread::sleep_ms(20);
        Socket client;
        if (client.create(SocketType::Tcp)) {
            if (client.connect("127.0.0.1", 16443, 2000)) {
                const char msg[] = "PING";
                client.send_all(msg, 4);
                char buf[8] = {0};
                client.recv(buf, 4);
                if (std::memcmp(buf, "PONG", 4) == 0) {
                    *done = true;
                }
            }
        }
    }, &client_done);

    std::string client_ip;
    uint16_t client_port = 0;
    Socket accepted = server.accept(&client_ip, &client_port);
    assert(accepted.is_valid());

    char buf[16] = {0};
    ssize_t n = accepted.recv(buf, 4);
    assert(n == 4);
    assert(std::memcmp(buf, "PING", 4) == 0);

    accepted.send_all("PONG", 4);
    client_th.join();
    assert(client_done.load());
    std::cout << "[PASS] test_socket_loopback" << std::endl;
}

void test_system_telemetry() {
    std::cout << "[RUN] test_system_telemetry" << std::endl;
    size_t sram = System::get_free_heap_bytes();
    assert(sram > 0);
    size_t psram = System::get_free_psram_bytes();
    assert(psram > 0);
    uint32_t freq = System::get_cpu_freq_mhz();
    assert(freq > 0);
    uint64_t uptime = System::get_uptime_ms();
    assert(uptime >= 0);

    System::feed_watchdog(); // Must not crash
    std::cout << "[PASS] test_system_telemetry" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 OSAL Tests ===" << std::endl;
    test_thread_lifecycle();
    test_thread_static_utilities();
    test_mutex_basic();
    test_recursive_mutex();
    test_condition_variable();
    test_timer_oneshot_and_periodic();
    test_socket_loopback();
    test_system_telemetry();
    std::cout << "=== All OSAL Tests Passed Successfully ===" << std::endl;
    return 0;
}
