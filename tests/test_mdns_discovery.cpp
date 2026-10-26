#include "net/mdns_discovery.hpp"
#include "osal/thread.hpp"
#include <iostream>
#include <cassert>

using namespace kube32::net;

void test_mdns_announcement_and_discovery() {
    auto server = create_mdns_discovery();
    std::map<std::string, std::string> txt = {
        {"cluster", "kube32-mesh"},
        {"role", "control-plane"},
        {"version", "v0.4.0"}
    };

    bool ok = server->start_service_announcement("_kube32-api._tcp", 6443, txt);
    assert(ok && "Failed to start mDNS announcement");

    kube32::osal::Thread::sleep_ms(100);

    auto client = create_mdns_discovery();
    ServiceEndpoint ep;
    bool discovered = client->discover_service("_kube32-api._tcp", 2000, ep);
    assert(discovered && "Failed to discover announced mDNS service");

    assert(ep.service_name == "_kube32-api._tcp");
    assert(ep.port == 6443);
    assert(!ep.host_ip.empty());
    assert(ep.txt_records["cluster"] == "kube32-mesh");
    assert(ep.txt_records["role"] == "control-plane");
    assert(ep.txt_records["version"] == "v0.4.0");

    server->stop_service_announcement();
    std::cout << "[PASS] test_mdns_announcement_and_discovery" << std::endl;
}

void test_mdns_discovery_timeout() {
    auto client = create_mdns_discovery();
    ServiceEndpoint ep;
    bool discovered = client->discover_service("_nonexistent._tcp", 300, ep);
    assert(!discovered && "Discovery of nonexistent service should fail/timeout");

    std::cout << "[PASS] test_mdns_discovery_timeout" << std::endl;
}

void test_mdns_service_filtering() {
    auto server = create_mdns_discovery();
    std::map<std::string, std::string> txt = {
        {"channel", "telemetry"}
    };

    bool ok = server->start_service_announcement("_kube32-api._tcp", 8443, txt);
    assert(ok);

    kube32::osal::Thread::sleep_ms(100);

    auto client = create_mdns_discovery();
    ServiceEndpoint ep;
    bool found_wrong = client->discover_service("_kube32-other._tcp", 300, ep);
    assert(!found_wrong && "Should not match wrong service name");

    bool found_right = client->discover_service("_kube32-api._tcp", 1000, ep);
    assert(found_right && "Should match correct service name");
    assert(ep.port == 8443);
    assert(ep.txt_records["channel"] == "telemetry");

    server->stop_service_announcement();
    std::cout << "[PASS] test_mdns_service_filtering" << std::endl;
}

int main() {
    std::cout << "=== Running Kube32 mDNS Discovery Unit Tests ===" << std::endl;
    test_mdns_announcement_and_discovery();
    test_mdns_discovery_timeout();
    test_mdns_service_filtering();
    std::cout << "All mDNS discovery tests passed!" << std::endl;
    return 0;
}
