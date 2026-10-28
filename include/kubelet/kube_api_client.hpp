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
    virtual bool renew_node_lease(const std::string& /*node_name*/, uint32_t /*duration_sec*/, const std::string& /*renew_time_iso8601*/) { return true; }
    virtual bool list_pods(std::vector<models::KubePod>& out_pods) = 0;
    virtual bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) = 0;
    virtual bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) = 0;
    // core/v1 Event (POST /api/v1/namespaces/{ns}/events). Default: accepted, nowhere.
    virtual bool create_event(const std::string& /*ns*/, const std::string& /*event_json*/) { return true; }
    // Ships log records (NDJSON, see runtime/pod_log.hpp) to the API server's mirror ring.
    virtual bool push_pod_logs(const std::string& /*ns*/, const std::string& /*name*/, const std::string& /*ndjson*/) { return true; }
    // Pushes a measured node/pod usage sample (see api/metrics_registry.hpp for the body).
    virtual bool push_metrics(const std::string& /*node_name*/, const std::string& /*sample_json*/) { return true; }
    // Finalizes a terminating pod (DELETE with gracePeriodSeconds=0).
    virtual bool delete_pod(const std::string& /*ns*/, const std::string& /*name*/) { return true; }
};

// Standard HTTP/1.1 REST Client implementation for Kubelet
class HttpKubeClient : public KubeApiClient {
public:
    HttpKubeClient(std::string host, uint16_t port);
    ~HttpKubeClient() override = default;

    bool register_node(const std::string& node_json, bool& out_conflict) override;
    bool patch_node_status(const std::string& node_name, const std::string& patch_json) override;
    bool renew_node_lease(const std::string& node_name, uint32_t duration_sec, const std::string& renew_time_iso8601) override;
    bool list_pods(std::vector<models::KubePod>& out_pods) override;
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) override;
    bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) override;
    bool create_event(const std::string& ns, const std::string& event_json) override;
    bool push_pod_logs(const std::string& ns, const std::string& name, const std::string& ndjson) override;
    bool push_metrics(const std::string& node_name, const std::string& sample_json) override;
    bool delete_pod(const std::string& ns, const std::string& name) override;
    // Restricts list_pods_raw to pods bound to this node (server-side fieldSelector).
    void set_node_filter(const std::string& node_name) { node_filter_ = node_name; }

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
    std::string node_filter_;
};

} // namespace kube32::kubelet
