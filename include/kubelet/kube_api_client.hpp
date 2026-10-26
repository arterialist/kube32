#pragma once

#include "models/kube_models.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>

namespace kube32::kubelet {

class KubeApiClient {
public:
    virtual ~KubeApiClient() = default;

    virtual bool register_node(const std::string& node_json, bool& out_conflict) = 0;
    virtual bool patch_node_status(const std::string& node_name, const std::string& patch_json) = 0;
    virtual bool list_pods(std::vector<models::KubePod>& out_pods) = 0;
    virtual bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) = 0;
    virtual bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) = 0;
};

// Standard HTTP/1.1 REST Client implementation for Kubelet
class HttpKubeClient : public KubeApiClient {
public:
    HttpKubeClient(std::string host, uint16_t port);
    ~HttpKubeClient() override = default;

    bool register_node(const std::string& node_json, bool& out_conflict) override;
    bool patch_node_status(const std::string& node_name, const std::string& patch_json) override;
    bool list_pods(std::vector<models::KubePod>& out_pods) override;
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) override;
    bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) override;

    // Helper functions for HTTP response chunk decoding
    static bool is_chunked_transfer_encoding(std::string_view headers);
    static std::string decode_chunked_body(std::string_view raw_body);

private:
    struct HttpResponse {
        int status_code{0};
        std::string body;
    };

    HttpResponse send_http_request(const char* method, const std::string& path, const std::string& body);

    std::string host_;
    uint16_t port_;
};

} // namespace kube32::kubelet
