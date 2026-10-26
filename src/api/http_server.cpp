#include "api/http_server.hpp"
#include "mem/monotonic_arena.hpp"

#include <cstring>
#include <cstdio>
#include <vector>
#include <iostream>

namespace kube32::api {

HttpServer::HttpServer(HttpServerOptions options, storage::Store& store, scheduler::Scheduler* scheduler)
    : options_(std::move(options)), route_handler_(store, scheduler) {}

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start() {
    if (running_.load()) return true;

    if (!osal::Socket::init_networking()) {
        return false;
    }

    if (!listen_sock_.create(osal::SocketType::Tcp)) {
        return false;
    }

    listen_sock_.set_reuse_addr(true);
    listen_sock_.set_tcp_nodelay(true);

    if (!listen_sock_.bind(options_.host.c_str(), options_.port)) {
        listen_sock_.close();
        return false;
    }

    if (!listen_sock_.listen(options_.backlog)) {
        listen_sock_.close();
        return false;
    }

    running_.store(true);
    server_thread_ = std::make_unique<osal::Thread>();
    osal::Thread::Config cfg;
    cfg.name = "k32_http";
    cfg.stack_size = 12288;
    bool started = server_thread_->start([](void* p) {
        auto* self = static_cast<HttpServer*>(p);
        self->run_worker_loop();
    }, this, cfg);

    if (!started) {
        running_.store(false);
        listen_sock_.close();
        return false;
    }
    return true;
}

void HttpServer::stop() {
    if (!running_.load()) return;
    running_.store(false);

    listen_sock_.shutdown(osal::ShutdownMode::Both);
    listen_sock_.close();

    if (server_thread_) {
        server_thread_->join();
        server_thread_.reset();
    }
}

void HttpServer::run_worker_loop() {
    while (running_.load()) {
        osal::Thread::yield();
        osal::Socket client = listen_sock_.accept();
        if (!client.is_valid()) {
            if (!running_.load()) break;
            continue;
        }

        client.set_tcp_nodelay(true);
        // Configure 1s receive and 5s send timeouts to mitigate Slowloris denial of service
        client.set_timeout(1000, 5000);
        handle_connection(std::move(client));
    }
}

static const char* get_status_text(int code) {
    switch (code) {
        case 200: return "OK";
        case 201: return "Created";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "Status";
    }
}

bool HttpServer::parse_request_headers(const char* buf, size_t len, HttpRequest& out_req, size_t& out_header_bytes) {
    const char* end_headers = std::strstr(buf, "\r\n\r\n");
    if (!end_headers) {
        end_headers = std::strstr(buf, "\n\n");
        if (!end_headers) return false;
        out_header_bytes = (end_headers - buf) + 2;
    } else {
        out_header_bytes = (end_headers - buf) + 4;
    }

    std::string_view header_view(buf, out_header_bytes);

    // Parse Request Line
    size_t first_line_end = header_view.find("\r\n");
    size_t line_sep_len = 2;
    if (first_line_end == std::string_view::npos) {
        first_line_end = header_view.find('\n');
        line_sep_len = 1;
    }
    if (first_line_end == std::string_view::npos) return false;

    std::string_view req_line = header_view.substr(0, first_line_end);
    size_t space1 = req_line.find(' ');
    if (space1 == std::string_view::npos) return false;
    size_t space2 = req_line.find(' ', space1 + 1);
    if (space2 == std::string_view::npos) return false;

    std::string_view method_sv = req_line.substr(0, space1);
    std::string_view uri_sv = req_line.substr(space1 + 1, space2 - space1 - 1);

    out_req.method = parse_http_method(method_sv);
    out_req.raw_uri = std::string(uri_sv);
    normalize_uri(out_req.raw_uri, out_req.path, out_req.query_string);

    // Scan Headers
    size_t cur = first_line_end + line_sep_len;
    while (cur < out_header_bytes) {
        size_t next_line = header_view.find("\r\n", cur);
        size_t sep_len = 2;
        if (next_line == std::string_view::npos) {
            next_line = header_view.find('\n', cur);
            sep_len = 1;
        }
        if (next_line == std::string_view::npos || next_line == cur) break;

        std::string_view line = header_view.substr(cur, next_line - cur);
        size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string_view key = line.substr(0, colon);
            std::string_view val = line.substr(colon + 1);
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t' || val.back() == '\r')) val.remove_suffix(1);

            if (strncasecmp(key.data(), "Accept", key.size()) == 0) {
                out_req.accept = std::string(val);
                if (out_req.accept.find("as=Table") != std::string::npos) {
                    out_req.as_table = true;
                }
            } else if (strncasecmp(key.data(), "Content-Type", key.size()) == 0) {
                out_req.content_type = std::string(val);
            } else if (strncasecmp(key.data(), "Content-Length", key.size()) == 0) {
                // Strict validation: Reject leading '-', non-digits, or empty values
                bool is_valid_number = !val.empty();
                for (char c : val) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        is_valid_number = false;
                        break;
                    }
                }
                if (!is_valid_number) {
                    out_req.invalid_content_length = true;
                    out_req.content_length = 0;
                } else {
                    try {
                        out_req.content_length = std::stoul(std::string(val));
                    } catch (...) {
                        out_req.invalid_content_length = true;
                        out_req.content_length = 0;
                    }
                }
            }
        }
        cur = next_line + sep_len;
    }

    return true;
}

