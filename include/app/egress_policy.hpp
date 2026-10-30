#pragma once

// NetworkPolicy egress for k32_http_call: the policies that restrict a
// calling pod, and the pods it calls as policy peers see them (the rules and
// their evaluation are in net/network_policy.hpp).
//
// On the control plane both come from the object store. On a worker the
// policies of the namespaces that have pods on the node, and the labels of
// those pods, come from net::NodeNetworkPolicies, which the kubelet keeps
// current with its pod sync; a target pod of an in-cluster call is read from
// the API server when a rule compares its labels or port names, and kept a
// few seconds.

#include "net/network_policy.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kube32::storage { class Store; }

namespace kube32::app {

struct EgressScope {
    std::shared_ptr<const net::NamespacePolicies> policies;   // of the caller's namespace
    std::vector<const net::NetworkPolicyRules*> selecting;    // those restricting the caller (into *policies); none: unrestricted
};

class EgressPolicySource {
public:
    virtual ~EgressPolicySource() = default;
    // The policies that restrict pod `pod` ("<ns>/<name>"). False with `why`
    // when that cannot be told yet: the call is then denied.
    virtual bool scope(std::string_view pod, EgressScope& out, std::string& why) = 0;
    // Labels and named ports of pod "<ns>/<name>", with its namespace's labels
    virtual bool peer(const std::string& pod, net::PeerPod& out) = 0;
};

// From the object store (control plane); compiled policies and pods are kept
// until a NetworkPolicy, Pod or Namespace changes.
std::unique_ptr<EgressPolicySource> make_store_egress_source(storage::Store& store);
// Workers: policies and the callers' labels from `policies`, target pods
// from the API server (`api_get(path, body)` returns the HTTP status), kept
// for `ttl_ms`.
std::unique_ptr<EgressPolicySource> make_node_egress_source(std::shared_ptr<net::NodeNetworkPolicies> policies,
                                                            std::function<int(const std::string&, std::string&)> api_get,
                                                            uint32_t ttl_ms = 3000);

} // namespace kube32::app
