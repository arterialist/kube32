#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <memory>

namespace kube32::net {

struct ServiceEndpoint {
    std::string service_name;
    std::string host_ip;
    uint16_t port{0};
    std::map<std::string, std::string> txt_records;
};

class MdnsDiscovery {
public:
    virtual ~MdnsDiscovery() = default;

    // Control Plane / Master: Announce API Server
    virtual bool start_service_announcement(const std::string& service_type,
                                            uint16_t port,
                                            const std::map<std::string, std::string>& txt_records) = 0;
    virtual void stop_service_announcement() = 0;

    // Worker Node: Discover API Server
    virtual bool discover_service(const std::string& service_type,
                                  uint32_t timeout_ms,
                                  ServiceEndpoint& out_endpoint) = 0;
};

std::unique_ptr<MdnsDiscovery> create_mdns_discovery();

} // namespace kube32::net
