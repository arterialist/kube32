#pragma once

#include "api/chunked_stream_writer.hpp"
#include "models/kube_models.hpp"

#include <string>
#include <vector>
#include <cstdint>

namespace kube32::api {

enum class ResourceKind {
    Node,
    Pod,
    Deployment
};

class TableSerializer {
public:
    // Direct from C++ Model Objects
    static bool serialize_nodes_table(ChunkedStreamWriter& writer,
                                      const std::vector<models::KubeNode>& nodes,
                                      uint64_t resource_version = 100);

    static bool serialize_pods_table(ChunkedStreamWriter& writer,
                                     const std::vector<models::KubePod>& pods,
                                     uint64_t resource_version = 100);

    static bool serialize_deployments_table(ChunkedStreamWriter& writer,
                                            const std::vector<models::KubeDeployment>& deployments,
                                            uint64_t resource_version = 100);

    // Direct from stored JSON manifests
    static bool serialize_nodes_table_from_json(ChunkedStreamWriter& writer,
                                                const std::vector<std::string>& node_jsons,
                                                uint64_t resource_version = 100);

    static bool serialize_pods_table_from_json(ChunkedStreamWriter& writer,
                                               const std::vector<std::string>& pod_jsons,
                                               uint64_t resource_version = 100);

    static bool serialize_deployments_table_from_json(ChunkedStreamWriter& writer,
                                                      const std::vector<std::string>& deployment_jsons,
                                                      uint64_t resource_version = 100);

    // Fine-Grained Row-by-Row Streaming API
    static bool write_table_header(ChunkedStreamWriter& writer, ResourceKind kind, uint64_t resource_version = 100);
    static bool write_node_row(ChunkedStreamWriter& writer, const models::KubeNode& node, bool is_first = false);
    static bool write_pod_row(ChunkedStreamWriter& writer, const models::KubePod& pod, bool is_first = false);
    static bool write_deployment_row(ChunkedStreamWriter& writer, const models::KubeDeployment& deployment, bool is_first = false);
    static bool write_table_footer(ChunkedStreamWriter& writer);

    // Standard List Streamers (When Accept is NOT as=Table)
    static bool serialize_nodes_list_from_json(ChunkedStreamWriter& writer,
                                               const std::vector<std::string>& node_jsons,
                                               uint64_t resource_version = 100);

    static bool serialize_pods_list_from_json(ChunkedStreamWriter& writer,
                                              const std::vector<std::string>& pod_jsons,
                                              uint64_t resource_version = 100);

    static bool serialize_deployments_list_from_json(ChunkedStreamWriter& writer,
                                                     const std::vector<std::string>& deployment_jsons,
                                                     uint64_t resource_version = 100);

    // In-memory string formatting helpers (for synchronous responses)
    static std::string build_nodes_table_json(const std::vector<std::string>& node_jsons, uint64_t resource_version = 100);
    static std::string build_pods_table_json(const std::vector<std::string>& pod_jsons, uint64_t resource_version = 100);
    static std::string build_deployments_table_json(const std::vector<std::string>& deployment_jsons, uint64_t resource_version = 100);

    static std::string build_nodes_list_json(const std::vector<std::string>& node_jsons, uint64_t resource_version = 100);
    static std::string build_pods_list_json(const std::vector<std::string>& pod_jsons, uint64_t resource_version = 100);
    static std::string build_deployments_list_json(const std::vector<std::string>& deployment_jsons, uint64_t resource_version = 100);

    // Formatting Helpers
    static void format_age(uint64_t duration_sec, char* out_buf, size_t out_buf_len);
};

} // namespace kube32::api
