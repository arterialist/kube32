#include "net/mdns_discovery.hpp"
#include "osal/socket.hpp"
#include "osal/thread.hpp"
#include "osal/system.hpp"

#include <iostream>
#include <sstream>
#include <cstring>
#include <atomic>
#include <chrono>

#if defined(KUBE32_TARGET_ESP32) && __has_include(<mdns.h>)
#include "mdns.h"
#include "esp_log.h"
#define KUBE32_HAS_ESP_MDNS 1
static const char* TAG = "Kube32Mdns";
#endif

namespace kube32::net {

#if defined(KUBE32_TARGET_ESP32)

class EspMdnsDiscovery : public MdnsDiscovery {
public:
    EspMdnsDiscovery() {
        mdns_init();
    }

    ~EspMdnsDiscovery() override {
        stop_service_announcement();
        mdns_free();
    }

    bool start_service_announcement(const std::string& service_type,
                                    uint16_t port,
                                    const std::map<std::string, std::string>& txt_records) override {
        mdns_hostname_set("kube32-master");
        mdns_instance_name_set("kube32-control-plane");

        std::vector<mdns_txt_item_t> items;
        for (const auto& kv : txt_records) {
            mdns_txt_item_t it;
            it.key = kv.first.c_str();
            it.value = kv.second.c_str();
            items.push_back(it);
        }

        // Parse service and proto (e.g. "_kube32-api._tcp")
        std::string service = "_kube32-api";
        std::string proto = "_tcp";
        size_t dot = service_type.find('.');
        if (dot != std::string::npos) {
            service = service_type.substr(0, dot);
            proto = service_type.substr(dot + 1);
        }

        esp_err_t err = mdns_service_add(nullptr, service.c_str(), proto.c_str(), port, items.data(), items.size());
        return (err == ESP_OK);
    }

    void stop_service_announcement() override {
        mdns_service_remove("_kube32-api", "_tcp");
    }

    bool discover_service(const std::string& service_type,
                          uint32_t timeout_ms,
                          ServiceEndpoint& out_endpoint) override {
        std::string service = "_kube32-api";
        std::string proto = "_tcp";
        size_t dot = service_type.find('.');
        if (dot != std::string::npos) {
            service = service_type.substr(0, dot);
            proto = service_type.substr(dot + 1);
        }

        mdns_result_t* results = nullptr;
        esp_err_t err = mdns_query_ptr(service.c_str(), proto.c_str(), timeout_ms, 1, &results);
        if (err != ESP_OK || !results) {
            return false;
        }

        out_endpoint.service_name = service_type;
        out_endpoint.port = results->port;

        if (results->addr) {
            char ip_str[32];
            if (results->addr->addr.type == ESP_IPADDR_TYPE_V4) {
                esp_ip4addr_ntoa(&results->addr->addr.u_addr.ip4, ip_str, sizeof(ip_str));
                out_endpoint.host_ip = ip_str;
            }
        }

        for (size_t i = 0; i < results->txt_count; ++i) {
            out_endpoint.txt_records[results->txt[i].key] = results->txt[i].value;
        }

        mdns_query_results_free(results);
        return true;
    }
};

#else // Host Simulation (POSIX)

static constexpr uint16_t MDNS_SIM_PORT = 5354;

class PosixMdnsDiscovery : public MdnsDiscovery {
public:
    PosixMdnsDiscovery() = default;
    ~PosixMdnsDiscovery() override {
        stop_service_announcement();
    }

    bool start_service_announcement(const std::string& service_type,
                                    uint16_t port,
                                    const std::map<std::string, std::string>& txt_records) override {
        stop_service_announcement();

        service_type_ = service_type;
        port_ = port;
        txt_records_ = txt_records;

        if (!sock_.create(osal::SocketType::Udp)) {
            return false;
        }
        sock_.set_reuse_addr(true);
        if (!sock_.bind("0.0.0.0", MDNS_SIM_PORT)) {
            sock_.close();
            return false;
        }
        sock_.set_timeout(200, 200);

        running_.store(true);
        thread_ = std::make_unique<osal::Thread>();
        osal::Thread::Config cfg;
        cfg.name = "mdns_announce";
        return thread_->start(announcer_worker, this, cfg);
    }

    void stop_service_announcement() override {
        running_.store(false);
        sock_.close();
        if (thread_ && thread_->is_joinable()) {
            thread_->join();
            thread_.reset();
        }
    }

