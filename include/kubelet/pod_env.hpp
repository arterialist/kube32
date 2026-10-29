#pragma once

// Container environment, built the way the Kubernetes kubelet builds it:
//
//   1. Service links, unless spec.enableServiceLinks is false: for every
//      Service in the pod's namespace when the container starts,
//      <NAME>_SERVICE_HOST / <NAME>_SERVICE_PORT[_<PORT NAME>] and the
//      docker-link variables <NAME>_PORT, <NAME>_PORT_<port>_<PROTO>[_PROTO|_PORT|_ADDR].
//      KUBERNETES_SERVICE_HOST/PORT name the API server for every pod.
//   2. envFrom (configMapRef, secretRef, with prefix), in order.
//   3. env, in order: value with $(VAR) references expanded, or valueFrom
//      (configMapKeyRef, secretKeyRef, fieldRef).
//
// Later definitions override earlier ones; service variables only fill names
// nothing else defined.
//
// Service addresses: there is no ClusterIP or service proxy, and a pod's IP is
// the address of the node running it, where the node's pod HTTP server answers
// on the Service's target port. So <NAME>_SERVICE_HOST is the IP of a Running
// pod the Service selects (this node's IP when the starting pod is selected
// itself) and the port variables carry the resolved target ports. A Service
// whose pods are not running yet gets no variables.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace kube32::kubelet {

using EnvVars = std::vector<std::pair<std::string, std::string>>;

struct PodEnvContext {
    // GET <path> on the API server: returns the HTTP status (0 when the server
    // cannot be reached) and fills the body.
    std::function<int(const std::string& path, std::string& body)> api_get;
    std::string node_name;
    std::string node_ip;      // this node: the starting pod's IP and hostIP
    std::string api_host;     // the API server as pods address it
    uint16_t api_port{6443};
};

// Builds the environment of the pod's first container. Returns false with a
// CreateContainerConfigError message when a referenced ConfigMap, Secret or
// key is missing and not optional, when a reference cannot be read, or when a
// valueFrom source is not supported.
bool build_container_env(const std::string& pod_manifest_json, const PodEnvContext& ctx, EnvVars& out, std::string& out_error);

// $(VAR) expansion as in Kubernetes: $$ escapes a $, an unknown or unclosed
// reference stays as written.
std::string expand_env_refs(const std::string& in, const std::function<const std::string*(const std::string&)>& lookup);

// Name of a service link variable: upper case, '-' becomes '_'.
std::string service_env_name(const std::string& name);

} // namespace kube32::kubelet
