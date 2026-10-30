#pragma once

// A local HTTPS host for the https tests: TLS with the certificate of `tls`,
// or (null) a host that answers whatever arrives with plain HTTP. It records
// the requests it serves. Paths:
//   /hello    "hello over tls"
//   /echo     the request body
//   /large    20000 bytes
//   /chunked  "hello" in two chunks, 201
//   /down     503 "down"
//   /bighead  "ok" after a 3 KB response head
//   /slow     "ok" after 1.5 s
//   /hook     the next status queued in `hook_status` (200 when none) and a
//             body of `hook_body_bytes`

#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "osal/tls.hpp"

#include <atomic>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace k32test {

struct HttpsHost {
    kube32::osal::TlsServer* tls{nullptr};
    kube32::osal::Socket listener;
    uint16_t port{0};
    std::atomic<bool> running{true};
    std::atomic<int> requests{0};
    std::mutex mu;
    std::string last_head;
    std::vector<std::string> hook_bodies;   // bodies POSTed to /hook, in order
    std::deque<int> hook_status;
    std::atomic<size_t> hook_body_bytes{0};   // size of /hook's answer body
    std::thread th;

    bool start(uint16_t base) {
        if (!listener.create(kube32::osal::SocketType::Tcp)) return false;
        listener.set_reuse_addr(true);
        for (uint16_t p = base; p < base + 40; ++p) {
            if (listener.bind("127.0.0.1", p)) { port = p; break; }
        }
        if (!port || !listener.listen(8)) return false;
        th = std::thread([this]() { run(); });
        return true;
    }
    void stop() {
        running = false;
        listener.shutdown(kube32::osal::ShutdownMode::Both);
        listener.close();
        if (th.joinable()) th.join();
    }
    void run() {
        while (running) {
            kube32::osal::Socket c = listener.accept();
            if (!c.is_valid()) continue;
            std::thread([this](kube32::osal::Socket s) { serve(std::move(s)); }, std::move(c)).detach();
        }
    }
    void serve(kube32::osal::Socket s) {
        s.set_timeout(5000, 5000);
        char buf[4096];
        if (!tls) {
            s.recv(buf, sizeof buf);   // a ClientHello
            const std::string out = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            s.send_all(out.data(), out.size());
            return;
        }
        if (!s.tls_accept(*tls)) return;
        std::string in;
        size_t he;
        while ((he = in.find("\r\n\r\n")) == std::string::npos) {
            ssize_t n = s.recv(buf, sizeof buf);
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
        }
        const std::string head = in.substr(0, he);
        size_t cl_at = head.find("Content-Length: ");
        size_t cl = cl_at == std::string::npos ? 0 : std::strtoul(head.c_str() + cl_at + 16, nullptr, 10);
        while (in.size() < he + 4 + cl) {
            ssize_t n = s.recv(buf, sizeof buf);
            if (n <= 0) return;
            in.append(buf, static_cast<size_t>(n));
        }
        const std::string body = in.substr(he + 4, cl);
        std::string path = head.substr(head.find(' ') + 1);
        path = path.substr(0, path.find(' '));
        int hook = 200;
        {
            std::lock_guard<std::mutex> l(mu);
            last_head = head;
            if (path.rfind("/hook", 0) == 0) {
                hook_bodies.push_back(body);
                if (!hook_status.empty()) { hook = hook_status.front(); hook_status.pop_front(); }
            }
        }
        requests++;
        std::string out;
        if (path == "/hello") out = "HTTP/1.1 200 OK\r\nContent-Length: 14\r\nConnection: close\r\n\r\nhello over tls";
        else if (path == "/echo") out = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        else if (path == "/large") out = "HTTP/1.1 200 OK\r\nContent-Length: 20000\r\nConnection: close\r\n\r\n" + std::string(20000, 'x');
        else if (path == "/chunked") out = "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n3\r\nhel\r\n2\r\nlo\r\n0\r\n\r\n";
        else if (path == "/down") out = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 4\r\nConnection: close\r\n\r\ndown";
        else if (path == "/bighead") out = "HTTP/1.1 200 OK\r\nSet-Cookie: c=" + std::string(3000, 'c') + "\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        else if (path == "/slow") { kube32::osal::Thread::sleep_ms(1500); out = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"; }
        else if (path.rfind("/hook", 0) == 0) {
            const size_t n = hook_body_bytes;
            out = "HTTP/1.1 " + std::to_string(hook) + " X\r\nContent-Length: " + std::to_string(n) + "\r\nConnection: close\r\n\r\n" + std::string(n, '<');
        }
        else out = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        s.send_all(out.data(), out.size());
    }
    std::string head() { std::lock_guard<std::mutex> l(mu); return last_head; }
    std::vector<std::string> hooks() { std::lock_guard<std::mutex> l(mu); return hook_bodies; }
};

} // namespace k32test