    bool discover_service(const std::string& service_type,
                          uint32_t timeout_ms,
                          ServiceEndpoint& out_endpoint) override {
        osal::Socket client_sock;
        if (!client_sock.create(osal::SocketType::Udp)) {
            return false;
        }
        client_sock.set_reuse_addr(true);
        client_sock.bind("0.0.0.0", 0); // Ephemeral
        client_sock.set_timeout(200, 200);

        std::string query = "K32_MDNS_QUERY:" + service_type;

        auto start = std::chrono::steady_clock::now();
        uint8_t rx_buf[1024];

        while (true) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
            if (elapsed >= timeout_ms) break;

            // Broadcast query to local fallback port and multicast
            client_sock.sendto(query.data(), query.size(), "127.0.0.1", MDNS_SIM_PORT);
            client_sock.sendto(query.data(), query.size(), "224.0.0.251", 5353);

            std::string sender_ip;
            uint16_t sender_port = 0;
            ssize_t n = client_sock.recvfrom(rx_buf, sizeof(rx_buf) - 1, &sender_ip, &sender_port);
            if (n > 0) {
                rx_buf[n] = '\0';
                std::string msg(reinterpret_cast<char*>(rx_buf), n);
                if (msg.rfind("K32_MDNS_RESP:", 0) == 0) {
                    if (parse_response(msg, service_type, out_endpoint)) {
                        if (out_endpoint.host_ip.empty() || out_endpoint.host_ip == "0.0.0.0") {
                            out_endpoint.host_ip = "127.0.0.1";
                        }
                        return true;
                    }
                }
            }
            osal::Thread::sleep_ms(50);
        }

        return false;
    }

private:
    static void announcer_worker(void* arg) {
        static_cast<PosixMdnsDiscovery*>(arg)->run_announcer();
    }

    void run_announcer() {
        uint8_t rx_buf[512];
        std::string sender_ip;
        uint16_t sender_port = 0;

        std::string resp_msg = build_response_msg();

        while (running_.load()) {
            ssize_t n = sock_.recvfrom(rx_buf, sizeof(rx_buf) - 1, &sender_ip, &sender_port);
            if (n > 0) {
                rx_buf[n] = '\0';
                std::string req(reinterpret_cast<char*>(rx_buf), n);
                if (req.rfind("K32_MDNS_QUERY:", 0) == 0) {
                    std::string req_srv = req.substr(15);
                    if (req_srv.empty() || req_srv == service_type_) {
                        sock_.sendto(resp_msg.data(), resp_msg.size(), sender_ip.c_str(), sender_port);
                    }
                }
            }
            osal::Thread::sleep_ms(50);
        }
    }

    std::string build_response_msg() const {
        std::ostringstream oss;
        oss << "K32_MDNS_RESP:" << service_type_ << "|" << port_ << "|127.0.0.1|";
        bool first = true;
        for (const auto& kv : txt_records_) {
            if (!first) oss << ";";
            oss << kv.first << "=" << kv.second;
            first = false;
        }
        return oss.str();
    }

    static bool parse_response(const std::string& msg, const std::string& expected_service, ServiceEndpoint& out) {
        // Format: K32_MDNS_RESP:<service>|<port>|<ip>|<txt1=v1;txt2=v2>
        std::string body = msg.substr(14);
        std::vector<std::string> parts;
        std::stringstream ss(body);
        std::string part;
        while (std::getline(ss, part, '|')) {
            parts.push_back(part);
        }
        if (parts.size() < 3) return false;
        if (parts[0] != expected_service) return false;

        out.service_name = parts[0];
        out.port = static_cast<uint16_t>(std::stoi(parts[1]));
        out.host_ip = parts[2];
        out.txt_records.clear();

        if (parts.size() >= 4) {
            std::stringstream txt_ss(parts[3]);
            std::string item;
            while (std::getline(txt_ss, item, ';')) {
                size_t eq = item.find('=');
                if (eq != std::string::npos) {
                    out.txt_records[item.substr(0, eq)] = item.substr(eq + 1);
                }
            }
        }
        return true;
    }

    std::string service_type_;
    uint16_t port_{0};
    std::map<std::string, std::string> txt_records_;
    osal::Socket sock_;
    std::unique_ptr<osal::Thread> thread_;
    std::atomic<bool> running_{false};
};

#endif

std::unique_ptr<MdnsDiscovery> create_mdns_discovery() {
#if defined(KUBE32_TARGET_ESP32)
    return std::make_unique<EspMdnsDiscovery>();
#else
    return std::make_unique<PosixMdnsDiscovery>();
#endif
}

} // namespace kube32::net