void HttpServer::handle_connection(osal::Socket client_sock) {
    client_sock.set_tcp_nodelay(true);
    client_sock.set_timeout(1000, 5000);

    std::vector<char> rx_buf(options_.rx_buffer_size);
    ssize_t n = client_sock.recv(rx_buf.data(), rx_buf.size() - 1);
    if (n <= 0) {
        client_sock.close();
        return;
    }
    rx_buf[n] = '\0';

    HttpRequest req;
    size_t header_bytes = 0;
    if (!parse_request_headers(rx_buf.data(), static_cast<size_t>(n), req, header_bytes)) {
        const char* bad_req = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        client_sock.send_all(bad_req, std::strlen(bad_req));
        client_sock.close();
        return;
    }

    // 1. Check for negative or malformed Content-Length -> Return HTTP 400 Bad Request
    if (req.invalid_content_length) {
        HttpResponse resp = RouteHandler::make_status(400, "BadRequest", "Invalid Content-Length header");
        char header_buf[256];
        int hlen = snprintf(header_buf, sizeof(header_buf),
                            "HTTP/1.1 400 Bad Request\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            resp.content_type.c_str(),
                            resp.body.size());
        if (hlen > 0) client_sock.send_all(header_buf, static_cast<size_t>(hlen));
        if (!resp.body.empty()) client_sock.send_all(resp.body.data(), resp.body.size());
        client_sock.close();
        return;
    }

    // 2. Bound incoming request body size to 2MB -> Return HTTP 413 Payload Too Large
    if (req.content_length > options_.max_body_size) {
        HttpResponse resp = RouteHandler::make_status(413, "PayloadTooLarge",
            "Request body exceeds maximum allowed size of 2097152 bytes (2MB)");
        char header_buf[256];
        int hlen = snprintf(header_buf, sizeof(header_buf),
                            "HTTP/1.1 413 Payload Too Large\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            resp.content_type.c_str(),
                            resp.body.size());
        if (hlen > 0) client_sock.send_all(header_buf, static_cast<size_t>(hlen));
        if (!resp.body.empty()) client_sock.send_all(resp.body.data(), resp.body.size());
        client_sock.close();
        return;
    }

    // Read remaining body if Content-Length exceeds what was received
    size_t body_received = static_cast<size_t>(n) - header_bytes;
    std::string body_str;
    body_str.reserve(req.content_length);

    if (body_received > 0) {
        size_t take = std::min(body_received, req.content_length);
        body_str.append(rx_buf.data() + header_bytes, take);
    }

    while (body_str.size() < req.content_length) {
        char chunk[2048];
        size_t needed = std::min(sizeof(chunk), req.content_length - body_str.size());
        ssize_t r = client_sock.recv(chunk, needed);
        if (r <= 0) {
            // Client timed out (5s SO_RCVTIMEO) or closed connection prematurely
            client_sock.close();
            return;
        }
        body_str.append(chunk, static_cast<size_t>(r));
    }
    req.body = std::move(body_str);

    // Per-request 16KB Monotonic Scratchpad
    mem::MonotonicArena arena;
    HttpResponse resp = route_handler_.handle_request(req, arena, &client_sock);
    arena.reset();

    // Send HTTP Response if not chunk-streamed directly to socket
    if (!resp.is_chunked) {
        char header_buf[256];
        size_t content_len = (req.method == HttpMethod::Head) ? 0 : resp.body.size();
        int hlen = snprintf(header_buf, sizeof(header_buf),
                            "HTTP/1.1 %d %s\r\n"
                            "Content-Type: %s\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            resp.status_code,
                            get_status_text(resp.status_code),
                            resp.content_type.c_str(),
                            content_len);

        if (hlen > 0) {
            client_sock.send_all(header_buf, static_cast<size_t>(hlen));
        }
        if (req.method != HttpMethod::Head && !resp.body.empty()) {
            client_sock.send_all(resp.body.data(), resp.body.size());
        }
    }

    client_sock.close();
    osal::Thread::yield();
}

} // namespace kube32::api
