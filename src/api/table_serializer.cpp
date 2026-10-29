#include "api/table_serializer.hpp"
#include "osal/thread.hpp"

#include <cstdio>
#include <cstring>
#include <cctype>
#include <sstream>

namespace kube32::api {

void TableSerializer::format_age(uint64_t duration_sec, char* out_buf, size_t out_buf_len) {
    if (out_buf_len == 0) return;
    if (duration_sec == models::AGE_UNKNOWN) {
        snprintf(out_buf, out_buf_len, "<unknown>");
    } else if (duration_sec < 60) {
        snprintf(out_buf, out_buf_len, "%llus", static_cast<unsigned long long>(duration_sec));
    } else if (duration_sec < 3600) {
        snprintf(out_buf, out_buf_len, "%llum", static_cast<unsigned long long>(duration_sec / 60));
    } else if (duration_sec < 86400) {
        snprintf(out_buf, out_buf_len, "%lluh", static_cast<unsigned long long>(duration_sec / 3600));
    } else {
        snprintf(out_buf, out_buf_len, "%llud", static_cast<unsigned long long>(duration_sec / 86400));
    }
}

bool TableSerializer::write_table_header(ChunkedStreamWriter& writer, ResourceKind kind, uint64_t resource_version) {
    if (!writer.write("{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"resourceVersion\":\"")) return false;
    if (!writer.write_uint(resource_version)) return false;
    if (!writer.write("\"},\"columnDefinitions\":[")) return false;

    switch (kind) {
        case ResourceKind::Node:
            if (!writer.write(
                "{\"name\":\"NAME\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"STATUS\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"ROLES\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"AGE\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"VERSION\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"INTERNAL-IP\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"EXTERNAL-IP\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"OS-IMAGE\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"KERNEL-VERSION\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"CONTAINER-RUNTIME\",\"type\":\"string\",\"priority\":1}"
            )) return false;
            break;

        case ResourceKind::Pod:
            if (!writer.write(
                "{\"name\":\"NAME\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"READY\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"STATUS\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"RESTARTS\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"AGE\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"IP\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"NODE\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"NOMINATED NODE\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"READINESS GATES\",\"type\":\"string\",\"priority\":1}"
            )) return false;
            break;

        case ResourceKind::Deployment:
            if (!writer.write(
                "{\"name\":\"NAME\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"READY\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"UP-TO-DATE\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"AVAILABLE\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"AGE\",\"type\":\"string\",\"priority\":0},"
                "{\"name\":\"CONTAINERS\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"IMAGES\",\"type\":\"string\",\"priority\":1},"
                "{\"name\":\"SELECTOR\",\"type\":\"string\",\"priority\":1}"
            )) return false;
            break;
    }

    if (!writer.write("],\"rows\":[")) return false;
    return true;
}

bool TableSerializer::write_node_row(ChunkedStreamWriter& writer, const models::KubeNode& node, bool is_first) {
    if (!is_first && !writer.write(",")) return false;

    if (!writer.write("{\"cells\":[\"")) return false;
    // 0: NAME
    if (!writer.write_escaped_json(node.name.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 1: STATUS
    if (!writer.write(node.is_ready ? "Ready" : "NotReady")) return false;
    if (!writer.write("\",\"")) return false;

    // 2: ROLES
    if (!writer.write(node.roles.empty() ? "<none>" : node.roles.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 3: AGE
    char age_buf[16];
    format_age(node.age_seconds, age_buf, sizeof(age_buf));
    if (!writer.write(age_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 4: VERSION
    if (!writer.write(node.version.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 5: INTERNAL-IP
    if (!writer.write(node.internal_ip.empty() ? "<none>" : node.internal_ip.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 6: EXTERNAL-IP
    if (!writer.write(node.external_ip.empty() ? "<none>" : node.external_ip.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 7: OS-IMAGE
    if (!writer.write(node.os_image.empty() ? "<unknown>" : node.os_image.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 8: KERNEL-VERSION
    if (!writer.write(node.kernel_version.empty() ? "<unknown>" : node.kernel_version.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 9: CONTAINER-RUNTIME
    if (!writer.write(node.container_runtime.empty() ? "<unknown>" : node.container_runtime.c_str())) return false;
    if (!writer.write("\"],\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"")) return false;
    if (!writer.write_escaped_json(node.name.c_str())) return false;
    if (!writer.write("\"}}}")) return false;

    osal::Thread::yield();
    return true;
}

bool TableSerializer::write_pod_row(ChunkedStreamWriter& writer, const models::KubePod& pod, bool is_first) {
    if (!is_first && !writer.write(",")) return false;

    if (!writer.write("{\"cells\":[\"")) return false;
    // 0: NAME
    if (!writer.write_escaped_json(pod.name.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 1: READY
    char ready_buf[16];
    snprintf(ready_buf, sizeof(ready_buf), "%u/%u", pod.ready_containers, pod.total_containers);
    if (!writer.write(ready_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 2: STATUS
    if (!writer.write(pod.status.empty() ? "Pending" : pod.status.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 3: RESTARTS
    char restart_buf[16];
    snprintf(restart_buf, sizeof(restart_buf), "%u", pod.restart_count);
    if (!writer.write(restart_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 4: AGE
    char age_buf[16];
    format_age(pod.age_seconds, age_buf, sizeof(age_buf));
    if (!writer.write(age_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 5: IP
    if (!writer.write(pod.ip.empty() ? "<none>" : pod.ip.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 6: NODE
    if (!writer.write(pod.node_name.empty() ? "<none>" : pod.node_name.c_str())) return false;
    if (!writer.write("\",\"<none>\",\"<none>\"],\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"")) return false;
    if (!writer.write_escaped_json(pod.name.c_str())) return false;
    if (!writer.write("\",\"namespace\":\"")) return false;
    if (!writer.write_escaped_json(pod.namespace_name.c_str())) return false;
    if (!writer.write("\"}}}")) return false;

    osal::Thread::yield();
    return true;
}

bool TableSerializer::write_deployment_row(ChunkedStreamWriter& writer, const models::KubeDeployment& deploy, bool is_first) {
    if (!is_first && !writer.write(",")) return false;

    if (!writer.write("{\"cells\":[\"")) return false;
    // 0: NAME
    if (!writer.write_escaped_json(deploy.name.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 1: READY
    char ready_buf[16];
    snprintf(ready_buf, sizeof(ready_buf), "%u/%u", deploy.ready_replicas, deploy.replicas);
    if (!writer.write(ready_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 2: UP-TO-DATE
    char uptodate_buf[16];
    snprintf(uptodate_buf, sizeof(uptodate_buf), "%u", deploy.updated_replicas);
    if (!writer.write(uptodate_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 3: AVAILABLE
    char avail_buf[16];
    snprintf(avail_buf, sizeof(avail_buf), "%u", deploy.available_replicas);
    if (!writer.write(avail_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 4: AGE
    char age_buf[16];
    format_age(deploy.age_seconds, age_buf, sizeof(age_buf));
    if (!writer.write(age_buf)) return false;
    if (!writer.write("\",\"")) return false;

    // 5: CONTAINERS
    if (!writer.write(deploy.containers.empty() ? "app" : deploy.containers.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 6: IMAGES
    if (!writer.write(deploy.images.empty() ? "wasm://app" : deploy.images.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 7: SELECTOR
    if (!writer.write(deploy.selector.empty() ? "app=test" : deploy.selector.c_str())) return false;
    if (!writer.write("\"],\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"")) return false;
    if (!writer.write_escaped_json(deploy.name.c_str())) return false;
    if (!writer.write("\",\"namespace\":\"")) return false;
    if (!writer.write_escaped_json(deploy.namespace_name.c_str())) return false;
    if (!writer.write("\"}}}")) return false;

    osal::Thread::yield();
    return true;
}

bool TableSerializer::write_table_footer(ChunkedStreamWriter& writer) {
    if (!writer.write("]}")) return false;
    return writer.finish();
}

bool TableSerializer::serialize_nodes_table(ChunkedStreamWriter& writer,
                                           const std::vector<models::KubeNode>& nodes,
                                           uint64_t resource_version) {
    if (!writer.send_headers("application/json;as=Table;v=v1;g=meta.k8s.io")) return false;
    if (!write_table_header(writer, ResourceKind::Node, resource_version)) return false;

    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!write_node_row(writer, nodes[i], i == 0)) return false;
    }

    return write_table_footer(writer);
}

bool TableSerializer::serialize_pods_table(ChunkedStreamWriter& writer,
                                          const std::vector<models::KubePod>& pods,
                                          uint64_t resource_version) {
    if (!writer.send_headers("application/json;as=Table;v=v1;g=meta.k8s.io")) return false;
    if (!write_table_header(writer, ResourceKind::Pod, resource_version)) return false;

    for (size_t i = 0; i < pods.size(); ++i) {
        if (!write_pod_row(writer, pods[i], i == 0)) return false;
    }

    return write_table_footer(writer);
}

bool TableSerializer::serialize_deployments_table(ChunkedStreamWriter& writer,
                                                 const std::vector<models::KubeDeployment>& deployments,
                                                 uint64_t resource_version) {
    if (!writer.send_headers("application/json;as=Table;v=v1;g=meta.k8s.io")) return false;
    if (!write_table_header(writer, ResourceKind::Deployment, resource_version)) return false;

    for (size_t i = 0; i < deployments.size(); ++i) {
        if (!write_deployment_row(writer, deployments[i], i == 0)) return false;
    }

    return write_table_footer(writer);
}

bool TableSerializer::serialize_nodes_table_from_json(ChunkedStreamWriter& writer,
                                                     const std::vector<std::string>& node_jsons,
                                                     uint64_t resource_version) {
    if (!writer.send_headers("application/json;as=Table;v=v1;g=meta.k8s.io")) return false;
    if (!write_table_header(writer, ResourceKind::Node, resource_version)) return false;

    for (size_t i = 0; i < node_jsons.size(); ++i) {
        models::KubeNode node = models::parse_node_manifest(node_jsons[i]);
        if (!write_node_row(writer, node, i == 0)) return false;
    }

    return write_table_footer(writer);
}

bool TableSerializer::serialize_pods_table_from_json(ChunkedStreamWriter& writer,
                                                    const std::vector<std::string>& pod_jsons,
                                                    uint64_t resource_version) {
    if (!writer.send_headers("application/json;as=Table;v=v1;g=meta.k8s.io")) return false;
    if (!write_table_header(writer, ResourceKind::Pod, resource_version)) return false;

    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        models::KubePod pod = models::parse_pod_manifest(pod_jsons[i]);
        if (!write_pod_row(writer, pod, i == 0)) return false;
    }

    return write_table_footer(writer);
}

bool TableSerializer::serialize_deployments_table_from_json(ChunkedStreamWriter& writer,
                                                           const std::vector<std::string>& deployment_jsons,
                                                           uint64_t resource_version) {
    if (!writer.send_headers("application/json;as=Table;v=v1;g=meta.k8s.io")) return false;
    if (!write_table_header(writer, ResourceKind::Deployment, resource_version)) return false;

    for (size_t i = 0; i < deployment_jsons.size(); ++i) {
        models::KubeDeployment deploy = models::parse_deployment_manifest(deployment_jsons[i]);
        if (!write_deployment_row(writer, deploy, i == 0)) return false;
    }

    return write_table_footer(writer);
}

bool TableSerializer::serialize_nodes_list_from_json(ChunkedStreamWriter& writer,
                                                    const std::vector<std::string>& node_jsons,
                                                    uint64_t resource_version) {
    if (!writer.send_headers("application/json")) return false;
    if (!writer.write("{\"kind\":\"NodeList\",\"apiVersion\":\"v1\",\"metadata\":{\"resourceVersion\":\"")) return false;
    if (!writer.write_uint(resource_version)) return false;
    if (!writer.write("\"},\"items\":[")) return false;

    for (size_t i = 0; i < node_jsons.size(); ++i) {
        if (i > 0 && !writer.write(",")) return false;
        if (!writer.write(node_jsons[i].c_str(), node_jsons[i].size())) return false;
        osal::Thread::yield();
    }

    if (!writer.write("]}")) return false;
    return writer.finish();
}

bool TableSerializer::serialize_pods_list_from_json(ChunkedStreamWriter& writer,
                                                   const std::vector<std::string>& pod_jsons,
                                                   uint64_t resource_version) {
    if (!writer.send_headers("application/json")) return false;
    if (!writer.write("{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{\"resourceVersion\":\"")) return false;
    if (!writer.write_uint(resource_version)) return false;
    if (!writer.write("\"},\"items\":[")) return false;

    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        if (i > 0 && !writer.write(",")) return false;
        if (!writer.write(pod_jsons[i].c_str(), pod_jsons[i].size())) return false;
        osal::Thread::yield();
    }

    if (!writer.write("]}")) return false;
    return writer.finish();
}

bool TableSerializer::serialize_deployments_list_from_json(ChunkedStreamWriter& writer,
                                                          const std::vector<std::string>& deployment_jsons,
                                                          uint64_t resource_version) {
    if (!writer.send_headers("application/json")) return false;
    if (!writer.write("{\"kind\":\"DeploymentList\",\"apiVersion\":\"apps/v1\",\"metadata\":{\"resourceVersion\":\"")) return false;
    if (!writer.write_uint(resource_version)) return false;
    if (!writer.write("\"},\"items\":[")) return false;

    for (size_t i = 0; i < deployment_jsons.size(); ++i) {
        if (i > 0 && !writer.write(",")) return false;
        if (!writer.write(deployment_jsons[i].c_str(), deployment_jsons[i].size())) return false;
        osal::Thread::yield();
    }

    if (!writer.write("]}")) return false;
    return writer.finish();
}

// In-memory string sinks for synchronous responses
class StringSink : public ISink {
public:
    explicit StringSink(std::string& str) : str_(str) {}
    bool write(const void* data, size_t len) override {
        str_.append(static_cast<const char*>(data), len);
        return true;
    }
private:
    std::string& str_;
};

std::string TableSerializer::build_nodes_table_json(const std::vector<std::string>& node_jsons, uint64_t resource_version) {
    std::string clean = "{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"resourceVersion\":\"" + 
                        std::to_string(resource_version) + "\"},\"columnDefinitions\":["
                        "{\"name\":\"NAME\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"STATUS\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"ROLES\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"AGE\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"VERSION\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"INTERNAL-IP\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"EXTERNAL-IP\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"OS-IMAGE\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"KERNEL-VERSION\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"CONTAINER-RUNTIME\",\"type\":\"string\",\"priority\":1}"
                        "],\"rows\":[";
    for (size_t i = 0; i < node_jsons.size(); ++i) {
        if (i > 0) clean += ",";
        models::KubeNode node = models::parse_node_manifest(node_jsons[i]);
        char age_buf[16];
        format_age(node.age_seconds, age_buf, sizeof(age_buf));
        clean += "{\"cells\":[\"" + node.name + "\",\"" + (node.is_ready ? "Ready" : "NotReady") + "\",\"" +
                 (node.roles.empty() ? "worker" : node.roles) + "\",\"" + age_buf + "\",\"" +
                 node.version + "\",\"" +
                 (node.internal_ip.empty() ? "<none>" : node.internal_ip) + "\",\"" +
                 (node.external_ip.empty() ? "<none>" : node.external_ip) + "\",\"" +
                 (node.os_image.empty() ? "<unknown>" : node.os_image) + "\",\"" +
                 (node.kernel_version.empty() ? "<unknown>" : node.kernel_version) + "\",\"" +
                 (node.container_runtime.empty() ? "<unknown>" : node.container_runtime) + "\"],"
                 "\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"" +
                 node.name + "\"}}}";
    }
    clean += "]}";
    return clean;
}

std::string TableSerializer::build_pods_table_json(const std::vector<std::string>& pod_jsons, uint64_t resource_version) {
    std::string clean = "{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"resourceVersion\":\"" +
                        std::to_string(resource_version) + "\"},\"columnDefinitions\":["
                        "{\"name\":\"NAME\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"READY\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"STATUS\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"RESTARTS\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"AGE\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"IP\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"NODE\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"NOMINATED NODE\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"READINESS GATES\",\"type\":\"string\",\"priority\":1}"
                        "],\"rows\":[";
    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        if (i > 0) clean += ",";
        models::KubePod pod = models::parse_pod_manifest(pod_jsons[i]);
        char age_buf[16];
        format_age(pod.age_seconds, age_buf, sizeof(age_buf));
        char ready_buf[16];
        snprintf(ready_buf, sizeof(ready_buf), "%u/%u", pod.ready_containers, pod.total_containers);
        clean += "{\"cells\":[\"" + pod.name + "\",\"" + ready_buf + "\",\"" +
                 pod.status + "\",\"" + std::to_string(pod.restart_count) + "\",\"" +
                 age_buf + "\",\"" + (pod.ip.empty() ? "<none>" : pod.ip) + "\",\"" +
                 (pod.node_name.empty() ? "<none>" : pod.node_name) + "\",\"<none>\",\"<none>\"],"
                 "\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"" +
                 pod.name + "\",\"namespace\":\"" + pod.namespace_name + "\"}}}";
    }
    clean += "]}";
    return clean;
}

std::string TableSerializer::build_deployments_table_json(const std::vector<std::string>& deployment_jsons, uint64_t resource_version) {
    std::string clean = "{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"resourceVersion\":\"" +
                        std::to_string(resource_version) + "\"},\"columnDefinitions\":["
                        "{\"name\":\"NAME\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"READY\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"UP-TO-DATE\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"AVAILABLE\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"AGE\",\"type\":\"string\",\"priority\":0},"
                        "{\"name\":\"CONTAINERS\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"IMAGES\",\"type\":\"string\",\"priority\":1},"
                        "{\"name\":\"SELECTOR\",\"type\":\"string\",\"priority\":1}"
                        "],\"rows\":[";
    for (size_t i = 0; i < deployment_jsons.size(); ++i) {
        if (i > 0) clean += ",";
        models::KubeDeployment deploy = models::parse_deployment_manifest(deployment_jsons[i]);
        char age_buf[16];
        format_age(deploy.age_seconds, age_buf, sizeof(age_buf));
        char ready_buf[16];
        snprintf(ready_buf, sizeof(ready_buf), "%u/%u", deploy.ready_replicas, deploy.replicas);
        clean += "{\"cells\":[\"" + deploy.name + "\",\"" + ready_buf + "\",\"" +
                 std::to_string(deploy.updated_replicas) + "\",\"" +
                 std::to_string(deploy.available_replicas) + "\",\"" +
                 age_buf + "\",\"" + deploy.containers + "\",\"" +
                 deploy.images + "\",\"" + deploy.selector + "\"],"
                 "\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"" +
                 deploy.name + "\",\"namespace\":\"" + deploy.namespace_name + "\"}}}";
    }
    clean += "]}";
    return clean;
}

std::string TableSerializer::build_nodes_list_json(const std::vector<std::string>& node_jsons, uint64_t resource_version) {
    std::string out = "{\"kind\":\"NodeList\",\"apiVersion\":\"v1\",\"metadata\":{\"resourceVersion\":\"" +
                      std::to_string(resource_version) + "\"},\"items\":[";
    for (size_t i = 0; i < node_jsons.size(); ++i) {
        if (i > 0) out += ",";
        out += node_jsons[i];
    }
    out += "]}";
    return out;
}

std::string TableSerializer::build_pods_list_json(const std::vector<std::string>& pod_jsons, uint64_t resource_version) {
    std::string out = "{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{\"resourceVersion\":\"" +
                      std::to_string(resource_version) + "\"},\"items\":[";
    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        if (i > 0) out += ",";
        out += pod_jsons[i];
    }
    out += "]}";
    return out;
}

std::string TableSerializer::build_deployments_list_json(const std::vector<std::string>& deployment_jsons, uint64_t resource_version) {
    std::string out = "{\"kind\":\"DeploymentList\",\"apiVersion\":\"apps/v1\",\"metadata\":{\"resourceVersion\":\"" +
                      std::to_string(resource_version) + "\"},\"items\":[";
    for (size_t i = 0; i < deployment_jsons.size(); ++i) {
        if (i > 0) out += ",";
        out += deployment_jsons[i];
    }
    out += "]}";
    return out;
}


// =============================================================================
// Generic Tables (Event, Namespace, Service, ConfigMap, Secret, Lease, Ingress)
// =============================================================================
std::string TableSerializer::format_age(uint64_t duration_sec) {
    char buf[16];
    format_age(duration_sec, buf, sizeof(buf));
    return buf;
}

static std::string json_cell(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned char>(c)); out += b; }
                else out += c;
        }
    }
    out += "\"";
    return out;
}

static std::string col(const char* name, int priority) {
    return std::string("{\"name\":\"") + name + "\",\"type\":\"string\",\"priority\":" + std::to_string(priority) + "}";
}

bool TableSerializer::has_generic_table(std::string_view kind) {
    return kind == "Event" || kind == "Namespace" || kind == "Service" || kind == "ConfigMap" || kind == "Secret" || kind == "Lease" ||
           kind == "Ingress" || kind == "Endpoints" || kind == "EndpointSlice";
}

std::string TableSerializer::build_generic_table_json(std::string_view kind, const std::vector<std::string>& jsons, uint64_t resource_version) {
    std::string out = "{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"resourceVersion\":\"" +
                      std::to_string(resource_version) + "\"},\"columnDefinitions\":[";
    if (kind == "Event") {
        out += col("LAST SEEN", 0) + "," + col("TYPE", 0) + "," + col("REASON", 0) + "," + col("OBJECT", 0) + "," +
               col("SUBOBJECT", 1) + "," + col("SOURCE", 1) + "," + col("MESSAGE", 0) + "," + col("FIRST SEEN", 1) + "," +
               col("COUNT", 1) + "," + col("NAME", 1);
    } else if (kind == "Namespace") {
        out += col("NAME", 0) + "," + col("STATUS", 0) + "," + col("AGE", 0);
    } else if (kind == "Service") {
        out += col("NAME", 0) + "," + col("TYPE", 0) + "," + col("CLUSTER-IP", 0) + "," + col("EXTERNAL-IP", 0) + "," +
               col("PORT(S)", 0) + "," + col("AGE", 0) + "," + col("SELECTOR", 1);
    } else if (kind == "ConfigMap") {
        out += col("NAME", 0) + "," + col("DATA", 0) + "," + col("AGE", 0);
    } else if (kind == "Secret") {
        out += col("NAME", 0) + "," + col("TYPE", 0) + "," + col("DATA", 0) + "," + col("AGE", 0);
    } else if (kind == "Lease") {
        out += col("NAME", 0) + "," + col("HOLDER", 0) + "," + col("AGE", 0);
    } else if (kind == "Ingress") {
        out += col("NAME", 0) + "," + col("CLASS", 0) + "," + col("HOSTS", 0) + "," + col("ADDRESS", 0) + "," +
               col("PORTS", 0) + "," + col("AGE", 0);
    } else if (kind == "Endpoints") {
        out += col("NAME", 0) + "," + col("ENDPOINTS", 0) + "," + col("AGE", 0);
    } else if (kind == "EndpointSlice") {
        out += col("NAME", 0) + "," + col("ADDRESSTYPE", 0) + "," + col("PORTS", 0) + "," + col("ENDPOINTS", 0) + "," + col("AGE", 0);
    } else {
        out += col("NAME", 0) + "," + col("AGE", 0);
    }
    out += "],\"rows\":[";

    for (size_t i = 0; i < jsons.size(); ++i) {
        models::JsonValue doc;
        std::string err;
        if (!models::parse_json(jsons[i], doc, err) || !doc.is_object()) continue;
        if (i > 0) out += ",";
        const models::JsonValue* meta = doc.get("metadata");
        std::string name = meta ? meta->get_string("name", "") : "";
        std::string ns = meta ? meta->get_string("namespace", "") : "";
        std::string age = format_age(models::age_seconds_since(meta ? meta->get_string("creationTimestamp", "") : ""));
        std::vector<std::string> cells;

        if (kind == "Event") {
            const models::JsonValue* inv = doc.get("involvedObject");
            const models::JsonValue* src = doc.get("source");
            std::string last = doc.get_string("lastTimestamp", doc.get_string("eventTime", ""));
            std::string first = doc.get_string("firstTimestamp", last);
            std::string object = inv ? (inv->get_string("kind", "") + "/" + inv->get_string("name", "")) : "";
            std::string kind_lower = object;
            for (auto& c : kind_lower) { if (c == '/') break; c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
            std::string source = doc.get_string("reportingComponent", src ? src->get_string("component", "") : "");
            std::string host = doc.get_string("reportingInstance", src ? src->get_string("host", "") : "");
            if (!host.empty()) source += ", " + host;
            cells = {format_age(models::age_seconds_since(last)), doc.get_string("type", ""), doc.get_string("reason", ""), kind_lower,
                     inv ? inv->get_string("fieldPath", "") : "", source, doc.get_string("message", ""),
                     format_age(models::age_seconds_since(first)), std::to_string(doc.get_int("count", 1)), name};
        } else if (kind == "Namespace") {
            const models::JsonValue* st = doc.get("status");
            cells = {name, st ? st->get_string("phase", "") : "", age};
        } else if (kind == "Service") {
            const models::JsonValue* spec = doc.get("spec");
            std::string type = spec ? spec->get_string("type", "ClusterIP") : "ClusterIP";
            std::string cluster_ip = spec ? spec->get_string("clusterIP", "") : "";
            if (cluster_ip.empty()) cluster_ip = "<none>";
            std::string ports;
            std::string selector;
            if (spec) {
                const models::JsonValue* pl = spec->get("ports");
                if (pl && pl->is_array()) {
                    for (const auto& p : pl->arr_val) {
                        if (!ports.empty()) ports += ",";
                        ports += std::to_string(p.get_int("port", 0));
                        int64_t np = p.get_int("nodePort", 0);
                        if (np) ports += ":" + std::to_string(np);
                        ports += "/" + p.get_string("protocol", "TCP");
                    }
                }
                const models::JsonValue* sel = spec->get("selector");
                if (sel && sel->is_object()) {
                    for (const auto& kv : sel->obj_val) {
                        if (!selector.empty()) selector += ",";
                        selector += kv.first + "=" + (kv.second.is_string() ? kv.second.str_val : "");
                    }
                }
            }
            cells = {name, type, cluster_ip, "<none>", ports.empty() ? "<none>" : ports, age, selector.empty() ? "<none>" : selector};
        } else if (kind == "ConfigMap" || kind == "Secret") {
            size_t n = 0;
            const models::JsonValue* data = doc.get("data");
            if (data && data->is_object()) n += data->obj_val.size();
            const models::JsonValue* bdata = doc.get("binaryData");
            if (bdata && bdata->is_object()) n += bdata->obj_val.size();
            const models::JsonValue* sdata = doc.get("stringData");
            if (sdata && sdata->is_object()) n += sdata->obj_val.size();
            if (kind == "Secret") cells = {name, doc.get_string("type", "Opaque"), std::to_string(n), age};
            else cells = {name, std::to_string(n), age};
        } else if (kind == "Lease") {
            const models::JsonValue* spec = doc.get("spec");
            cells = {name, spec ? spec->get_string("holderIdentity", "") : "", age};
        } else if (kind == "Ingress") {
            // Same cells as kubectl's printer: hosts of the rules ("*" for a
            // rule without one), status.loadBalancer addresses, 80 or 80, 443 with TLS
            const models::JsonValue* spec = doc.get("spec");
            std::string cls = spec ? spec->get_string("ingressClassName", "") : "";
            std::string hosts;
            bool tls = false;
            if (spec) {
                const models::JsonValue* rules = spec->get("rules");
                if (rules && rules->is_array()) {
                    for (const auto& r : rules->arr_val) {
                        std::string h = r.get_string("host", "");
                        if (h.empty()) h = "*";
                        if (!hosts.empty()) hosts += ",";
                        hosts += h;
                    }
                }
                const models::JsonValue* t = spec->get("tls");
                tls = t && t->is_array() && !t->arr_val.empty();
            }
            std::string address;
            if (const models::JsonValue* st = doc.get("status")) {
                const models::JsonValue* lb = st->get("loadBalancer");
                const models::JsonValue* list = lb ? lb->get("ingress") : nullptr;
                if (list && list->is_array()) {
                    for (const auto& a : list->arr_val) {
                        std::string v = a.get_string("ip", a.get_string("hostname", ""));
                        if (v.empty()) continue;
                        if (!address.empty()) address += ",";
                        address += v;
                    }
                }
            }
            cells = {name, cls.empty() ? "<none>" : cls, hosts.empty() ? "*" : hosts, address, tls ? "80, 443" : "80", age};
        } else if (kind == "Endpoints" || kind == "EndpointSlice") {
            // kubectl's printers: the first three endpoints, then "+ N more..."
            std::vector<std::string> eps;
            std::string ports;
            if (kind == "Endpoints") {
                if (const models::JsonValue* subsets = doc.get("subsets"); subsets && subsets->is_array()) {
                    for (const auto& sub : subsets->arr_val) {
                        const models::JsonValue* addrs = sub.get("addresses");
                        const models::JsonValue* pl = sub.get("ports");
                        if (!addrs || !addrs->is_array()) continue;
                        for (const auto& a : addrs->arr_val) {
                            if (pl && pl->is_array() && !pl->arr_val.empty()) {
                                for (const auto& p : pl->arr_val) eps.push_back(a.get_string("ip", "") + ":" + std::to_string(p.get_int("port", 0)));
                            } else {
                                eps.push_back(a.get_string("ip", ""));
                            }
                        }
                    }
                }
            } else {
                if (const models::JsonValue* pl = doc.get("ports"); pl && pl->is_array()) {
                    for (const auto& p : pl->arr_val) {
                        if (!ports.empty()) ports += ",";
                        ports += std::to_string(p.get_int("port", 0));
                    }
                }
                if (const models::JsonValue* el = doc.get("endpoints"); el && el->is_array()) {
                    for (const auto& e : el->arr_val) {
                        const models::JsonValue* addrs = e.get("addresses");
                        if (addrs && addrs->is_array()) for (const auto& a : addrs->arr_val) if (a.is_string()) eps.push_back(a.str_val);
                    }
                }
            }
            std::string list;
            for (size_t k = 0; k < eps.size() && k < 3; ++k) list += (k ? "," : "") + eps[k];
            if (eps.size() > 3) list += " + " + std::to_string(eps.size() - 3) + " more...";
            if (list.empty()) list = "<none>";
            if (kind == "Endpoints") cells = {name, list, age};
            else cells = {name, doc.get_string("addressType", ""), ports.empty() ? "<unset>" : ports, list, age};
        } else {
            cells = {name, age};
        }

        out += "{\"cells\":[";
        for (size_t c = 0; c < cells.size(); ++c) {
            if (c) out += ",";
            out += json_cell(cells[c]);
        }
        out += "],\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":" + json_cell(name);
        if (!ns.empty()) out += ",\"namespace\":" + json_cell(ns);
        out += "}}}";
    }
    out += "]}";
    return out;
}

} // namespace kube32::api
