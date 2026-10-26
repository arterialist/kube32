#pragma once

#include "api/route_handler.hpp"
#include "storage/store.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "scheduler/scheduler.hpp"

#include <string>
#include <atomic>
#include <memory>

namespace kube32::api {

constexpr size_t MAX_REQUEST_BODY_SIZE = 2 * 1024 * 1024; // 2MB

struct HttpServerOptions {
    std::string host{"0.0.0.0"};
    uint16_t port{6443};
    int backlog{10};
    size_t rx_buffer_size{8192};
    size_t max_body_size{MAX_REQUEST_BODY_SIZE};
};

class HttpServer {
public:
    HttpServer(HttpServerOptions options, storage::Store& store, scheduler::Scheduler* scheduler = nullptr);
    ~HttpServer();

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    void run_worker_loop();

    RouteHandler& route_handler() { return route_handler_; }

private:
    void handle_connection(osal::Socket client_sock);
    static bool parse_request_headers(const char* buf, size_t len, HttpRequest& out_req, size_t& out_header_bytes);

    HttpServerOptions options_;
    RouteHandler route_handler_;
    osal::Socket listen_sock_;
    std::unique_ptr<osal::Thread> server_thread_;
    std::atomic<bool> running_{false};
};

} // namespace kube32::api
