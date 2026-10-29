#pragma once

#include "models/kube_models.hpp"
#include "mem/function_ref.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>

namespace kube32::kubelet {

// What the pod sync needs of one listed pod, and the pod's JSON. The views
// point into the list as it streams in: valid during the visit only.
struct PodListEntry {
    std::string_view name;
    std::string_view namespace_name;
    std::string_view uid;
    std::string_view node_name;            // spec.nodeName
    std::string_view phase;                // status.phase
    bool deleting{false};                  // metadata.deletionTimestamp is set
    std::string_view manifest;             // the pod's JSON
    const models::KubePod* pod{nullptr};   // the pod as the client parsed it, if it did
};

class KubeApiClient {
public:
    virtual ~KubeApiClient() = default;

    virtual bool register_node(const std::string& node_json, bool& out_conflict) = 0;
    virtual bool patch_node_status(const std::string& node_name, std::string_view patch_json) = 0;
    virtual bool renew_node_lease(const std::string& /*node_name*/, uint32_t /*duration_sec*/, std::string_view /*renew_time_iso8601*/) { return true; }
    virtual bool list_pods(std::vector<models::KubePod>& out_pods) = 0;
    virtual bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) = 0;
    // The pods bound to this node, handed to `visit` one at a time. False
    // when the list could not be read whole: then no pod may be taken as
    // deleted. The default reads the list through list_pods_raw.
    virtual bool for_each_node_pod(mem::FunctionRef<void(const PodListEntry&)> visit);
    virtual bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) = 0;
    // core/v1 Event (POST /api/v1/namespaces/{ns}/events). Default: accepted, nowhere.
    virtual bool create_event(const std::string& /*ns*/, const std::string& /*event_json*/) { return true; }
    // Ships log records (NDJSON, see runtime/pod_log.hpp) to the API server's mirror ring.
    virtual bool push_pod_logs(const std::string& /*ns*/, const std::string& /*name*/, const std::string& /*ndjson*/) { return true; }
    // Pushes a measured node/pod usage sample (see api/metrics_registry.hpp for the body).
    virtual bool push_metrics(const std::string& /*node_name*/, std::string_view /*sample_json*/) { return true; }
    // Finalizes a terminating pod (DELETE with gracePeriodSeconds=0).
    virtual bool delete_pod(const std::string& /*ns*/, const std::string& /*name*/) { return true; }
    // GET of an API path (ConfigMaps, Secrets, Services, pods by label for the
    // container environment). Returns the HTTP status, 0 when unreachable.
    virtual int get(const std::string& /*path*/, std::string& out_body) { out_body.clear(); return 0; }
    // GET of a list: each element of its "items" reaches `on_item` as one
    // JSON text, one at a time. Returns the HTTP status; 0 when the server
    // could not be reached or the list arrived incomplete. The default reads
    // the list whole through get().
    virtual int list_items(const std::string& path, const std::function<void(std::string&&)>& on_item);
    // Module bytes of a pod's inline image (annotation kube32.io/wasm-base64),
    // fetched with the pod and decoded as it streams in. Pod lists leave the
    // annotation's value out (see list_pods_raw).
    virtual bool get_pod_image(const std::string& /*ns*/, const std::string& /*name*/, std::vector<uint8_t>& /*out*/, std::string& why) {
        why = "not supported by this client";
        return false;
    }
};

// Standard HTTP/1.1 REST Client implementation for Kubelet
class HttpKubeClient : public KubeApiClient {
public:
    HttpKubeClient(std::string host, uint16_t port);
    ~HttpKubeClient() override = default;

    bool register_node(const std::string& node_json, bool& out_conflict) override;
    bool patch_node_status(const std::string& node_name, std::string_view patch_json) override;
    bool renew_node_lease(const std::string& node_name, uint32_t duration_sec, std::string_view renew_time_iso8601) override;
    bool list_pods(std::vector<models::KubePod>& out_pods) override;
    bool list_pods_raw(std::vector<std::pair<models::KubePod, std::string>>& out_pods_with_manifest) override;
    // Streams the list through an item buffer on the stack and reads each
    // pod's fields in place: a pass over the node's pods takes nothing from
    // the heap.
    bool for_each_node_pod(mem::FunctionRef<void(const PodListEntry&)> visit) override;
    bool patch_pod_status(const std::string& ns, const std::string& name, const std::string& status_patch_json) override;
    bool create_event(const std::string& ns, const std::string& event_json) override;
    bool push_pod_logs(const std::string& ns, const std::string& name, const std::string& ndjson) override;
    bool push_metrics(const std::string& node_name, std::string_view sample_json) override;
    bool delete_pod(const std::string& ns, const std::string& name) override;
    int get(const std::string& path, std::string& out_body) override;
    // Splits the list into items as it streams in: the whole response or a
    // JSON tree of it is never held.
    int list_items(const std::string& path, const std::function<void(std::string&&)>& on_item) override;
    bool get_pod_image(const std::string& ns, const std::string& name, std::vector<uint8_t>& out, std::string& why) override;
    // Restricts list_pods_raw to pods bound to this node (server-side fieldSelector).
    // The list asks the server to leave inline image annotations empty
    // (?inlineImages=omit) and is split into pods as it streams in, so a
    // kubelet never holds the whole response or a JSON tree of it.
    void set_node_filter(const std::string& node_name) { node_filter_ = node_name; }
    // Sent as "Authorization: Bearer <token>" on every request when not empty.
    void set_bearer_token(const std::string& token) { token_ = token; }
    // Largest response body get() and the status calls keep (0: no limit); a
    // larger one fails (status 0) instead of exhausting a small heap.
    void set_max_body(size_t bytes) { max_body_ = bytes; }


    // Helper functions for HTTP response chunk decoding
    static bool is_chunked_transfer_encoding(std::string_view headers);
    static std::string decode_chunked_body(std::string_view raw_body);

private:
    // The start of a response body, for messages
    struct BodyStart {
        char text[160];
        size_t len{0};
    };

    // One request whose answer is not used: returns the status, and the start
    // of the body in `start` when given. Nothing is taken from the heap.
    int exchange(const char* method, std::string_view path, std::string_view body, BodyStart* start = nullptr);
    // Streams one exchange: the body reaches `on_data` as it arrives (false
    // from it ends the exchange). Returns the status, 0 when the server could
    // not be reached or the response was malformed.
    int stream_request(const char* method, std::string_view path, std::string_view body,
                       mem::FunctionRef<bool(const char*, size_t)> on_data, long* out_content_length = nullptr);

    std::string host_;
    uint16_t port_;
    std::string node_filter_;
    std::string token_;
    size_t max_body_{0};
};

} // namespace kube32::kubelet
