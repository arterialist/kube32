#include "api/table_serializer.hpp"
#include "osal/thread.hpp"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace kube32::api {

void TableSerializer::format_age(uint64_t duration_sec, char* out_buf, size_t out_buf_len) {
    if (out_buf_len == 0) return;
    if (duration_sec < 60) {
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
    if (!writer.write(node.version.empty() ? "v1.33.0-kube32" : node.version.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 5: INTERNAL-IP
    if (!writer.write(node.internal_ip.empty() ? "127.0.0.1" : node.internal_ip.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 6: EXTERNAL-IP
    if (!writer.write(node.external_ip.empty() ? "<none>" : node.external_ip.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 7: OS-IMAGE
    if (!writer.write(node.os_image.empty() ? "ESP-IDF v5.2" : node.os_image.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 8: KERNEL-VERSION
    if (!writer.write(node.kernel_version.empty() ? "FreeRTOS-v10.4" : node.kernel_version.c_str())) return false;
    if (!writer.write("\",\"")) return false;

    // 9: CONTAINER-RUNTIME
    if (!writer.write(node.container_runtime.empty() ? "kube32-runtime://0.1.0" : node.container_runtime.c_str())) return false;
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
    if (!writer.write("{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[")) return false;

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
    if (!writer.write("{\"kind\":\"DeploymentList\",\"apiVersion\":\"apps/v1\",\"metadata\":{},\"items\":[")) return false;

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
                 (node.version.empty() ? "v1.33.0-kube32" : node.version) + "\",\"" +
                 (node.internal_ip.empty() ? "127.0.0.1" : node.internal_ip) + "\",\"" +
                 (node.external_ip.empty() ? "<none>" : node.external_ip) + "\",\"" +
                 (node.os_image.empty() ? "ESP-IDF v5.2" : node.os_image) + "\",\"" +
                 (node.kernel_version.empty() ? "FreeRTOS-v10.4" : node.kernel_version) + "\",\"" +
                 (node.container_runtime.empty() ? "kube32-runtime://0.1.0" : node.container_runtime) + "\"],"
                 "\"object\":{\"kind\":\"PartialObjectMetadata\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{\"name\":\"" +
                 node.name + "\"}}}";
    }
    clean += "]}";
    return clean;
}

std::string TableSerializer::build_pods_table_json(const std::vector<std::string>& pod_jsons, uint64_t resource_version) {
    std::string clean = "{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{},\"columnDefinitions\":["
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
    std::string clean = "{\"kind\":\"Table\",\"apiVersion\":\"meta.k8s.io/v1\",\"metadata\":{},\"columnDefinitions\":["
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
    std::string out = "{\"kind\":\"PodList\",\"apiVersion\":\"v1\",\"metadata\":{},\"items\":[";
    for (size_t i = 0; i < pod_jsons.size(); ++i) {
        if (i > 0) out += ",";
        out += pod_jsons[i];
    }
    out += "]}";
    return out;
}

std::string TableSerializer::build_deployments_list_json(const std::vector<std::string>& deployment_jsons, uint64_t resource_version) {
    std::string out = "{\"kind\":\"DeploymentList\",\"apiVersion\":\"apps/v1\",\"metadata\":{},\"items\":[";
    for (size_t i = 0; i < deployment_jsons.size(); ++i) {
        if (i > 0) out += ",";
        out += deployment_jsons[i];
    }
    out += "]}";
    return out;
}

} // namespace kube32::api
